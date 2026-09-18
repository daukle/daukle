#include "greatest.h"
#include "luax.h"
#include "lua_sandbox.h"

#include <string.h>

static lua_State *sandboxed(fr_error *err) {
    lua_State *state = fr_lua_open(64u * 1024u * 1024u, err);
    fr_lua_sandbox_install(state, "test/fixtures/lua", err);
    return state;
}

TEST names_a_removed_library_in_the_error(void) {
    fr_error err;
    lua_State *state = sandboxed(&err);
    ASSERT_EQ(FR_ERR, fr_lua_run(state, "io.open('secret', 'r')", "daukle.lua", &err));
    ASSERT(strstr(err.message, "io is not available") != NULL);
    fr_lua_close(state);
    PASS();
}

TEST keeps_the_libraries_a_config_needs(void) {
    fr_error err;
    lua_State *state = sandboxed(&err);
    ASSERT_EQ(FR_OK, fr_lua_run(state,
        "assert(string.rep('a', 3) == 'aaa')\n"
        "assert(table.concat({'a','b'}, ',') == 'a,b')\n"
        "assert(math.max(1, 2) == 2)\n", "daukle.lua", &err));
    fr_lua_close(state);
    PASS();
}

TEST stops_a_script_that_never_finishes(void) {
    fr_error err;
    lua_State *state = sandboxed(&err);
    fr_lua_set_instruction_limit(state, 100000);
    ASSERT_EQ(FR_ERR, fr_lua_run(state, "while true do end", "daukle.lua", &err));
    ASSERT(strstr(err.message, "too long") != NULL);
    fr_lua_close(state);
    PASS();
}

TEST includes_a_file_beside_the_manifest(void) {
    fr_error err;
    lua_State *state = sandboxed(&err);
    ASSERT_EQ(FR_OK, fr_lua_run(state,
        "daukle.include('helper.lua')\n"
        "assert(helper_ran == true)\n", "daukle.lua", &err));
    fr_lua_close(state);
    PASS();
}

TEST refuses_an_include_that_climbs_out(void) {
    fr_error err;
    lua_State *state = sandboxed(&err);
    ASSERT_EQ(FR_ERR, fr_lua_run(state, "daukle.include('../escape.lua')", "daukle.lua", &err));
    ASSERT(strstr(err.message, "outside") != NULL);
    fr_lua_close(state);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(names_a_removed_library_in_the_error);
    RUN_TEST(keeps_the_libraries_a_config_needs);
    RUN_TEST(stops_a_script_that_never_finishes);
    RUN_TEST(includes_a_file_beside_the_manifest);
    RUN_TEST(refuses_an_include_that_climbs_out);
    GREATEST_MAIN_END();
}
