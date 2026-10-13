#include "plugin/plugins_internal.h"

#include "plugin/plugins.h"

#include "config/config_lua.h"
#include "lua/lua_verbs.h"
#include "lua/luax.h"
#include "plugin/plugin_modules.h"
#include "util/error.h"
#include "util/version.h"

#include "lauxlib.h"

#include <stdlib.h>
#include <string.h>

/* Raised to stop the chunk the moment daukle.plugin has been read, so catching it
   distinguishes a declaration that was read from a plugin that really failed. */
#define FR_PLUGIN_DECLARATION_READ "daukle: plugin declaration read"

/* The plugin api this binary provides. A plugin declaring a HIGHER one is
   refused by name; a plugin declaring a lower one is accepted forever, which is
   what makes the number usable at all.

   @implNote raise this in any release that adds a plugin-visible capability
   which is NOT a new verb. A new verb needs no bump: an unknown entry in `uses`
   is already refused by name by every core back to 0.1.0, which was measured
   against the released binaries rather than assumed. The shape with no signal
   of its own is a new FIELD inside a context table a callback already receives,
   which is how `context.modules` reached 0.7.0 and `context.toolchain.version`
   before it, and how a plugin needing one got a green build that compiled
   nothing. D-116. */
#define FR_PLUGIN_API_PROVIDED 1

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
    for (size_t index = 0; index < declaration->env_count; index++) {
        free(declaration->env[index]);
    }
    declaration->env_count = 0;
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

/* Mirrors record_uses, with the validation a variable name needs rather than the
   known-verb check: a name is compared against the environment, so an empty one
   or one holding "=" could never match anything and is a typo rather than a
   permission. */
static int record_env(lua_State *state, fr_plugin_declaration *declaration) {
    if (lua_isnil(state, -1)) return 0;
    if (!lua_istable(state, -1)) {
        return luaL_error(state, "%s \"%s\": env must be a list of variable names",
                          declaration->kind, declaration->label);
    }

    lua_Integer length = (lua_Integer) lua_rawlen(state, -1);
    for (lua_Integer index = 1; index <= length; index++) {
        lua_rawgeti(state, -1, index);
        if (lua_type(state, -1) != LUA_TSTRING) {
            return luaL_error(state, "%s \"%s\": every name in env must be a string",
                              declaration->kind, declaration->label);
        }
        const char *name = lua_tostring(state, -1);
        if (name[0] == 0x00) {
            return luaL_error(state, "%s \"%s\": env names an empty variable",
                              declaration->kind, declaration->label);
        }
        if (strlen(name) >= FR_PLUGIN_MAX_ENV_NAME) {
            return luaL_error(state, "%s \"%s\": the env name \"%s\" is longer than %d bytes",
                              declaration->kind, declaration->label, name,
                              FR_PLUGIN_MAX_ENV_NAME - 1);
        }
        if (strchr(name, 0x3d) != NULL) {
            return luaL_error(state, "%s \"%s\": the env name \"%s\" holds \"=\", which no"
                                     " variable name can", declaration->kind,
                              declaration->label, name);
        }
        if (declaration->env_count == FR_PLUGIN_MAX_ENV) {
            return luaL_error(state, "%s \"%s\": env names more than %d variables",
                              declaration->kind, declaration->label, FR_PLUGIN_MAX_ENV);
        }
        char *copy = fr_dup_string(name);
        if (copy == NULL) {
            return luaL_error(state, "%s \"%s\": out of memory reading env",
                              declaration->kind, declaration->label);
        }
        declaration->env[declaration->env_count] = copy;
        declaration->env_count++;
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
        if (api < 1) {
            return luaL_error(state, "%s \"%s\": api must be 1 or greater, not %I",
                              declaration->kind, declaration->label, (LUAI_UACINT) api);
        }
        /* Only a HIGHER api is refused. The check was `api != 1`, which made the
           field impossible to use: the first core to provide 2 would have
           refused every plugin in the org, since they all declare 1. */
        if (api > FR_PLUGIN_API_PROVIDED) {
            return luaL_error(state,
                              "%s \"%s\": needs daukle api %I and this daukle %s provides %d,"
                              " so upgrade daukle",
                              declaration->kind, declaration->label, (LUAI_UACINT) api,
                              fr_self_version(), FR_PLUGIN_API_PROVIDED);
        }
    }
    lua_pop(state, 1);

    lua_getfield(state, 1, "uses");
    record_uses(state, declaration);
    lua_pop(state, 1);

    lua_getfield(state, 1, "requires");
    fr_plugins_record_requires(state, declaration);
    lua_pop(state, 1);

    lua_getfield(state, 1, "exports");
    record_exports(state, declaration);
    lua_pop(state, 1);

    lua_getfield(state, 1, "env");
    record_env(state, declaration);
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
