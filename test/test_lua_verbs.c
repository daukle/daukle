#include "greatest.h"

#include "cache.h"
#include "config_lua.h"
#include "http.h"
#include "lua_verbs.h"
#include "luax.h"
#include "registry.h"
#include "support.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *test_verb_dup(const char *text) {
    size_t size = strlen(text) + 1;
    char *copy = malloc(size);
    if (copy != NULL) memcpy(copy, text, size);
    return copy;
}

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

static int stub_http_get(const char *url, const fr_http_header *headers, size_t header_count,
                         char **out_body, size_t *out_length, fr_error *err) {
    (void) err;
    for (size_t index = 0; index < header_count; index++) {
        if (strcmp(headers[index].name, "Authorization") == 0) {
            *out_body = test_verb_dup("authorized");
            *out_length = strlen(*out_body);
            return FR_OK;
        }
    }
    *out_body = test_verb_dup(url);
    *out_length = strlen(*out_body);
    return FR_OK;
}

TEST fetch_returns_the_body_and_passes_headers(void) {
    fr_error err;
    fr_http_fn previous = fr_http_set_backend(stub_http_get);

    fr_registry *registry = fr_registry_create();
    ASSERT_EQ(FR_OK, fr_lua_runtime_begin(".", registry, &err));
    lua_State *state = fr_lua_runtime_state();

    const char *verbs[] = { "fetch" };
    ASSERT_EQ(FR_OK, fr_lua_verbs_push_env(state, verbs, 1, &err));
    int env = lua_gettop(state);

    ASSERT_EQ(FR_OK, fr_lua_run_in_env(state,
        "plain = daukle.fetch('https://example.test/a')\n"
        "authed = daukle.fetch('https://example.test/a', { Authorization = 'Bearer x' })",
        "=t", env, &err));

    lua_getfield(state, env, "plain");
    ASSERT_STR_EQ("https://example.test/a", lua_tostring(state, -1));
    lua_getfield(state, env, "authed");
    ASSERT_STR_EQ("authorized", lua_tostring(state, -1));

    lua_settop(state, 0);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    fr_http_set_backend(previous);
    PASS();
}

/* A previous run's cache entry must never satisfy this test, so the project
   id carries the process id and each run gets a fresh cache key. */
static void cache_test_project(char *out, size_t out_size) {
    snprintf(out, out_size, "daukle-test/verb-cache-%d", fr_test_process_id());
}

static void remove_cache_entry(const char *project, const char *version, const char *artifact) {
    char *path = NULL;
    fr_error err;
    if (fr_cache_path(project, version, artifact, &path, &err) != FR_OK) return;

    char *slash = strrchr(path, '/');
    if (slash != NULL) *slash = '\0';
    fr_test_remove_tree(path);
    free(path);
}

TEST cache_calls_the_producer_once(void) {
    fr_error err;
    fr_cache_set_enabled(1);

    char project[128];
    cache_test_project(project, sizeof project);
    const char *version = "1.0.0";
    const char *artifact = "art";

    fr_registry *registry = fr_registry_create();
    ASSERT_EQ(FR_OK, fr_lua_runtime_begin(".", registry, &err));
    lua_State *state = fr_lua_runtime_state();

    const char *verbs[] = { "cache" };
    ASSERT_EQ(FR_OK, fr_lua_verbs_push_env(state, verbs, 1, &err));
    int env = lua_gettop(state);

    char script[256];
    snprintf(script, sizeof script,
        "calls = 0\n"
        "local function make() calls = calls + 1; return 'body' end\n"
        "first = daukle.cache('%s', '%s', '%s', make)\n"
        "second = daukle.cache('%s', '%s', '%s', make)",
        project, version, artifact, project, version, artifact);
    ASSERT_EQ(FR_OK, fr_lua_run_in_env(state, script, "=t", env, &err));

    lua_getfield(state, env, "first");
    ASSERT_STR_EQ("body", lua_tostring(state, -1));
    lua_getfield(state, env, "second");
    ASSERT_STR_EQ("body", lua_tostring(state, -1));
    lua_getfield(state, env, "calls");
    ASSERT_EQ(1, (int) lua_tointeger(state, -1));

    lua_settop(state, 0);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    remove_cache_entry(project, version, artifact);
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
    RUN_TEST(fetch_returns_the_body_and_passes_headers);
    RUN_TEST(cache_calls_the_producer_once);
    GREATEST_MAIN_END();
}
