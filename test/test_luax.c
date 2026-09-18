#include "greatest.h"
#include "luax.h"

#include <string.h>

TEST opens_and_runs_a_chunk(void) {
    fr_error err;
    lua_State *state = fr_lua_open(64u * 1024u * 1024u, &err);
    ASSERT(state != NULL);
    ASSERT_EQ(FR_OK, fr_lua_run(state, "local x = 1 + 1", "=test", &err));
    fr_lua_close(state);
    PASS();
}

TEST reports_a_syntax_error_with_its_chunk_name(void) {
    fr_error err;
    lua_State *state = fr_lua_open(64u * 1024u * 1024u, &err);
    ASSERT_EQ(FR_ERR, fr_lua_run(state, "local = =", "daukle.lua", &err));
    ASSERT(strstr(err.message, "daukle.lua") != NULL);
    fr_lua_close(state);
    PASS();
}

TEST reports_a_runtime_error_with_its_line(void) {
    fr_error err;
    lua_State *state = fr_lua_open(64u * 1024u * 1024u, &err);
    ASSERT_EQ(FR_ERR, fr_lua_run(state, "\nerror('boom')", "daukle.lua", &err));
    ASSERT(strstr(err.message, "boom") != NULL);
    ASSERT(strstr(err.message, "daukle.lua:2") != NULL);
    fr_lua_close(state);
    PASS();
}

TEST stops_a_script_that_allocates_without_end(void) {
    fr_error err;
    lua_State *state = fr_lua_open(1u * 1024u * 1024u, &err);
    ASSERT_EQ(FR_ERR, fr_lua_run(state,
        "local t = {} local i = 1 while true do t[i] = string.rep('x', 1024) i = i + 1 end",
        "daukle.lua", &err));
    fr_lua_close(state);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(opens_and_runs_a_chunk);
    RUN_TEST(reports_a_syntax_error_with_its_chunk_name);
    RUN_TEST(reports_a_runtime_error_with_its_line);
    RUN_TEST(stops_a_script_that_allocates_without_end);
    GREATEST_MAIN_END();
}
