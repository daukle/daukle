#include "lua_verbs.h"

#include "cache.h"
#include "config_lua.h"
#include "error.h"
#include "http.h"
#include "jsonedit.h"
#include "lang_region.h"
#include "lua_sandbox.h"
#include "luax.h"
#include "region.h"
#include "registry.h"

#include "cJSON.h"
#include "lauxlib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FR_VERB_MAX_HEADERS 16

static const char *BASE[] = {
    "assert", "error", "ipairs", "pairs", "next", "select", "tonumber", "tostring",
    "type", "setmetatable", "getmetatable", "string", "table", "math", "_VERSION"
};

static const char *KNOWN_VERBS[] = {
    "fetch", "read", "cache", "env", "region", "json_set", "parse", "exec"
};

static const char *RESERVED_VERBS[] = { "exec" };

int fr_lua_verbs_is_known(const char *name) {
    for (size_t index = 0; index < sizeof KNOWN_VERBS / sizeof KNOWN_VERBS[0]; index++) {
        if (strcmp(KNOWN_VERBS[index], name) == 0) return 1;
    }
    return 0;
}

int fr_lua_verbs_is_reserved(const char *name) {
    for (size_t index = 0; index < sizeof RESERVED_VERBS / sizeof RESERVED_VERBS[0]; index++) {
        if (strcmp(RESERVED_VERBS[index], name) == 0) return 1;
    }
    return 0;
}

static int verb_env(lua_State *state) {
    const char *name = luaL_checkstring(state, 1);
    const char *value = getenv(name);
    if (value == NULL) {
        lua_pushnil(state);
    } else {
        lua_pushstring(state, value);
    }
    return 1;
}

static int verb_read(lua_State *state) {
    const char *relative = luaL_checkstring(state, 1);
    char *resolved = NULL;
    fr_error err;
    if (fr_lua_sandbox_resolve(state, relative, &resolved, &err) != FR_OK) {
        return luaL_error(state, "%s", err.message);
    }

    char *text = NULL;
    int status = fr_file_read_text(resolved, &text, &err);
    free(resolved);
    if (status != FR_OK) return luaL_error(state, "%s", err.message);

    lua_pushstring(state, text);
    free(text);
    return 1;
}

static int reserved_verb(lua_State *state) {
    return luaL_error(state, "daukle.exec is not implemented in this version");
}

static int verb_fetch(lua_State *state) {
    const char *url = luaL_checkstring(state, 1);

    fr_http_header headers[FR_VERB_MAX_HEADERS];
    size_t header_count = 0;
    if (!lua_isnoneornil(state, 2)) {
        luaL_checktype(state, 2, LUA_TTABLE);
        lua_pushnil(state);
        while (lua_next(state, 2) != 0) {
            if (header_count == FR_VERB_MAX_HEADERS) {
                return luaL_error(state, "at most %d headers", FR_VERB_MAX_HEADERS);
            }
            if (lua_type(state, -2) != LUA_TSTRING || lua_type(state, -1) != LUA_TSTRING) {
                return luaL_error(state, "every header name and value must be a string");
            }
            /* headers[].name/value point into the table still on the stack
               below; the table must not be popped before fr_http_get runs. */
            headers[header_count].name = lua_tostring(state, -2);
            headers[header_count].value = lua_tostring(state, -1);
            header_count++;
            lua_pop(state, 1);
        }
    }

    char *body = NULL;
    size_t length = 0;
    fr_error err;
    if (fr_http_get(url, headers, header_count, &body, &length, &err) != FR_OK) {
        return luaL_error(state, "%s", err.message);
    }
    lua_pushlstring(state, body, length);
    free(body);
    return 1;
}

static int verb_cache(lua_State *state) {
    const char *project = luaL_checkstring(state, 1);
    const char *version = luaL_checkstring(state, 2);
    const char *artifact = luaL_checkstring(state, 3);
    luaL_checktype(state, 4, LUA_TFUNCTION);

    char *cached = NULL;
    fr_error err;
    if (fr_cache_read(project, version, artifact, &cached, &err) == FR_OK && cached != NULL) {
        lua_pushstring(state, cached);
        free(cached);
        return 1;
    }

    lua_pushvalue(state, 4);
    /* Runs inside the plugin's protected frame already, so a producer error
       must propagate with its original message and traceback intact. */
    lua_call(state, 0, 1);
    size_t length = 0;
    const char *produced = lua_tolstring(state, -1, &length);
    if (produced == NULL) return luaL_error(state, "the producer returned no text");

    fr_cache_write(project, version, artifact, produced, length);
    return 1;
}

static const char *pending_region_body;

static int render_pending_body(const fr_consumer *consumer, const fr_resolved *resolved,
                               size_t count, char **out_text, fr_error *err) {
    (void) consumer; (void) resolved; (void) count;
    *out_text = malloc(strlen(pending_region_body) + 1);
    if (*out_text == NULL) {
        fr_error_set(err, "out of memory rendering a region");
        return FR_ERR;
    }
    strcpy(*out_text, pending_region_body);
    return FR_OK;
}

