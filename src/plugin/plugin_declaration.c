#include "plugin/plugins_internal.h"

#include "plugin/plugins.h"

#include "config/config_lua.h"
#include "lua/lua_sandbox.h"
#include "lua/lua_verbs.h"
#include "lua/luax.h"
#include "plugin/plugin_modules.h"
#include "util/error.h"

#include "lauxlib.h"

#include <stdlib.h>
#include <string.h>

/* Raised to stop the chunk the moment daukle.plugin has been read, so catching it
   distinguishes a declaration that was read from a plugin that really failed. */
#define FR_PLUGIN_DECLARATION_READ "daukle: plugin declaration read"

static fr_plugin_declaration *declaration_in_progress;

void fr_plugins_free_declaration(fr_plugin_declaration *declaration) {
    for (size_t index = 0; index < declaration->uses_count; index++) {
        free(declaration->uses[index]);
    }
    declaration->uses_count = 0;
    for (size_t index = 0; index < declaration->exports_count; index++) {
        free(declaration->exports[index]);
    }
    declaration->exports_count = 0;
    for (size_t index = 0; index < declaration->requires_count; index++) {
        free(declaration->requires[index].alias);
        free(declaration->requires[index].url);
        free(declaration->requires[index].sha256);
    }
    declaration->requires_count = 0;
}

static int record_uses(lua_State *state, fr_plugin_declaration *declaration) {
    if (lua_isnil(state, -1)) return 0;
    if (!lua_istable(state, -1)) {
        return luaL_error(state, "%s \"%s\": uses must be a list of verb names",
                          declaration->kind, declaration->label);
    }

    lua_Integer length = (lua_Integer) lua_rawlen(state, -1);
    for (lua_Integer index = 1; index <= length; index++) {
        lua_rawgeti(state, -1, index);
        if (lua_type(state, -1) != LUA_TSTRING) {
            return luaL_error(state, "%s \"%s\": every name in uses must be a string",
                              declaration->kind, declaration->label);
        }
        const char *name = lua_tostring(state, -1);
        if (!fr_lua_verbs_is_known(name)) {
            return luaL_error(state, "%s \"%s\": \"%s\" is not a daukle verb",
                              declaration->kind, declaration->label, name);
        }
        if (declaration->uses_count == FR_PLUGIN_MAX_USES) {
            return luaL_error(state, "%s \"%s\": uses names more than %d verbs",
                              declaration->kind, declaration->label, FR_PLUGIN_MAX_USES);
        }
        char *copy = fr_dup_string(name);
        if (copy == NULL) {
            return luaL_error(state, "%s \"%s\": out of memory reading uses",
                              declaration->kind, declaration->label);
        }
        declaration->uses[declaration->uses_count] = copy;
        declaration->uses_count++;
        lua_pop(state, 1);
    }
    return 0;
}

static int record_exports(lua_State *state, fr_plugin_declaration *declaration) {
    if (lua_isnil(state, -1)) return 0;
    if (strcmp(declaration->kind, "resolver") == 0) {
        return luaL_error(state, "resolver \"%s\": a resolver may not export a module, because a"
                                 " resolver has no dependents",
                          declaration->label);
    }
    if (!lua_istable(state, -1)) {
        return luaL_error(state, "%s \"%s\": exports must be a list of module names",
                          declaration->kind, declaration->label);
    }

    lua_Integer length = (lua_Integer) lua_rawlen(state, -1);
    for (lua_Integer index = 1; index <= length; index++) {
        lua_rawgeti(state, -1, index);
        if (lua_type(state, -1) != LUA_TSTRING) {
            return luaL_error(state, "%s \"%s\": every name in exports must be a string",
                              declaration->kind, declaration->label);
        }
        const char *name = lua_tostring(state, -1);
        fr_error name_err;
        if (fr_plugin_export_name_check(name, &name_err) != FR_OK) {
            return luaL_error(state, "%s \"%s\": %s", declaration->kind, declaration->label,
                              name_err.message);
        }
        if (declaration->exports_count == FR_PLUGIN_MAX_EXPORTS) {
            return luaL_error(state, "%s \"%s\": exports names more than %d modules",
                              declaration->kind, declaration->label, FR_PLUGIN_MAX_EXPORTS);
        }
        char *copy = fr_dup_string(name);
        if (copy == NULL) {
            return luaL_error(state, "%s \"%s\": out of memory reading exports",
                              declaration->kind, declaration->label);
        }
        declaration->exports[declaration->exports_count] = copy;
        declaration->exports_count++;
        lua_pop(state, 1);
    }
    return 0;
}

