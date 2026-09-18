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
    ASSERT(strstr(err.message, "not enough memory") != NULL);
    fr_lua_close(state);
    PASS();
}

TEST fails_gracefully_when_the_cap_is_too_tight_for_the_standard_libraries(void) {
    fr_error err;
    /* Big enough for lua_newstate to construct the state, too small for
       luaL_openlibs to finish; empirically bracketed between 5120 (state built,
       libs fail) and 19456 (still failing) below 20480 (libs succeed). Before the
       protected-pcall fix in fr_lua_open, this window aborted the process instead
       of returning NULL. */
    lua_State *state = fr_lua_open(8u * 1024u, &err);
    ASSERT(state == NULL);
    ASSERT(strstr(err.message, "not enough memory") != NULL);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(opens_and_runs_a_chunk);
    RUN_TEST(reports_a_syntax_error_with_its_chunk_name);
    RUN_TEST(reports_a_runtime_error_with_its_line);
    RUN_TEST(stops_a_script_that_allocates_without_end);
    RUN_TEST(fails_gracefully_when_the_cap_is_too_tight_for_the_standard_libraries);
    GREATEST_MAIN_END();
}
