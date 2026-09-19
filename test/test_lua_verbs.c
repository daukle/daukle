#include "greatest.h"

#include "config_lua.h"
#include "lua_verbs.h"
#include "luax.h"
#include "registry.h"
#include "support.h"

#include <string.h>

TEST a_declared_verb_is_present(void) {
    fr_error err;
    fr_registry *registry = fr_registry_create();
    ASSERT_EQ(FR_OK, fr_lua_runtime_begin(".", registry, &err));
    lua_State *state = fr_lua_runtime_state();

    const char *verbs[] = { "env" };
    ASSERT_EQ(FR_OK, fr_lua_verbs_push_env(state, verbs, 1, &err));
    int env = lua_gettop(state);

    ASSERT_EQ(FR_OK, fr_lua_run_in_env(state, "ok = type(daukle.env) == 'function'", "=t", env, &err));
    lua_getfield(state, env, "ok");
    ASSERT(lua_toboolean(state, -1));

    lua_settop(state, 0);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    PASS();
}

TEST an_undeclared_verb_raises_naming_itself(void) {
    fr_error err;
    fr_registry *registry = fr_registry_create();
    ASSERT_EQ(FR_OK, fr_lua_runtime_begin(".", registry, &err));
    lua_State *state = fr_lua_runtime_state();

    const char *verbs[] = { "env" };
    ASSERT_EQ(FR_OK, fr_lua_verbs_push_env(state, verbs, 1, &err));
    int env = lua_gettop(state);

    ASSERT_EQ(FR_ERR, fr_lua_run_in_env(state, "return daukle.fetch('x')", "=t", env, &err));
    ASSERT(strstr(err.message, "fetch") != NULL);

    lua_settop(state, 0);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    PASS();
}

TEST registration_functions_are_always_present(void) {
    fr_error err;
    fr_registry *registry = fr_registry_create();
    ASSERT_EQ(FR_OK, fr_lua_runtime_begin(".", registry, &err));
    lua_State *state = fr_lua_runtime_state();

    ASSERT_EQ(FR_OK, fr_lua_verbs_push_env(state, NULL, 0, &err));
    int env = lua_gettop(state);

    ASSERT_EQ(FR_OK, fr_lua_run_in_env(state,
        "ok = type(daukle.source) == 'function' and type(daukle.language) == 'function'",
        "=t", env, &err));
    lua_getfield(state, env, "ok");
    ASSERT(lua_toboolean(state, -1));

    lua_settop(state, 0);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    PASS();
}

TEST exec_is_accepted_in_uses_but_not_implemented(void) {
    fr_error err;
    fr_registry *registry = fr_registry_create();
    ASSERT_EQ(FR_OK, fr_lua_runtime_begin(".", registry, &err));
    lua_State *state = fr_lua_runtime_state();

    ASSERT_EQ(1, fr_lua_verbs_is_known("exec"));
    ASSERT_EQ(1, fr_lua_verbs_is_reserved("exec"));

    const char *verbs[] = { "exec" };
    ASSERT_EQ(FR_OK, fr_lua_verbs_push_env(state, verbs, 1, &err));
    int env = lua_gettop(state);

    ASSERT_EQ(FR_ERR, fr_lua_run_in_env(state, "daukle.exec('git')", "=t", env, &err));
    ASSERT(strstr(err.message, "not implemented") != NULL);
    ASSERT(strstr(err.message, "was not declared") == NULL);

    lua_settop(state, 0);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    PASS();
}

TEST env_reads_a_variable_and_nil_for_an_absent_one(void) {
    fr_error err;
    fr_test_set_env("DAUKLE_TEST_VERB", "present");
    fr_registry *registry = fr_registry_create();
    ASSERT_EQ(FR_OK, fr_lua_runtime_begin(".", registry, &err));
    lua_State *state = fr_lua_runtime_state();

    const char *verbs[] = { "env" };
    ASSERT_EQ(FR_OK, fr_lua_verbs_push_env(state, verbs, 1, &err));
    int env = lua_gettop(state);

    ASSERT_EQ(FR_OK, fr_lua_run_in_env(state,
        "found = daukle.env('DAUKLE_TEST_VERB'); missing = daukle.env('DAUKLE_TEST_ABSENT')",
        "=t", env, &err));

    lua_getfield(state, env, "found");
    ASSERT_STR_EQ("present", lua_tostring(state, -1));
    lua_getfield(state, env, "missing");
    ASSERT(lua_isnil(state, -1));

    lua_settop(state, 0);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    PASS();
}

TEST read_refuses_a_path_outside_the_base_directory(void) {
    fr_error err;
    fr_registry *registry = fr_registry_create();
    ASSERT_EQ(FR_OK, fr_lua_runtime_begin("test/fixtures", registry, &err));
    lua_State *state = fr_lua_runtime_state();

    const char *verbs[] = { "read" };
    ASSERT_EQ(FR_OK, fr_lua_verbs_push_env(state, verbs, 1, &err));
    int env = lua_gettop(state);

    ASSERT_EQ(FR_ERR, fr_lua_run_in_env(state, "daukle.read('../../CMakeLists.txt')", "=t", env, &err));

    lua_settop(state, 0);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    PASS();
}

TEST read_returns_the_text_of_a_file_inside_the_base_directory(void) {
    fr_error err;
    fr_registry *registry = fr_registry_create();
    ASSERT_EQ(FR_OK, fr_lua_runtime_begin("test/fixtures/lua-plugin", registry, &err));
    lua_State *state = fr_lua_runtime_state();

    const char *verbs[] = { "read" };
    ASSERT_EQ(FR_OK, fr_lua_verbs_push_env(state, verbs, 1, &err));
    int env = lua_gettop(state);

    ASSERT_EQ(FR_OK, fr_lua_run_in_env(state, "text = daukle.read('daukle.toml')", "=t", env, &err));
    lua_getfield(state, env, "text");
    ASSERT(strstr(lua_tostring(state, -1), "forebay/plugin") != NULL);

    lua_settop(state, 0);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(a_declared_verb_is_present);
    RUN_TEST(an_undeclared_verb_raises_naming_itself);
    RUN_TEST(registration_functions_are_always_present);
    RUN_TEST(exec_is_accepted_in_uses_but_not_implemented);
    RUN_TEST(env_reads_a_variable_and_nil_for_an_absent_one);
    RUN_TEST(read_refuses_a_path_outside_the_base_directory);
    RUN_TEST(read_returns_the_text_of_a_file_inside_the_base_directory);
    GREATEST_MAIN_END();
}
