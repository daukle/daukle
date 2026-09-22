#include "greatest.h"

#include "cache.h"
#include "config_lua.h"
#include "http.h"
#include "lua_verbs.h"
#include "luax.h"
#include "registry.h"
#include "support.h"
#include "sync.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

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

TEST publish_is_accepted_in_uses_but_not_implemented(void) {
    fr_error err;
    fr_registry *registry = fr_registry_create();
    ASSERT_EQ(FR_OK, fr_lua_runtime_begin(".", registry, &err));
    lua_State *state = fr_lua_runtime_state();

    ASSERT_EQ(1, fr_lua_verbs_is_known("publish"));
    ASSERT_EQ(1, fr_lua_verbs_is_reserved("publish"));

    const char *verbs[] = { "publish" };
    ASSERT_EQ(FR_OK, fr_lua_verbs_push_env(state, verbs, 1, &err));
    int env = lua_gettop(state);

    ASSERT_EQ(FR_ERR, fr_lua_run_in_env(state, "daukle.publish('git')", "=t", env, &err));
    ASSERT(strstr(err.message, "not implemented") != NULL);
    ASSERT(strstr(err.message, "was not declared") == NULL);

    lua_settop(state, 0);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    PASS();
}

TEST tool_resolves_and_exec_runs_what_it_resolved(void) {
    fr_error err;
    fr_registry *registry = fr_registry_create();
    ASSERT_EQ(FR_OK, fr_lua_runtime_begin(".", registry, &err));
    lua_State *state = fr_lua_runtime_state();

    const char *verbs[] = { "tool", "exec" };
    ASSERT_EQ(FR_OK, fr_lua_verbs_push_env(state, verbs, 2, &err));
    int env = lua_gettop(state);

    ASSERT_EQ(FR_OK, fr_lua_run_in_env(state,
        "local t = daukle.tool('test_lua_verbs')\n"
        "result = daukle.exec(t, { '--exec-child', '0', 'hi' }, { capture = true })",
        "=t", env, &err));

    lua_getfield(state, env, "result");
    lua_getfield(state, -1, "code");
    ASSERT_EQ(0, (int) lua_tointeger(state, -1));
    lua_pop(state, 1);
    lua_getfield(state, -1, "stdout");
    ASSERT(strstr(lua_tostring(state, -1), "[hi]") != NULL);

    lua_settop(state, 0);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    PASS();
}

TEST exec_raises_on_a_nonzero_exit_by_default(void) {
    fr_error err;
    fr_registry *registry = fr_registry_create();
    ASSERT_EQ(FR_OK, fr_lua_runtime_begin(".", registry, &err));
    lua_State *state = fr_lua_runtime_state();

    const char *verbs[] = { "tool", "exec" };
    ASSERT_EQ(FR_OK, fr_lua_verbs_push_env(state, verbs, 2, &err));
    int env = lua_gettop(state);

    ASSERT_EQ(FR_ERR, fr_lua_run_in_env(state,
        "local t = daukle.tool('test_lua_verbs')\n"
        "daukle.exec(t, { '--exec-child', '4' })",
        "=t", env, &err));
    ASSERT(strstr(err.message, "exited with code 4") != NULL);

    lua_settop(state, 0);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    PASS();
}

TEST exec_returns_the_code_when_check_is_false(void) {
    fr_error err;
    fr_registry *registry = fr_registry_create();
    ASSERT_EQ(FR_OK, fr_lua_runtime_begin(".", registry, &err));
    lua_State *state = fr_lua_runtime_state();

    const char *verbs[] = { "tool", "exec" };
    ASSERT_EQ(FR_OK, fr_lua_verbs_push_env(state, verbs, 2, &err));
    int env = lua_gettop(state);

    ASSERT_EQ(FR_OK, fr_lua_run_in_env(state,
        "local t = daukle.tool('test_lua_verbs')\n"
        "code = daukle.exec(t, { '--exec-child', '4' }, { check = false }).code",
        "=t", env, &err));
    lua_getfield(state, env, "code");
    ASSERT_EQ(4, (int) lua_tointeger(state, -1));

    lua_settop(state, 0);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    PASS();
}

