#include "config/config_dispatch_migrate.h"

#include "config/config_lua.h"
#include "config/config_lua_internal.h"
#include "lua/lua_verbs.h"
#include "lua/luax.h"
#include "project/derived.h"

#include "cJSON.h"
#include "lauxlib.h"

#include <stdlib.h>

/* The protected-entry rule in config_dispatch.c applies to every function named
   protected_ below: the Lua API is touched only inside a C function run under
   lua_pcall, and the arguments travel through this file-static context. */
typedef struct {
    const fr_lua_plugin_slot *slot;
    int callback;
    const fr_migrate_call *call;
    const cJSON *foreign;
    cJSON **out_result;
} migrate_dispatch_context;

static migrate_dispatch_context *dispatch_context;

static void push_call(lua_State *state, const fr_migrate_call *call) {
    lua_newtable(state);
    lua_pushstring(state, call->tool);
    lua_setfield(state, -2, "tool");
    lua_pushstring(state, call->instance);
    lua_setfield(state, -2, "instance");
    if (call->dir != NULL) {
        lua_pushstring(state, call->dir);
        lua_setfield(state, -2, "dir");
    }
    lua_pushstring(state, call->root);
    lua_setfield(state, -2, "root");
    lua_newtable(state);
    lua_pushstring(state, fr_lua_host_os());
    lua_setfield(state, -2, "os");
    lua_pushstring(state, fr_lua_host_arch());
    lua_setfield(state, -2, "arch");
    lua_setfield(state, -2, "host");
}

static void raise_on_failure(lua_State *state, int status, const fr_error *err) {
    if (status != FR_OK) luaL_error(state, "%s", err->message);
}

static void convert_table_result(lua_State *state, const char *what, const char *name,
                                 cJSON **out_result) {
    if (!lua_istable(state, -1)) {
        luaL_error(state, "the %s \"%s\" returned %s, expected a table", what, name,
                   luaL_typename(state, -1));
    }
    fr_error convert_err;
    raise_on_failure(state, fr_lua_to_json(state, -1, out_result, &convert_err), &convert_err);
}

static int protected_migrator_call(lua_State *state) {
    const migrate_dispatch_context *context = dispatch_context;
    lua_rawgeti(state, LUA_REGISTRYINDEX, context->callback);
    push_call(state, context->call);
    if (context->call->proof != NULL) {
        fr_error push_err;
        raise_on_failure(state, fr_lua_push_json(state, context->call->proof, &push_err), &push_err);
        lua_setfield(state, -2, "proof");
    }
    lua_call(state, 1, context->out_result != NULL ? 1 : 0);
    if (context->out_result != NULL) {
        convert_table_result(state, "migrator", context->slot->capability, context->out_result);
    }
    return 0;
}

static int protected_translator_call(lua_State *state) {
    const migrate_dispatch_context *context = dispatch_context;
    lua_rawgeti(state, LUA_REGISTRYINDEX, context->callback);
    fr_error push_err;
    raise_on_failure(state, fr_lua_push_json(state, context->foreign, &push_err), &push_err);
    lua_call(state, 1, 1);
    convert_table_result(state, "translator", context->slot->capability, context->out_result);
    return 0;
}

/* daukle.write resolves the derived directory and fails when it is missing, so
   the migrator's window makes it the way a task's does. */
static int ensure_derived_dir(const fr_migrate_call *call, fr_error *err) {
    char *derived_root = NULL;
    if (fr_derived_root(call->manifest_dir, &derived_root, err) != FR_OK) return FR_ERR;
    int prepared = fr_derived_ensure_root(derived_root, err);
    free(derived_root);
    if (prepared != FR_OK) return FR_ERR;

    char *derived_dir = NULL;
    if (fr_derived_dir(call->manifest_dir, call->instance, &derived_dir, err) != FR_OK) return FR_ERR;
    prepared = fr_derived_ensure_dir(derived_dir, err);
    free(derived_dir);
    return prepared;
}

static int finish_call(int status, int top, fr_error *err) {
    lua_State *state = fr_lua_runtime_state();
    if (status != LUA_OK) {
        fr_error_set(err, "%s", fr_lua_error_text(state));
        lua_settop(state, top);
        return FR_ERR;
    }
    lua_settop(state, top);
    return FR_OK;
}

static int call_migrator(const fr_migrator_plugin *migrator, int callback, const fr_migrate_call *call,
                         cJSON **out_result, fr_error *err) {
    const fr_lua_plugin_slot *slot = migrator->state;
    if (ensure_derived_dir(call, err) != FR_OK) return FR_ERR;

    int top = lua_gettop(fr_lua_runtime_state());
    migrate_dispatch_context context = { slot, callback, call, NULL, out_result };
    dispatch_context = &context;
    fr_lua_verbs_set_declared_env((const char *const *) slot->env, slot->env_count);
    fr_lua_set_task_cwd(call->derived_dir_relative);
    lua_pushcfunction(fr_lua_runtime_state(), protected_migrator_call);
    fr_lua_enter_task_dispatch();
    int status = lua_pcall(fr_lua_runtime_state(), 0, 0, 0);
    fr_lua_leave_task_dispatch();
    fr_lua_set_task_cwd(NULL);
    dispatch_context = NULL;
    fr_lua_verbs_set_declared_env(NULL, 0);
    return finish_call(status, top, err);
}

int fr_lua_dispatch_migrator_run(const fr_migrator_plugin *migrator, const fr_migrate_call *call,
                                 cJSON **out_result, fr_error *err) {
    const fr_lua_plugin_slot *slot = migrator->state;
    return call_migrator(migrator, slot->callback, call, out_result, err);
}

int fr_lua_dispatch_migrator_build(const fr_migrator_plugin *migrator, const fr_migrate_call *call,
                                   fr_error *err) {
    const fr_lua_plugin_slot *slot = migrator->state;
    if (slot->second_callback == LUA_NOREF) return FR_OK;
    return call_migrator(migrator, slot->second_callback, call, NULL, err);
}

int fr_lua_dispatch_translator(const fr_translator_plugin *translator, const cJSON *foreign,
                               cJSON **out_result, fr_error *err) {
    const fr_lua_plugin_slot *slot = translator->state;
    int top = lua_gettop(fr_lua_runtime_state());
    migrate_dispatch_context context = { slot, slot->callback, NULL, foreign, out_result };
    dispatch_context = &context;
    fr_lua_verbs_set_declared_env((const char *const *) slot->env, slot->env_count);
    lua_pushcfunction(fr_lua_runtime_state(), protected_translator_call);
    fr_lua_enter_process_free_callback("a translator translates");
    int status = lua_pcall(fr_lua_runtime_state(), 0, 0, 0);
    fr_lua_leave_process_free_callback();
    dispatch_context = NULL;
    fr_lua_verbs_set_declared_env(NULL, 0);
    return finish_call(status, top, err);
}
