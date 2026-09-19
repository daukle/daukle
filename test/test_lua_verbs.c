#include "greatest.h"

#include "config_lua.h"
#include "lua_verbs.h"
#include "luax.h"
#include "registry.h"

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

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(a_declared_verb_is_present);
    RUN_TEST(an_undeclared_verb_raises_naming_itself);
    RUN_TEST(registration_functions_are_always_present);
    GREATEST_MAIN_END();
}