TEST exec_refuses_anything_that_is_not_a_tool_handle(void) {
    fr_error err;
    fr_registry *registry = fr_registry_create();
    ASSERT_EQ(FR_OK, fr_lua_runtime_begin(".", registry, &err));
    lua_State *state = fr_lua_runtime_state();

    const char *verbs[] = { "exec" };
    ASSERT_EQ(FR_OK, fr_lua_verbs_push_env(state, verbs, 1, &err));
    int env = lua_gettop(state);

    ASSERT_EQ(FR_ERR, fr_lua_run_in_env(state,
        "daukle.exec('/bin/sh', { '-c', 'echo no' })", "=t", env, &err));
    ASSERT(strstr(err.message, "must be a tool handle") != NULL);

    lua_settop(state, 0);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    PASS();
}

TEST tool_names_what_it_could_not_find(void) {
    fr_error err;
    fr_registry *registry = fr_registry_create();
    ASSERT_EQ(FR_OK, fr_lua_runtime_begin(".", registry, &err));
    lua_State *state = fr_lua_runtime_state();

    const char *verbs[] = { "tool" };
    ASSERT_EQ(FR_OK, fr_lua_verbs_push_env(state, verbs, 1, &err));
    int env = lua_gettop(state);

    ASSERT_EQ(FR_ERR, fr_lua_run_in_env(state,
        "daukle.tool('daukle-no-such-tool')", "=t", env, &err));
    ASSERT(strstr(err.message, "is not installed") != NULL);

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

TEST cache_propagates_a_read_failure_rather_than_refetching(void) {
    fr_error err;
    fr_cache_set_enabled(1);

    char project[128];
    cache_test_project(project, sizeof project);

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
        "daukle.cache('%s', '..', 'art', make)",
        project);
    ASSERT_EQ(FR_ERR, fr_lua_run_in_env(state, script, "=t", env, &err));
    ASSERT(strstr(err.message, "not a safe cache path component") != NULL);

    lua_getfield(state, env, "calls");
    ASSERT_EQ(0, (int) lua_tointeger(state, -1));

    lua_settop(state, 0);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    PASS();
}

TEST region_replaces_only_between_the_markers(void) {
    fr_error err;
    fr_registry *registry = fr_registry_create();
    ASSERT_EQ(FR_OK, fr_lua_runtime_begin(".", registry, &err));
    lua_State *state = fr_lua_runtime_state();

    const char *verbs[] = { "region" };
    ASSERT_EQ(FR_OK, fr_lua_verbs_push_env(state, verbs, 1, &err));
    int env = lua_gettop(state);

    ASSERT_EQ(FR_OK, fr_lua_run_in_env(state,
        "out = daukle.region('keep\\n# b\\nold\\n# e\\ntail\\n', '# b', '# e', 'new\\n')",
        "=t", env, &err));

    lua_getfield(state, env, "out");
    const char *out = lua_tostring(state, -1);
    ASSERT(strstr(out, "keep") != NULL);
    ASSERT(strstr(out, "tail") != NULL);
    ASSERT(strstr(out, "new") != NULL);
    ASSERT(strstr(out, "old") == NULL);

    lua_settop(state, 0);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    PASS();
}

TEST json_set_preserves_the_rest_of_the_document(void) {
    fr_error err;
    fr_registry *registry = fr_registry_create();
    ASSERT_EQ(FR_OK, fr_lua_runtime_begin(".", registry, &err));
    lua_State *state = fr_lua_runtime_state();

    const char *verbs[] = { "json_set" };
    ASSERT_EQ(FR_OK, fr_lua_verbs_push_env(state, verbs, 1, &err));
    int env = lua_gettop(state);

    ASSERT_EQ(FR_OK, fr_lua_run_in_env(state,
        "out = daukle.json_set('{\"name\":\"app\",\"dependencies\":{\"a\":\"1.0.0\"}}',"
        " 'dependencies', 'a', '2.0.0')",
        "=t", env, &err));

    lua_getfield(state, env, "out");
    const char *out = lua_tostring(state, -1);
    ASSERT(strstr(out, "\"name\":\"app\"") != NULL);
    ASSERT(strstr(out, "2.0.0") != NULL);
    ASSERT(strstr(out, "1.0.0") == NULL);

    lua_settop(state, 0);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    PASS();
}

