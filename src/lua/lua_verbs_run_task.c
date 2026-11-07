#include "lua/lua_verbs_internal.h"

#include "config/config_lua.h"
#include "util/error.h"

#include "lauxlib.h"

static fr_lua_task_runner task_runner;

void fr_lua_set_task_runner(fr_lua_task_runner runner) {
    task_runner = runner;
}

fr_lua_task_runner fr_lua_task_runner_in_progress(void) {
    return task_runner;
}

/* daukle.run_task(name): runs a task of the project, with what it depends on, inside the task
   run in progress, and returns true, or false and the failing task's message.
   @implNote it returns rather than raises because the sandbox has no pcall, and a plugin
   implementing "a || b" needs the failure as a value. */
int fr_lua_verb_run_task(lua_State *state) {
    if (fr_lua_generation_is_running()) {
        return luaL_error(state, "daukle.run_task is not available while generating; "
                                 "generation is a pure function of the manifest");
    }
    if (fr_lua_plugin_exec_is_refused()) {
        return luaL_error(state, "daukle.run_task is not available while a plugin chunk is"
                                 " loading; call it from a task callback");
    }
    const char *phase = fr_lua_process_free_callback_phase();
    if (phase != NULL) {
        return luaL_error(state, "daukle.run_task is not available while %s; call it from a"
                                 " task callback", phase);
    }
    fr_lua_task_runner runner = fr_lua_task_runner_in_progress();
    if (!fr_lua_task_dispatch_is_running() || runner == NULL) {
        return luaL_error(state, "daukle.run_task is available only inside a task or publish"
                                 " callback, where a task run is in progress");
    }
    const char *goal = luaL_checkstring(state, 1);
    fr_error err = { { 0 } };
    if (runner(goal, &err) != FR_OK) {
        lua_pushboolean(state, 0);
        lua_pushstring(state, err.message);
        return 2;
    }
    lua_pushboolean(state, 1);
    return 1;
}
