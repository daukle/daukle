#include "config/config_lua_internal.h"

#include "config/config_lua.h"
#include "config/manifest.h"
#include "lua/lua_verbs.h"
#include "lua/luax.h"
#include "util/error.h"

#include "cJSON.h"
#include "lauxlib.h"

#include <stdlib.h>

typedef struct {
    fr_lua_plugin_slot *slot;
    const char *project;
    const cJSON *block;
    const char *base_dir;
    cJSON *document;
} lua_source_context;

static lua_source_context *source_context;

static int protected_source_load(lua_State *state) {
    lua_source_context *context = source_context;
    lua_rawgeti(state, LUA_REGISTRYINDEX, context->slot->callback);
    lua_pushstring(state, context->project);
    fr_error push_err;
    if (fr_lua_push_json(state, context->block, &push_err) != FR_OK) {
        return luaL_error(state, "%s", push_err.message);
    }
    lua_pushstring(state, context->base_dir);

    lua_call(state, 3, 1);

    fr_error to_json_err;
    if (fr_lua_to_json(state, -1, &context->document, &to_json_err) != FR_OK) {
        return luaL_error(state, "%s", to_json_err.message);
    }
    return 0;
}

int fr_lua_dispatch_source_load(void *state, const char *project, const cJSON *block,
                           const char *base_dir, fr_project *out, fr_error *err) {
    int top = lua_gettop(fr_lua_runtime_state());
    lua_source_context context = { state, project, block, base_dir, NULL };
    source_context = &context;
    fr_lua_verbs_set_declared_env(
        (const char *const *) ((const fr_lua_plugin_slot *) state)->env,
        ((const fr_lua_plugin_slot *) state)->env_count);
    lua_pushcfunction(fr_lua_runtime_state(), protected_source_load);
    int status = lua_pcall(fr_lua_runtime_state(), 0, 0, 0);
    source_context = NULL;
    fr_lua_verbs_set_declared_env(NULL, 0);

    if (status != LUA_OK) {
        fr_error_set(err, "%s", fr_lua_error_text(fr_lua_runtime_state()));
        lua_settop(fr_lua_runtime_state(), top);
        return FR_ERR;
    }
    lua_settop(fr_lua_runtime_state(), top);

    char *text = cJSON_PrintUnformatted(context.document);
    cJSON_Delete(context.document);
    if (text == NULL) {
        fr_error_set(err, "out of memory reading the project \"%s\" produced", project);
        return FR_ERR;
    }
    int parse_status = fr_project_parse(text, project, out, err);
    free(text);
    return parse_status;
}

typedef struct {
    const char *coordinate;
    const cJSON *block;
    char **out_url;
    char **out_resolved;
    fr_http_headers *out_headers;
} lua_resolver_context;

static lua_resolver_context *resolver_context;

/* lua_next is itself raw, so an __index or __pairs on the returned table is
   never consulted here either. A number key would be coerced to a string by
   reading it, which breaks the traversal, so a non-string key is refused
   rather than read. */
static int read_resolver_headers(lua_State *state, int result, fr_http_headers *out) {
    fr_lua_raw_getfield(state, result, "headers");
    if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        return 0;
    }
    if (!lua_istable(state, -1)) {
        return luaL_error(state, "the resolver returned %s headers, expected a table",
                          luaL_typename(state, -1));
    }

    int table = lua_gettop(state);
    lua_pushnil(state);
    while (lua_next(state, table) != 0) {
        if (lua_type(state, -2) != LUA_TSTRING) {
            return luaL_error(state, "a header name must be a string, not %s",
                              luaL_typename(state, -2));
        }
        if (lua_type(state, -1) != LUA_TSTRING) {
            return luaL_error(state, "the \"%s\" header value must be a string, not %s",
                              lua_tostring(state, -2), luaL_typename(state, -1));
        }
        size_t name_length = 0;
        size_t value_length = 0;
        const char *name = lua_tolstring(state, -2, &name_length);
        const char *value = lua_tolstring(state, -1, &value_length);
        fr_error add_err;
        if (fr_http_headers_add(out, name, name_length, value, value_length, &add_err) != FR_OK) {
            return luaL_error(state, "%s", add_err.message);
        }
        lua_pop(state, 1);
    }
    lua_pop(state, 1);
    return 0;
}

static int protected_resolver_call(lua_State *state) {
    const lua_resolver_context *context = resolver_context;
    lua_rawgeti(state, LUA_REGISTRYINDEX, fr_lua_declare_resolver_callback());
    lua_pushstring(state, context->coordinate);
    fr_error push_err;
    if (fr_lua_push_json(state, context->block, &push_err) != FR_OK) {
        return luaL_error(state, "%s", push_err.message);
    }

    /* lua_call, not lua_pcall: see protected_language_apply above. */
    lua_call(state, 2, 1);

    if (!lua_istable(state, -1)) {
        return luaL_error(state, "the resolver returned %s, expected a table",
                          luaL_typename(state, -1));
    }
    int result = lua_gettop(state);

    fr_lua_raw_getfield(state, result, "url");
    if (lua_type(state, -1) == LUA_TSTRING) {
        *context->out_url = fr_dup_string(lua_tostring(state, -1));
        if (*context->out_url == NULL) return luaL_error(state, "out of memory");
    }
    lua_pop(state, 1);

    fr_lua_raw_getfield(state, result, "resolved");
    if (lua_type(state, -1) == LUA_TSTRING) {
        *context->out_resolved = fr_dup_string(lua_tostring(state, -1));
        if (*context->out_resolved == NULL) return luaL_error(state, "out of memory");
    }
    lua_pop(state, 1);

    return read_resolver_headers(state, result, context->out_headers);
}

int fr_lua_resolver_call(const char *coordinate, const cJSON *block, char **out_url,
                         char **out_resolved, fr_http_headers *out_headers, fr_error *err) {
    *out_url = NULL;
    *out_resolved = NULL;
    out_headers->count = 0;
    if (fr_lua_runtime_state() == NULL) {
        fr_error_set(err, "no lua runtime is open to call a resolver");
        return FR_ERR;
    }
    if (fr_lua_declare_resolver_callback() == LUA_NOREF) {
        fr_error_set(err, "no resolver is declared");
        return FR_ERR;
    }
    int top = lua_gettop(fr_lua_runtime_state());
    lua_resolver_context context = { coordinate, block, out_url, out_resolved, out_headers };
    resolver_context = &context;
    fr_lua_declare_set_resolver_env();
    lua_pushcfunction(fr_lua_runtime_state(), protected_resolver_call);
    int status = lua_pcall(fr_lua_runtime_state(), 0, 0, 0);
    resolver_context = NULL;
    fr_lua_verbs_set_declared_env(NULL, 0);

    if (status != LUA_OK) {
        fr_error_set(err, "%s", fr_lua_error_text(fr_lua_runtime_state()));
        lua_settop(fr_lua_runtime_state(), top);
        free(*out_url);
        *out_url = NULL;
        free(*out_resolved);
        *out_resolved = NULL;
        fr_http_headers_free(out_headers);
        return FR_ERR;
    }
    lua_settop(fr_lua_runtime_state(), top);

    if (*out_url == NULL) {
        free(*out_resolved);
        *out_resolved = NULL;
        fr_http_headers_free(out_headers);
        fr_error_set(err, "the resolver returned no url");
        return FR_ERR;
    }
    return FR_OK;
}