TEST json_set_writes_a_key_containing_a_dot(void) {
    fr_error err;
    fr_registry *registry = fr_registry_create();
    ASSERT_EQ(FR_OK, fr_lua_runtime_begin(".", registry, &err));
    lua_State *state = fr_lua_runtime_state();

    const char *verbs[] = { "json_set" };
    ASSERT_EQ(FR_OK, fr_lua_verbs_push_env(state, verbs, 1, &err));
    int env = lua_gettop(state);

    ASSERT_EQ(FR_OK, fr_lua_run_in_env(state,
        "out = daukle.json_set('{\"dependencies\":{}}', 'dependencies',"
        " 'lodash.merge', '^4.6.2')",
        "=t", env, &err));

    lua_getfield(state, env, "out");
    const char *out = lua_tostring(state, -1);
    ASSERT(out != NULL);
    ASSERT(strstr(out, "lodash.merge") != NULL);
    ASSERT(strstr(out, "^4.6.2") != NULL);

    lua_settop(state, 0);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    PASS();
}

TEST json_set_removes_a_key_when_the_value_is_nil(void) {
    fr_error err;
    fr_registry *registry = fr_registry_create();
    ASSERT_EQ(FR_OK, fr_lua_runtime_begin(".", registry, &err));
    lua_State *state = fr_lua_runtime_state();

    const char *verbs[] = { "json_set" };
    ASSERT_EQ(FR_OK, fr_lua_verbs_push_env(state, verbs, 1, &err));
    int env = lua_gettop(state);

    ASSERT_EQ(FR_OK, fr_lua_run_in_env(state,
        "out = daukle.json_set('{\"dependencies\":{\"left-pad\":\"^1.0.0\"}}',"
        " 'dependencies', 'left-pad', nil)",
        "=t", env, &err));

    lua_getfield(state, env, "out");
    const char *out = lua_tostring(state, -1);
    ASSERT(out != NULL);
    ASSERT(strstr(out, "left-pad") == NULL);

    lua_settop(state, 0);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    PASS();
}

TEST json_set_writes_a_string_array_when_the_value_is_a_table(void) {
    fr_error err;
    fr_registry *registry = fr_registry_create();
    ASSERT_EQ(FR_OK, fr_lua_runtime_begin(".", registry, &err));
    lua_State *state = fr_lua_runtime_state();

    const char *verbs[] = { "json_set" };
    ASSERT_EQ(FR_OK, fr_lua_verbs_push_env(state, verbs, 1, &err));
    int env = lua_gettop(state);

    ASSERT_EQ(FR_OK, fr_lua_run_in_env(state,
        "out = daukle.json_set('{}', 'daukle.managed', 'dependencies', { 'a', 'b' })",
        "=t", env, &err));

    lua_getfield(state, env, "out");
    const char *out = lua_tostring(state, -1);
    ASSERT(out != NULL);
    ASSERT(strstr(out, "\"a\"") != NULL);
    ASSERT(strstr(out, "\"b\"") != NULL);

    lua_settop(state, 0);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    PASS();
}

TEST json_set_refuses_a_non_string_in_an_array_naming_its_position(void) {
    fr_error err;
    fr_registry *registry = fr_registry_create();
    ASSERT_EQ(FR_OK, fr_lua_runtime_begin(".", registry, &err));
    lua_State *state = fr_lua_runtime_state();

    const char *verbs[] = { "json_set" };
    ASSERT_EQ(FR_OK, fr_lua_verbs_push_env(state, verbs, 1, &err));
    int env = lua_gettop(state);

    ASSERT_EQ(FR_ERR, fr_lua_run_in_env(state,
        "daukle.json_set('{}', '', 'k', { 'a', 7 })",
        "=t", env, &err));
    ASSERT(strstr(err.message, "2") != NULL);

    lua_settop(state, 0);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    PASS();
}

