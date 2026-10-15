#include "config/config_lua_internal.h"

#include "config/config_lua.h"
#include "plugin/resolve.h"
#include "lua/lua_verbs.h"
#include "lua/luax.h"
#include "util/error.h"

#include "lauxlib.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    fr_lua_plugin_slot *slot;
    const fr_consumer *consumer;
    const fr_resolved *resolved;
    size_t count;
    const char *original_text;
    char **out_text;
} lua_apply_context;

static lua_apply_context *apply_context;

static int protected_language_apply(lua_State *state) {
    const lua_apply_context *context = apply_context;
    const fr_lua_plugin_slot *slot = context->slot;
    lua_rawgeti(state, LUA_REGISTRYINDEX, slot->callback);

    lua_newtable(state);
    lua_pushstring(state, context->consumer->id);
    lua_setfield(state, -2, "id");
    lua_pushstring(state, context->consumer->configuration);
    lua_setfield(state, -2, "configuration");

    lua_newtable(state);
    for (size_t index = 0; index < context->count; index++) {
        lua_newtable(state);
        lua_pushstring(state, context->resolved[index].project);
        lua_setfield(state, -2, "project");
        lua_pushstring(state, context->resolved[index].module);
        lua_setfield(state, -2, "module");
        fr_error push_err;
        if (fr_lua_push_json(state, context->resolved[index].block, &push_err) != FR_OK) {
            return luaL_error(state, "%s", push_err.message);
        }
        lua_setfield(state, -2, "block");
        lua_rawseti(state, -2, (lua_Integer) index + 1);
    }

    lua_pushstring(state, context->original_text);

    /* lua_call, not lua_pcall: this whole function already runs inside the caller's
       protected frame, so an error the script raises here unwinds straight to it. */
    lua_call(state, 3, 1);

    const char *produced = lua_tostring(state, -1);
    if (produced == NULL) {
        return luaL_error(state, "the \"%s\" plugin returned %s, expected a string",
                          slot->capability, luaL_typename(state, -1));
    }
    size_t length = strlen(produced) + 1;
    char *copy = malloc(length);
    if (copy == NULL) {
        return luaL_error(state, "out of memory taking the output of \"%s\"", slot->capability);
    }
    memcpy(copy, produced, length);
    *context->out_text = copy;
    return 0;
}

int fr_lua_dispatch_language_apply(void *state, const fr_consumer *consumer,
                              const fr_resolved *resolved, size_t count,
                              const char *original_text, char **out_text, fr_error *err) {
    int top = lua_gettop(fr_lua_runtime_state());
    lua_apply_context context = { state, consumer, resolved, count, original_text, out_text };
    apply_context = &context;
    fr_lua_verbs_set_declared_env(
        (const char *const *) ((const fr_lua_plugin_slot *) state)->env,
        ((const fr_lua_plugin_slot *) state)->env_count);
    lua_pushcfunction(fr_lua_runtime_state(), protected_language_apply);
    fr_lua_enter_process_free_callback("a language applies");
    int status = lua_pcall(fr_lua_runtime_state(), 0, 0, 0);
    fr_lua_leave_process_free_callback();
    apply_context = NULL;
    fr_lua_verbs_set_declared_env(NULL, 0);

    if (status != LUA_OK) {
        fr_error_set(err, "%s", fr_lua_error_text(fr_lua_runtime_state()));
        lua_settop(fr_lua_runtime_state(), top);
        return FR_ERR;
    }
    lua_settop(fr_lua_runtime_state(), top);
    return FR_OK;
}