static int alias_is_well_formed(const char *alias) {
    for (const char *scan = alias; *scan != '\0'; scan++) {
        int ok = (*scan >= 'a' && *scan <= 'z') || (*scan >= 'A' && *scan <= 'Z')
              || (*scan >= '0' && *scan <= '9') || *scan == '-' || *scan == '_';
        if (!ok) return 0;
    }
    return alias[0] != '\0';
}

/* A one-character alias is the Windows drive-letter shape fr_lua_sandbox_climbs_out refuses before
   a require ever reaches the colon split ("c:x" is caught as an escape, never read as "plugin c,
   module x"), so daukle.require("<alias>:...") could never name a dependency declared under one.
   The acquisition spec records the identical hazard for a one-letter resolver label. */
static int alias_is_unreachable_as_a_drive_letter(const char *alias) {
    return strlen(alias) == 1;
}

/* Reads table[key] raw, the way config_lua.c's raw_getfield does: index may be
   relative (record_requires calls these with -1), so it is converted to
   absolute before the key is pushed, or the push would shift what the
   caller's index means. */
static int raw_has_field(lua_State *state, int index, const char *key) {
    int absolute = lua_absindex(state, index);
    lua_pushstring(state, key);
    lua_rawget(state, absolute);
    int present = !lua_isnil(state, -1);
    lua_pop(state, 1);
    return present;
}

static const char *raw_string_field(lua_State *state, int index, const char *key) {
    int absolute = lua_absindex(state, index);
    lua_pushstring(state, key);
    lua_rawget(state, absolute);
    const char *value = lua_type(state, -1) == LUA_TSTRING ? lua_tostring(state, -1) : NULL;
    lua_pop(state, 1);
    return value;
}

static int record_requires(lua_State *state, fr_plugin_declaration *declaration) {
    if (lua_isnil(state, -1)) return 0;
    if (strcmp(declaration->kind, "resolver") == 0) {
        return luaL_error(state, "resolver \"%s\": a resolver may not require a plugin, because a"
                                 " resolver is acquired by the floor only",
                          declaration->label);
    }
    if (!lua_istable(state, -1)) {
        return luaL_error(state, "plugin \"%s\": requires must be a table of alias to artifact",
                          declaration->label);
    }

    lua_pushnil(state);
    while (lua_next(state, -2) != 0) {
        if (lua_type(state, -2) != LUA_TSTRING) {
            return luaL_error(state, "plugin \"%s\": every key in requires must be an alias",
                              declaration->label);
        }
        const char *alias = lua_tostring(state, -2);
        if (!alias_is_well_formed(alias)) {
            return luaL_error(state, "plugin \"%s\": the alias \"%s\" may hold only letters, digits,"
                                     " \"-\" and \"_\"",
                              declaration->label, alias);
        }
        if (alias_is_unreachable_as_a_drive_letter(alias)) {
            return luaL_error(state, "plugin \"%s\": the alias \"%s\" is one letter, and a letter"
                                     " before \":\" is a Windows drive letter, so"
                                     " daukle.require(\"%s:...\") could never reach it. Use a longer"
                                     " alias",
                              declaration->label, alias, alias);
        }
        if (strlen(alias) > FR_PLUGIN_MAX_ALIAS) {
            return luaL_error(state, "plugin \"%s\": the alias \"%s\" is longer than %d bytes",
                              declaration->label, alias, FR_PLUGIN_MAX_ALIAS);
        }
        if (declaration->requires_count == FR_PLUGIN_MAX_REQUIRES) {
            return luaL_error(state, "plugin \"%s\": requires names more than %d plugins",
                              declaration->label, FR_PLUGIN_MAX_REQUIRES);
        }
        if (!lua_istable(state, -1)) {
            return luaL_error(state, "plugin \"%s\": requires[\"%s\"] must be a table holding url"
                                     " and sha256",
                              declaration->label, alias);
        }
        if (raw_has_field(state, -1, "path")) {
            return luaL_error(state, "plugin \"%s\": requires[\"%s\"] names a path, and a dependency"
                                     " is acquired by url. A local one is written in the manifest as"
                                     " [plugins.%s].requires",
                              declaration->label, alias, declaration->label);
        }
        const char *url = raw_string_field(state, -1, "url");
        const char *sha256 = raw_string_field(state, -1, "sha256");
        if (url == NULL) {
            return luaL_error(state, "plugin \"%s\": requires[\"%s\"] names no url",
                              declaration->label, alias);
        }
        if (sha256 == NULL) {
            return luaL_error(state, "plugin \"%s\": requires[\"%s\"] has no sha256, and a"
                                     " dependency is always pinned: a pin that is not transitive"
                                     " pins nothing",
                              declaration->label, alias);
        }
        char *alias_copy = fr_dup_string(alias);
        char *url_copy = fr_dup_string(url);
        char *sha256_copy = fr_dup_string(sha256);
        if (alias_copy == NULL || url_copy == NULL || sha256_copy == NULL) {
            free(alias_copy);
            free(url_copy);
            free(sha256_copy);
            return luaL_error(state, "plugin \"%s\": out of memory reading requires",
                              declaration->label);
        }
        fr_plugin_requirement *slot = &declaration->requires[declaration->requires_count];
        slot->alias = alias_copy;
        slot->url = url_copy;
        slot->sha256 = sha256_copy;
        declaration->requires_count++;
        lua_pop(state, 1);
    }
    return 0;
}