TEST json_parse_reads_a_plugins_own_data_file(void) {
    fr_error err;
    fr_registry *registry = fr_registry_create();
    ASSERT_EQ(FR_OK, fr_lua_runtime_begin(".", registry, &err));
    lua_State *state = fr_lua_runtime_state();

    const char *verbs[] = { "json_parse" };
    ASSERT_EQ(FR_OK, fr_lua_verbs_push_env(state, verbs, 1, &err));
    int env = lua_gettop(state);

    ASSERT_EQ(FR_OK, fr_lua_run_in_env(state,
        "local ledger = daukle.json_parse('{\"name\":\"app\",\"daukle\":{\"managed\":"
        "{\"dependencies\":[\"left-pad\",\"lodash\"]}}}')\n"
        "name = ledger.name\n"
        "owned = ledger.daukle.managed.dependencies\n"
        "first = owned[1]\n"
        "count = #owned",
        "=t", env, &err));

    lua_getfield(state, env, "name");
    ASSERT_STR_EQ("app", lua_tostring(state, -1));
    lua_getfield(state, env, "first");
    ASSERT_STR_EQ("left-pad", lua_tostring(state, -1));
    lua_getfield(state, env, "count");
    ASSERT_EQ(2, (int) lua_tointeger(state, -1));

    lua_settop(state, 0);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    PASS();
}

TEST json_parse_names_where_the_json_stops(void) {
    fr_error err;
    fr_registry *registry = fr_registry_create();
    ASSERT_EQ(FR_OK, fr_lua_runtime_begin(".", registry, &err));
    lua_State *state = fr_lua_runtime_state();

    const char *verbs[] = { "json_parse" };
    ASSERT_EQ(FR_OK, fr_lua_verbs_push_env(state, verbs, 1, &err));
    int env = lua_gettop(state);

    ASSERT_EQ(FR_ERR, fr_lua_run_in_env(state,
        "daukle.json_parse('{\"dependencies\": }')", "=t", env, &err));
    ASSERT(strstr(err.message, "json_parse") != NULL);
    ASSERT(strstr(err.message, "byte") != NULL);

    lua_settop(state, 0);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    PASS();
}

/* json_parse exists because a plugin's own data file is not a daukle manifest:
   no config format reads json any more, so daukle.parse must refuse the very
   text json_parse accepts. */
TEST json_parse_reads_what_parse_now_refuses(void) {
    fr_error err;
    fr_registry *registry = NULL;
    ASSERT_EQ(FR_OK, fr_build_registry(&registry, &err));
    ASSERT_EQ(FR_OK, fr_lua_runtime_begin(".", registry, &err));
    lua_State *state = fr_lua_runtime_state();

    const char *verbs[] = { "parse", "json_parse" };
    ASSERT_EQ(FR_OK, fr_lua_verbs_push_env(state, verbs, 2, &err));
    int env = lua_gettop(state);

    ASSERT_EQ(FR_ERR, fr_lua_run_in_env(state,
        "daukle.parse('{\"name\":\"app\"}', 'package.json')", "=t", env, &err));
    ASSERT(strstr(err.message, "json") != NULL);

    ASSERT_EQ(FR_OK, fr_lua_run_in_env(state,
        "name = daukle.json_parse('{\"name\":\"app\"}').name", "=t", env, &err));
    lua_getfield(state, env, "name");
    ASSERT_STR_EQ("app", lua_tostring(state, -1));

    lua_settop(state, 0);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    PASS();
}

