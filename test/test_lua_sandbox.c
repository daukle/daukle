#include "greatest.h"
#include "luax.h"
#include "lua_sandbox.h"
#include "region.h"
#include "support.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

static lua_State *sandboxed_at(const char *base_dir, fr_error *err) {
    lua_State *state = fr_lua_open(64u * 1024u * 1024u, err);
    fr_lua_sandbox_install(state, base_dir, err);
    return state;
}

static lua_State *sandboxed(fr_error *err) {
    return sandboxed_at("test/fixtures/lua", err);
}

/* Links a directory so a test can reproduce the reparse-point escape the
   sandbox has to defend against without shipping one in the repository
   itself: a plain file symlink needs a privilege this environment does not
   grant, but an NTFS junction (Windows) or a directory symlink (POSIX) does
   not, and either is a faithful stand-in for "a project's checkout already
   contains a link pointing outside itself" by the time daukle runs. */
static int link_directory(const char *link_path, const char *target_path) {
#ifdef _WIN32
    char full_target[900];
    if (GetFullPathNameA(target_path, sizeof full_target, full_target, NULL) == 0) return 0;
    char command[2048];
    snprintf(command, sizeof command, "cmd /c mklink /J \"%s\" \"%s\" >NUL 2>&1", link_path, full_target);
    return system(command) == 0;
#else
    return symlink(target_path, link_path) == 0;
#endif
}