static int declare_plugin(lua_State *state) {
    fr_plugin_declaration *declaration = declaration_in_progress;
    if (declaration == NULL) return 0;
    luaL_checktype(state, 1, LUA_TTABLE);

    lua_getfield(state, 1, "api");
    if (!lua_isnil(state, -1)) {
        int is_integer = 0;
        lua_Integer api = lua_tointegerx(state, -1, &is_integer);
        if (!is_integer) {
            return luaL_error(state, "%s \"%s\": api must be a whole number",
                              declaration->kind, declaration->label);
        }
        if (api != 1) {
            return luaL_error(state, "%s \"%s\": needs daukle api %I, this daukle provides 1",
                              declaration->kind, declaration->label, (LUAI_UACINT) api);
        }
    }
    lua_pop(state, 1);

    lua_getfield(state, 1, "uses");
    record_uses(state, declaration);
    lua_pop(state, 1);

    lua_getfield(state, 1, "requires");
    record_requires(state, declaration);
    lua_pop(state, 1);

    lua_getfield(state, 1, "exports");
    record_exports(state, declaration);
    lua_pop(state, 1);

    lua_pushliteral(state, FR_PLUGIN_DECLARATION_READ);
    return lua_error(state);
}

static int require_declaration_first(lua_State *state) {
    const fr_plugin_declaration *declaration = declaration_in_progress;
    return luaL_error(state, "daukle.plugin must be the first call in \"%s\"",
                      declaration != NULL ? declaration->label : "?");
}

static int protected_declaration_env(lua_State *state) {
    lua_newtable(state);
    lua_pushcfunction(state, declare_plugin);
    lua_setfield(state, -2, "plugin");

    lua_newtable(state);
    lua_pushcfunction(state, require_declaration_first);
    lua_setfield(state, -2, "__index");
    lua_setmetatable(state, -2);

    lua_setfield(state, 1, "daukle");
    return 0;
}

int fr_plugins_declaration_read(lua_State *state, const char *text, size_t length,
                                const char *origin, fr_plugin_declaration *declaration,
                                fr_error *err) {
    int top = lua_gettop(state);
    if (fr_lua_verbs_push_env(state, NULL, 0, err) != FR_OK) return FR_ERR;
    int env = lua_gettop(state);

    lua_pushcfunction(state, protected_declaration_env);
    lua_pushvalue(state, env);
    if (lua_pcall(state, 1, 0, 0) != LUA_OK) {
        fr_error_set(err, "%s", fr_lua_error_text(state));
        lua_settop(state, top);
        return FR_ERR;
    }

    declaration_in_progress = declaration;
    int status = fr_lua_run_in_env_bytes(state, text, length, origin, env, err);
    declaration_in_progress = NULL;
    lua_settop(state, top);

    if (status == FR_OK) return FR_OK;
    /* Anchored at offset 0: lua's error() prefixes the position, so a forged one is not. */
    return strncmp(err->message, FR_PLUGIN_DECLARATION_READ,
                   sizeof FR_PLUGIN_DECLARATION_READ - 1) == 0 ? FR_OK : FR_ERR;
}

/* Shared with resolvers.c: a resolver's chunk starts with the same
   daukle.plugin{ uses = {...} } call a plugin's does, and acquiring it goes
   through this same read-then-load split so that declaration is honoured
   there too, rather than every verb being either always on or always off for
   a resolver. kind names the caller ("plugin" or "resolver") in every message
   below, so a resolver's malformed uses is reported as a resolver, not a
   plugin. A NULL runtime state is a real error here, not a crash:
   read_declaration's first line touches it. */
int fr_plugins_read_declaration(const char *text, size_t length, const char *origin,
                                const char *kind, const char *label,
                                fr_plugin_declaration *out, fr_error *err) {
    memset(out, 0, sizeof *out);
    out->kind = kind;
    out->label = label;

    lua_State *state = fr_lua_runtime_state();
    if (state == NULL) {
        fr_error_set(err, "no lua runtime is open for \"%s\"", origin);
        return FR_ERR;
    }

    if (fr_plugins_declaration_read(state, text, length, origin, out, err) != FR_OK) {
        fr_plugins_free_declaration(out);
        return FR_ERR;
    }
    return FR_OK;
}