TEST parse_reads_toml_through_the_config_table(void) {
    fr_error err;
    fr_registry *registry = NULL;
    ASSERT_EQ(FR_OK, fr_build_registry(&registry, &err));
    ASSERT_EQ(FR_OK, fr_lua_runtime_begin(".", registry, &err));
    lua_State *state = fr_lua_runtime_state();

    const char *verbs[] = { "parse" };
    ASSERT_EQ(FR_OK, fr_lua_verbs_push_env(state, verbs, 1, &err));
    int env = lua_gettop(state);

    ASSERT_EQ(FR_OK, fr_lua_run_in_env(state,
        "local t = daukle.parse('schema = 1\\nproject = \"p/q\"\\nversion = \"1.0.0\"\\n',"
        " 'daukle.toml')\n"
        "name = t.project",
        "=t", env, &err));

    lua_getfield(state, env, "name");
    ASSERT_STR_EQ("p/q", lua_tostring(state, -1));

    lua_settop(state, 0);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    PASS();
}

TEST parse_refuses_an_executable_config_format(void) {
    fr_error err;
    fr_registry *registry = NULL;
    ASSERT_EQ(FR_OK, fr_build_registry(&registry, &err));
    ASSERT_EQ(FR_OK, fr_lua_runtime_begin(".", registry, &err));
    lua_State *state = fr_lua_runtime_state();

    const char *verbs[] = { "parse" };
    ASSERT_EQ(FR_OK, fr_lua_verbs_push_env(state, verbs, 1, &err));
    int env = lua_gettop(state);

    ASSERT_EQ(FR_ERR, fr_lua_run_in_env(state,
        "daukle.parse('error(\"executed\")', 'daukle.lua')", "=t", env, &err));
    ASSERT(strstr(err.message, "executable") != NULL);
    ASSERT(strstr(err.message, "executed") == NULL);

    lua_settop(state, 0);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[1], "--exec-child") == 0) {
#ifdef _WIN32
        _setmode(_fileno(stdout), _O_BINARY);
#endif
        int code = atoi(argv[2]);
        for (int index = 3; index < argc; index++) printf("[%s]\n", argv[index]);
        fflush(stdout);
        return code;
    }
    fr_test_prepend_to_path_dir_of(argv[0]);

    GREATEST_MAIN_BEGIN();
    RUN_TEST(a_declared_verb_is_present);
    RUN_TEST(an_undeclared_verb_raises_naming_itself);
    RUN_TEST(registration_functions_are_always_present);
    RUN_TEST(publish_is_accepted_in_uses_but_not_implemented);
    RUN_TEST(tool_resolves_and_exec_runs_what_it_resolved);
    RUN_TEST(exec_raises_on_a_nonzero_exit_by_default);
    RUN_TEST(exec_returns_the_code_when_check_is_false);
    RUN_TEST(exec_refuses_anything_that_is_not_a_tool_handle);
    RUN_TEST(tool_names_what_it_could_not_find);
    RUN_TEST(env_reads_a_variable_and_nil_for_an_absent_one);
    RUN_TEST(read_refuses_a_path_outside_the_base_directory);
    RUN_TEST(read_returns_the_text_of_a_file_inside_the_base_directory);
    RUN_TEST(fetch_returns_the_body_and_passes_headers);
    RUN_TEST(cache_calls_the_producer_once);
    RUN_TEST(cache_propagates_a_read_failure_rather_than_refetching);
    RUN_TEST(region_replaces_only_between_the_markers);
    RUN_TEST(json_set_preserves_the_rest_of_the_document);
    RUN_TEST(json_set_writes_a_key_containing_a_dot);
    RUN_TEST(json_set_removes_a_key_when_the_value_is_nil);
    RUN_TEST(json_set_writes_a_string_array_when_the_value_is_a_table);
    RUN_TEST(json_set_refuses_a_non_string_in_an_array_naming_its_position);
    RUN_TEST(json_parse_reads_a_plugins_own_data_file);
    RUN_TEST(json_parse_names_where_the_json_stops);
    RUN_TEST(json_parse_reads_what_parse_now_refuses);
    RUN_TEST(parse_reads_toml_through_the_config_table);
    RUN_TEST(parse_refuses_an_executable_config_format);
    GREATEST_MAIN_END();
}
