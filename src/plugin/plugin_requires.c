#include "plugin/plugins_internal.h"

#include "plugin/plugins.h"

#include "lauxlib.h"

#include <stdlib.h>
#include <string.h>

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

int fr_plugins_record_requires(lua_State *state, fr_plugin_declaration *declaration) {
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