static void remove_link_directory(const char *link_path) {
#ifdef _WIN32
    _rmdir(link_path);
#else
    unlink(link_path);
#endif
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

TEST removing_pcall_makes_the_instruction_budget_terminal(void) {
    fr_error err;
    lua_State *state = sandboxed(&err);
    fr_lua_set_instruction_limit(state, 100000);
    ASSERT_EQ(FR_ERR, fr_lua_run(state,
        "while true do pcall(function() while true do end end) end",
        "daukle.lua", &err));
    ASSERT(strstr(err.message, "pcall is not available") != NULL);
    fr_lua_close(state);

    lua_State *plain_state = sandboxed(&err);
    fr_lua_set_instruction_limit(plain_state, 100000);
    ASSERT_EQ(FR_ERR, fr_lua_run(plain_state, "while true do end", "daukle.lua", &err));
    ASSERT(strstr(err.message, "too long") != NULL);
    fr_lua_close(plain_state);
    PASS();
}

TEST removes_raw_table_access(void) {
    fr_error err;
    lua_State *state = sandboxed(&err);
    ASSERT_EQ(FR_ERR, fr_lua_run(state, "rawget(_G, 'io')", "daukle.lua", &err));
    ASSERT(strstr(err.message, "rawget is not available") != NULL);
    fr_lua_close(state);
    PASS();
}

TEST names_coroutine_and_utf8_in_the_error(void) {
    fr_error err;
    lua_State *state = sandboxed(&err);
    ASSERT_EQ(FR_ERR, fr_lua_run(state, "coroutine.create(function() end)", "daukle.lua", &err));
    ASSERT(strstr(err.message, "coroutine is not available") != NULL);
    fr_lua_close(state);

    lua_State *other_state = sandboxed(&err);
    ASSERT_EQ(FR_ERR, fr_lua_run(other_state, "utf8.char(65)", "daukle.lua", &err));
    ASSERT(strstr(err.message, "utf8 is not available") != NULL);
    fr_lua_close(other_state);
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

TEST refuses_an_include_path_that_is_too_long(void) {
    fr_error err;
    lua_State *state = sandboxed(&err);
    char long_name[600];
    memset(long_name, 'a', sizeof long_name - 1);
    long_name[sizeof long_name - 1] = '\0';
    char script[700];
    snprintf(script, sizeof script, "daukle.include('%s')", long_name);
    ASSERT_EQ(FR_ERR, fr_lua_run(state, script, "daukle.lua", &err));
    ASSERT(strstr(err.message, "too long") != NULL);
    fr_lua_close(state);
    PASS();
}

TEST refuses_an_include_that_escapes_through_a_reparse_point(void) {
    char root[512];
    snprintf(root, sizeof root, "%s/daukle_test_sandbox_link_%d", fr_test_temp_base(), fr_test_process_id());
    fr_test_remove_tree(root);
    ASSERT_EQ(0, fr_test_make_directory(root));

    char base_dir[560], outside_dir[560], outside_file[600], link_path[600];
    snprintf(base_dir, sizeof base_dir, "%s/project", root);
    snprintf(outside_dir, sizeof outside_dir, "%s/outside", root);
    ASSERT_EQ(0, fr_test_make_directory(base_dir));
    ASSERT_EQ(0, fr_test_make_directory(outside_dir));

    fr_error err;
    snprintf(outside_file, sizeof outside_file, "%s/secret.lua", outside_dir);
    ASSERT_EQ(FR_OK, fr_file_write_text(outside_file, "secret_reached = true\n", &err));

    snprintf(link_path, sizeof link_path, "%s/escape", base_dir);
    if (!link_directory(link_path, outside_dir)) {
        fr_test_remove_tree(root);
        SKIPm("cannot create a symlink or junction in this test environment");
    }

    lua_State *state = sandboxed_at(base_dir, &err);
    ASSERT_EQ(FR_ERR, fr_lua_run(state, "daukle.include('escape/secret.lua')", "daukle.lua", &err));
    ASSERT(strstr(err.message, "outside") != NULL);
    fr_lua_close(state);

    remove_link_directory(link_path);
    fr_test_remove_tree(root);
    PASS();
}

TEST refuses_an_include_whose_target_only_shares_a_path_prefix(void) {
    char root[512];
    snprintf(root, sizeof root, "%s/daukle_test_sandbox_prefix_%d", fr_test_temp_base(), fr_test_process_id());
    fr_test_remove_tree(root);
    ASSERT_EQ(0, fr_test_make_directory(root));

    char base_dir[560], sibling_dir[560], sibling_file[600], link_path[600];
    snprintf(base_dir, sizeof base_dir, "%s/project", root);
    snprintf(sibling_dir, sizeof sibling_dir, "%s/projectevil", root);
    ASSERT_EQ(0, fr_test_make_directory(base_dir));
    ASSERT_EQ(0, fr_test_make_directory(sibling_dir));

    fr_error err;
    snprintf(sibling_file, sizeof sibling_file, "%s/secret.lua", sibling_dir);
    ASSERT_EQ(FR_OK, fr_file_write_text(sibling_file, "secret_reached = true\n", &err));

    snprintf(link_path, sizeof link_path, "%s/escape", base_dir);
    if (!link_directory(link_path, sibling_dir)) {
        fr_test_remove_tree(root);
        SKIPm("cannot create a symlink or junction in this test environment");
    }

    lua_State *state = sandboxed_at(base_dir, &err);
    ASSERT_EQ(FR_ERR, fr_lua_run(state, "daukle.include('escape/secret.lua')", "daukle.lua", &err));
    ASSERT(strstr(err.message, "outside") != NULL);
    fr_lua_close(state);

    remove_link_directory(link_path);
    fr_test_remove_tree(root);
    PASS();
}

TEST refuses_to_change_its_own_metatable(void) {
    fr_error err;
    lua_State *state = sandboxed(&err);
    ASSERT_EQ(FR_ERR, fr_lua_run(state, "setmetatable(_G, nil)", "daukle.lua", &err));
    ASSERT(strstr(err.message, "protected metatable") != NULL);
    ASSERT_EQ(FR_ERR, fr_lua_run(state, "if io then end", "daukle.lua", &err));
    ASSERT(strstr(err.message, "io is not available") != NULL);
    fr_lua_close(state);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(names_a_removed_library_in_the_error);
    RUN_TEST(keeps_the_libraries_a_config_needs);
    RUN_TEST(stops_a_script_that_never_finishes);
    RUN_TEST(removing_pcall_makes_the_instruction_budget_terminal);
    RUN_TEST(removes_raw_table_access);
    RUN_TEST(names_coroutine_and_utf8_in_the_error);
    RUN_TEST(includes_a_file_beside_the_manifest);
    RUN_TEST(refuses_an_include_that_climbs_out);
    RUN_TEST(refuses_an_include_path_that_is_too_long);
    RUN_TEST(refuses_an_include_that_escapes_through_a_reparse_point);
    RUN_TEST(refuses_an_include_whose_target_only_shares_a_path_prefix);
    RUN_TEST(refuses_to_change_its_own_metatable);
    GREATEST_MAIN_END();
}
