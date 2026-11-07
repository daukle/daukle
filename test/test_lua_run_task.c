#include "greatest.h"
#include "config/config_lua.h"
#include "lua/lua_verbs.h"
#include "lua/luax.h"
#include "plugin/registry.h"
#include "util/error.h"

#include <stdio.h>
#include <string.h>

static char asked_for[128];

static int refusing_runner(const char *goal, fr_error *err) {
    snprintf(asked_for, sizeof asked_for, "%s", goal);
    fr_error_set(err, "task \"%s\": it said no", goal);
    return FR_ERR;
}

/* Runs chunk with run_task declared, inside the task window when in_task is set, and keeps the
   error or the chunk's own error() text in message. */
static int run_chunk(const char *chunk, int in_task, char *message, size_t size) {
    fr_registry *registry = fr_registry_create();
    fr_error err;
    err.message[0] = '\0';
    int status = FR_ERR;
    if (fr_lua_runtime_begin(".", registry, &err) == FR_OK) {
        lua_State *state = fr_lua_runtime_state();
        const char *verbs[] = { "run_task" };
        if (fr_lua_verbs_push_env(state, verbs, 1, &err) == FR_OK) {
            int env = lua_gettop(state);
            if (in_task) fr_lua_enter_task_dispatch();
            status = fr_lua_run_in_env(state, chunk, "=t", env, &err);
            if (in_task) fr_lua_leave_task_dispatch();
        }
        lua_settop(state, 0);
    }
    snprintf(message, size, "%s", err.message);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    return status;
}

TEST run_task_outside_a_task_is_refused(void) {
    static char message[512];
    ASSERT_EQ(FR_ERR, run_chunk("daukle.run_task('a:b')", 0, message, sizeof message));
    ASSERTm(message, strstr(message, "daukle.run_task is available only inside a task") != NULL);
    PASS();
}

TEST run_task_in_a_window_with_no_run_in_progress_is_refused(void) {
    static char message[512];
    fr_lua_set_task_runner(NULL);
    ASSERT_EQ(FR_ERR, run_chunk("daukle.run_task('a:b')", 1, message, sizeof message));
    ASSERTm(message, strstr(message, "where a task run is in progress") != NULL);
    PASS();
}

TEST run_task_hands_back_a_failure_as_a_value(void) {
    static char message[512];
    asked_for[0] = '\0';
    fr_lua_set_task_runner(refusing_runner);
    int status = run_chunk("local ok, why = daukle.run_task('a:b')\n"
                           "error(tostring(ok) .. ' ' .. why, 0)", 1, message, sizeof message);
    fr_lua_set_task_runner(NULL);
    ASSERT_EQ(FR_ERR, status);
    ASSERT_STR_EQ("a:b", asked_for);
    ASSERTm(message, strstr(message, "false task \"a:b\": it said no") != NULL);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(run_task_outside_a_task_is_refused);
    RUN_TEST(run_task_in_a_window_with_no_run_in_progress_is_refused);
    RUN_TEST(run_task_hands_back_a_failure_as_a_value);
    GREATEST_MAIN_END();
}