static int verb_region(lua_State *state) {
    const char *text = luaL_checkstring(state, 1);
    const char *begin = luaL_checkstring(state, 2);
    const char *end = luaL_checkstring(state, 3);
    pending_region_body = luaL_checkstring(state, 4);

    char *out = NULL;
    fr_error err;
    int status = fr_lang_region_apply(begin, end, render_pending_body, NULL, NULL, 0,
                                      text, &out, &err);
    pending_region_body = NULL;
    if (status != FR_OK) return luaL_error(state, "%s", err.message);

    lua_pushstring(state, out);
    free(out);
    return 1;
}

static int verb_json_set(lua_State *state) {
    const char *text = luaL_checkstring(state, 1);
    const char *pointer = luaL_checkstring(state, 2);
    const char *value = luaL_checkstring(state, 3);

    const char *last_dot = strrchr(pointer, '.');
    char container[256];
    const char *key = pointer;
    container[0] = '\0';
    if (last_dot != NULL) {
        size_t length = (size_t) (last_dot - pointer);
        if (length >= sizeof container) return luaL_error(state, "\"%s\" is too long", pointer);
        memcpy(container, pointer, length);
        container[length] = '\0';
        key = last_dot + 1;
    }

    char *out = NULL;
    fr_error err;
    if (fr_json_edit_set_string(text, container, key, value, &out, &err) != FR_OK) {
        return luaL_error(state, "%s", err.message);
    }
    lua_pushstring(state, out);
    free(out);
    return 1;
}

static int verb_parse(lua_State *state) {
    const char *text = luaL_checkstring(state, 1);
    const char *file_name = luaL_checkstring(state, 2);

    fr_registry *registry = fr_lua_registering_registry();
    if (registry == NULL) return luaL_error(state, "no registry is loading");

    char capability[128];
    const char *extension = strrchr(file_name, '.');
    if (extension == NULL) return luaL_error(state, "\"%s\" has no extension", file_name);
    snprintf(capability, sizeof capability, "daukle.config/%s", extension + 1);

    const fr_config_plugin *plugin = fr_registry_config(registry, capability);
    if (plugin == NULL) return luaL_error(state, "no plugin reads \"%s\"", extension + 1);
    if (plugin->overlay) {
        return luaL_error(state, "daukle.parse will not run \"%s\", which is an executable format",
                          extension + 1);
    }

    cJSON *document = NULL;
    fr_error err;
    if (plugin->load(plugin->state, text, file_name, ".", registry, NULL, &document, &err) != FR_OK) {
        return luaL_error(state, "%s", err.message);
    }

    int pushed = fr_lua_push_json(state, document, &err);
    cJSON_Delete(document);
    if (pushed != FR_OK) return luaL_error(state, "%s", err.message);
    return 1;
}

/* install_one handles all seven declared verbs plus the reserved exec; a name
   that is known (fr_lua_verbs_is_known) but neither handled here nor reserved
   would be left unset, which cannot currently happen. */
static void install_one(lua_State *state, const char *name) {
    if (strcmp(name, "env") == 0) {
        lua_pushcfunction(state, verb_env);
    } else if (strcmp(name, "read") == 0) {
        lua_pushcfunction(state, verb_read);
    } else if (strcmp(name, "fetch") == 0) {
        lua_pushcfunction(state, verb_fetch);
    } else if (strcmp(name, "cache") == 0) {
        lua_pushcfunction(state, verb_cache);
    } else if (strcmp(name, "region") == 0) {
        lua_pushcfunction(state, verb_region);
    } else if (strcmp(name, "json_set") == 0) {
        lua_pushcfunction(state, verb_json_set);
    } else if (strcmp(name, "parse") == 0) {
        lua_pushcfunction(state, verb_parse);
    } else if (fr_lua_verbs_is_reserved(name)) {
        lua_pushcfunction(state, reserved_verb);
    } else {
        return;
    }
    lua_setfield(state, -2, name);
}

static int undeclared_verb(lua_State *state) {
    const char *name = lua_tostring(state, 2);
    return luaL_error(state, "daukle.%s was not declared in uses",
                      name != NULL ? name : "?");
}

static const char *const *pending_verbs;
static size_t pending_verb_count;

static int protected_push_env(lua_State *state) {
    lua_newtable(state);
    for (size_t index = 0; index < sizeof BASE / sizeof BASE[0]; index++) {
        lua_getglobal(state, BASE[index]);
        lua_setfield(state, -2, BASE[index]);
    }

    lua_newtable(state);
    fr_lua_verbs_install_registration(state);
    for (size_t index = 0; index < pending_verb_count; index++) {
        install_one(state, pending_verbs[index]);
    }

    lua_newtable(state);
    lua_pushcfunction(state, undeclared_verb);
    lua_setfield(state, -2, "__index");
    lua_pushstring(state, "the daukle plugin environment");
    lua_setfield(state, -2, "__metatable");
    lua_setmetatable(state, -2);

    lua_setfield(state, -2, "daukle");

    lua_pushvalue(state, -1);
    lua_setfield(state, -2, "_G");
    return 1;
}

int fr_lua_verbs_push_env(lua_State *state, const char *const *verbs, size_t verb_count,
                          fr_error *err) {
    pending_verbs = verbs;
    pending_verb_count = verb_count;
    lua_pushcfunction(state, protected_push_env);
    int status = lua_pcall(state, 0, 1, 0);
    pending_verbs = NULL;
    pending_verb_count = 0;

    if (status != LUA_OK) {
        fr_error_set(err, "%s", fr_lua_error_text(state));
        lua_pop(state, 1);
        return FR_ERR;
    }
    return FR_OK;
}
