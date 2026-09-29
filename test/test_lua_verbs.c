#include "greatest.h"

#include "cache.h"
#include "config_lua.h"
#include "derived.h"
#include "http.h"
#include "lua_verbs.h"
#include "luax.h"
#include "provision.h"
#include "registry.h"
#include "support.h"
#include "sync.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <sys/stat.h>
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

TEST tool_refuses_a_name_that_climbs_out(void) {
    fr_error err;
    fr_registry *registry = fr_registry_create();
    ASSERT_EQ(FR_OK, fr_lua_runtime_begin(".", registry, &err));
    lua_State *state = fr_lua_runtime_state();

    const char *verbs[] = { "tool" };
    ASSERT_EQ(FR_OK, fr_lua_verbs_push_env(state, verbs, 1, &err));
    int env = lua_gettop(state);

    ASSERT_EQ(FR_ERR, fr_lua_run_in_env(state,
        "daukle.tool('../thing')", "=t", env, &err));
    ASSERT(strstr(err.message, "named, not pathed") != NULL);

    lua_settop(state, 0);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    PASS();
}

TEST tool_refuses_an_absolute_looking_name(void) {
    fr_error err;
    fr_registry *registry = fr_registry_create();
    ASSERT_EQ(FR_OK, fr_lua_runtime_begin(".", registry, &err));
    lua_State *state = fr_lua_runtime_state();

    const char *verbs[] = { "tool" };
    ASSERT_EQ(FR_OK, fr_lua_verbs_push_env(state, verbs, 1, &err));
    int env = lua_gettop(state);

    ASSERT_EQ(FR_ERR, fr_lua_run_in_env(state,
        "daukle.tool('/etc/passwd')", "=t", env, &err));
    ASSERT(strstr(err.message, "named, not pathed") != NULL);

    lua_settop(state, 0);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    PASS();
}

TEST tool_refuses_a_name_with_an_interior_separator(void) {
    fr_error err;
    fr_registry *registry = fr_registry_create();
    ASSERT_EQ(FR_OK, fr_lua_runtime_begin(".", registry, &err));
    lua_State *state = fr_lua_runtime_state();

    const char *verbs[] = { "tool" };
    ASSERT_EQ(FR_OK, fr_lua_verbs_push_env(state, verbs, 1, &err));
    int env = lua_gettop(state);

    ASSERT_EQ(FR_ERR, fr_lua_run_in_env(state,
        "daukle.tool('sub/tool')", "=t", env, &err));
    ASSERT(strstr(err.message, "named, not pathed") != NULL);

    lua_settop(state, 0);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    PASS();
}

/* Contains no '/' or '\\' at all, so only the reused fr_lua_sandbox_climbs_out
   rule can catch this; strpbrk alone would wave it through. */
TEST tool_refuses_a_bare_climb(void) {
    fr_error err;
    fr_registry *registry = fr_registry_create();
    ASSERT_EQ(FR_OK, fr_lua_runtime_begin(".", registry, &err));
    lua_State *state = fr_lua_runtime_state();

    const char *verbs[] = { "tool" };
    ASSERT_EQ(FR_OK, fr_lua_verbs_push_env(state, verbs, 1, &err));
    int env = lua_gettop(state);

    ASSERT_EQ(FR_ERR, fr_lua_run_in_env(state,
        "daukle.tool('..')", "=t", env, &err));
    ASSERT(strstr(err.message, "named, not pathed") != NULL);

    lua_settop(state, 0);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    PASS();
}

/* Also separator-free: only fr_lua_sandbox_climbs_out's drive-letter clause
   catches this one, pinning that clause as load bearing for daukle.tool too. */
TEST tool_refuses_a_drive_letter_prefix(void) {
    fr_error err;
    fr_registry *registry = fr_registry_create();
    ASSERT_EQ(FR_OK, fr_lua_runtime_begin(".", registry, &err));
    lua_State *state = fr_lua_runtime_state();

    const char *verbs[] = { "tool" };
    ASSERT_EQ(FR_OK, fr_lua_verbs_push_env(state, verbs, 1, &err));
    int env = lua_gettop(state);

    ASSERT_EQ(FR_ERR, fr_lua_run_in_env(state,
        "daukle.tool('c:cmd')", "=t", env, &err));
    ASSERT(strstr(err.message, "named, not pathed") != NULL);

    lua_settop(state, 0);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    PASS();
}

/* The spec's own example is daukle.tool("gcc", ">=13"). Ignoring it would hand
   back an unversioned handle to an author who believes a constraint holds. */
TEST tool_refuses_a_version_constraint_it_cannot_honour(void) {
    fr_error err;
    fr_registry *registry = fr_registry_create();
    ASSERT_EQ(FR_OK, fr_lua_runtime_begin(".", registry, &err));
    lua_State *state = fr_lua_runtime_state();

    const char *verbs[] = { "tool" };
    ASSERT_EQ(FR_OK, fr_lua_verbs_push_env(state, verbs, 1, &err));
    int env = lua_gettop(state);

    ASSERT_EQ(FR_ERR, fr_lua_run_in_env(state,
        "daukle.tool('test_lua_verbs', '>=13')", "=t", env, &err));
    ASSERT(strstr(err.message, "version constraints are not implemented") != NULL);

    lua_settop(state, 0);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    PASS();
}

/* A truncated name would name a different tool than the one resolved, so the
   handle refuses to be built rather than quietly holding a shortened one. */
TEST tool_refuses_a_name_too_long_for_a_handle(void) {
    fr_error err;
    fr_registry *registry = fr_registry_create();
    ASSERT_EQ(FR_OK, fr_lua_runtime_begin(".", registry, &err));
    lua_State *state = fr_lua_runtime_state();

    const char *verbs[] = { "tool" };
    ASSERT_EQ(FR_OK, fr_lua_verbs_push_env(state, verbs, 1, &err));
    int env = lua_gettop(state);

    ASSERT_EQ(FR_ERR, fr_lua_run_in_env(state,
        "daukle.tool(string.rep('t', 200))", "=t", env, &err));
    ASSERT(strstr(err.message, "too long") != NULL);

    lua_settop(state, 0);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    PASS();
}

/* getmetatable is a base global and the handle metatable is shared by every
   plugin, so handing it out would be a table one plugin could rewrite for all. */
TEST the_tool_handle_metatable_cannot_be_reached_from_a_plugin(void) {
    fr_error err;
    fr_registry *registry = fr_registry_create();
    ASSERT_EQ(FR_OK, fr_lua_runtime_begin(".", registry, &err));
    lua_State *state = fr_lua_runtime_state();

    const char *verbs[] = { "tool" };
    ASSERT_EQ(FR_OK, fr_lua_verbs_push_env(state, verbs, 1, &err));
    int env = lua_gettop(state);

    ASSERT_EQ(FR_OK, fr_lua_run_in_env(state,
        "local t = daukle.tool('test_lua_verbs')\n"
        "hidden = getmetatable(t) == false",
        "=t", env, &err));
    lua_getfield(state, env, "hidden");
    ASSERT(lua_toboolean(state, -1));
    lua_pop(state, 1);

    ASSERT_EQ(FR_ERR, fr_lua_run_in_env(state,
        "local t = daukle.tool('test_lua_verbs')\n"
        "getmetatable(t).__index = 'mine'",
        "=t", env, &err));
    ASSERT(strstr(err.message, "attempt to index a boolean") != NULL);

    lua_settop(state, 0);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    PASS();
}

/* Proves two things at once: cwd is honoured, and the resolved program is
   absolute, since a relative one would no longer be found from the new cwd. */
TEST exec_runs_in_a_cwd_inside_the_project_directory(void) {
    fr_error err;
    fr_registry *registry = fr_registry_create();
    ASSERT_EQ(FR_OK, fr_lua_runtime_begin(".", registry, &err));
    lua_State *state = fr_lua_runtime_state();

    const char *verbs[] = { "tool", "exec" };
    ASSERT_EQ(FR_OK, fr_lua_verbs_push_env(state, verbs, 2, &err));
    int env = lua_gettop(state);

    ASSERT_EQ(FR_OK, fr_lua_run_in_env(state,
        "local t = daukle.tool('test_lua_verbs')\n"
        "result = daukle.exec(t, { '--exec-child', '0', 'in-test' }, "
        "{ capture = true, cwd = 'test' })",
        "=t", env, &err));

    lua_getfield(state, env, "result");
    lua_getfield(state, -1, "stdout");
    ASSERT(strstr(lua_tostring(state, -1), "[in-test]") != NULL);

    lua_settop(state, 0);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    PASS();
}

TEST exec_refuses_a_cwd_outside_the_project_directory(void) {
    fr_error err;
    fr_registry *registry = fr_registry_create();
    ASSERT_EQ(FR_OK, fr_lua_runtime_begin(".", registry, &err));
    lua_State *state = fr_lua_runtime_state();

    const char *verbs[] = { "tool", "exec" };
    ASSERT_EQ(FR_OK, fr_lua_verbs_push_env(state, verbs, 2, &err));
    int env = lua_gettop(state);

    ASSERT_EQ(FR_ERR, fr_lua_run_in_env(state,
        "local t = daukle.tool('test_lua_verbs')\n"
        "daukle.exec(t, { '--exec-child', '0' }, { cwd = '../..' })",
        "=t", env, &err));
    ASSERT(strstr(err.message, "outside the project directory") != NULL);

    lua_settop(state, 0);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    PASS();
}

/* Regression for the missing lua_checkstack this file's own json_set_array
   already knew to call: an argv past LUA_MINSTACK (20) must not silently
   overrun the Lua stack, so this pushes comfortably past it and checks the
   whole vector, not just the exit code, arrived intact. */
TEST exec_accepts_an_argv_well_past_the_guaranteed_lua_stack(void) {
    fr_error err;
    fr_registry *registry = fr_registry_create();
    ASSERT_EQ(FR_OK, fr_lua_runtime_begin(".", registry, &err));
    lua_State *state = fr_lua_runtime_state();

    const char *verbs[] = { "tool", "exec" };
    ASSERT_EQ(FR_OK, fr_lua_verbs_push_env(state, verbs, 2, &err));
    int env = lua_gettop(state);

    ASSERT_EQ(FR_OK, fr_lua_run_in_env(state,
        "local t = daukle.tool('test_lua_verbs')\n"
        "local argv = { '--exec-child', '0' }\n"
        "for i = 1, 254 do argv[#argv + 1] = 'a' .. i end\n"
        "result = daukle.exec(t, argv, { capture = true })",
        "=t", env, &err));

    lua_getfield(state, env, "result");
    lua_getfield(state, -1, "code");
    ASSERT_EQ(0, (int) lua_tointeger(state, -1));
    lua_pop(state, 1);
    lua_getfield(state, -1, "stdout");
    const char *stdout_text = lua_tostring(state, -1);
    ASSERT(strstr(stdout_text, "[a1]") != NULL);
    ASSERT(strstr(stdout_text, "[a254]") != NULL);

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

/* Runs source in the library environment and reports what came back: 0 on
   success with the returned string in out_text, 1 on a lua error with the
   message there instead. */
static int run_in_library_env(const char *source, char *out_text, size_t size) {
    lua_State *state = fr_lua_runtime_state();
    int top = lua_gettop(state);
    fr_error err;
    out_text[0] = '\0';

    if (fr_lua_verbs_push_library_env(state, "gradle", "lib/jdk", &err) != FR_OK) {
        snprintf(out_text, size, "%s", err.message);
        lua_settop(state, top);
        return 1;
    }
    int env = lua_gettop(state);
    int status = fr_lua_run_in_env(state, source, "@test", env, &err);
    snprintf(out_text, size, "%s", status == FR_OK ? "ok" : err.message);
    lua_settop(state, top);
    return status == FR_OK ? 0 : 1;
}

TEST the_library_environment_has_no_registration_functions(void) {
    static char message[512];
    fr_error err;
    fr_registry *registry = NULL;
    int began = fr_build_registry(&registry, &err) == FR_OK
              && fr_lua_runtime_begin(".", registry, &err) == FR_OK;
    int failed = began
        ? run_in_library_env("daukle.toolchain{ name = \"x\" }", message, sizeof message)
        : -1;
    if (!began) snprintf(message, sizeof message, "%s", err.message);
    fr_lua_runtime_shutdown();
    fr_registry_destroy(registry);

    ASSERT(began);
    ASSERT_EQm(message, 1, failed);
    ASSERTm(message, strstr(message, "across a plugin boundary") != NULL);
    PASS();
}

TEST the_library_environment_has_no_verbs(void) {
    static char message[512];
    fr_error err;
    fr_registry *registry = NULL;
    int began = fr_build_registry(&registry, &err) == FR_OK
              && fr_lua_runtime_begin(".", registry, &err) == FR_OK;
    int failed = began ? run_in_library_env("daukle.exec{}", message, sizeof message) : -1;
    if (!began) snprintf(message, sizeof message, "%s", err.message);
    fr_lua_runtime_shutdown();
    fr_registry_destroy(registry);

    ASSERT(began);
    ASSERT_EQm(message, 1, failed);
    ASSERTm(message, strstr(message, "daukle.exec is not available") != NULL);
    ASSERTm(message, strstr(message, "\"lib/jdk\"") != NULL);
    ASSERTm(message, strstr(message, "\"gradle\"") != NULL);
    PASS();
}

TEST the_library_environment_keeps_the_base_globals(void) {
    static char message[512];
    fr_error err;
    fr_registry *registry = NULL;
    int began = fr_build_registry(&registry, &err) == FR_OK
              && fr_lua_runtime_begin(".", registry, &err) == FR_OK;
    int ran = began
        ? run_in_library_env("assert(string.rep(\"a\", 2) == \"aa\")", message, sizeof message)
        : -1;
    if (!began) snprintf(message, sizeof message, "%s", err.message);
    fr_lua_runtime_shutdown();
    fr_registry_destroy(registry);

    ASSERT(began);
    ASSERT_EQm(message, 0, ran);
    PASS();
}

/* Regression for a shared-statics defect: an older library environment's
   __index must keep naming its own dependent and member after a newer one is
   built while the older is still live, since daukle.require can nest one
   library environment inside another. */
TEST an_older_library_environments_raiser_survives_a_newer_one_being_built(void) {
    static char message[512];
    fr_error err;
    fr_registry *registry = NULL;
    lua_State *state = NULL;
    int began = fr_build_registry(&registry, &err) == FR_OK
              && fr_lua_runtime_begin(".", registry, &err) == FR_OK;
    if (began) state = fr_lua_runtime_state();

    int built_first = began
        && fr_lua_verbs_push_library_env(state, "alpha", "a/one", &err) == FR_OK;
    int env1 = built_first ? lua_gettop(state) : 0;
    int built_second = built_first
        && fr_lua_verbs_push_library_env(state, "beta", "b/two", &err) == FR_OK;

    int status = FR_ERR;
    if (built_second) {
        status = fr_lua_run_in_env(state, "daukle.toolchain{ name = \"x\" }", "@test", env1, &err);
    }
    snprintf(message, sizeof message, "%s",
            built_second ? (status == FR_OK ? "ok" : err.message) : err.message);

    if (began) lua_settop(state, 0);
    fr_lua_runtime_shutdown();
    fr_registry_destroy(registry);

    ASSERT(began);
    ASSERT(built_second);
    ASSERT_EQm(message, FR_ERR, status);
    ASSERTm(message, strstr(message, "\"a/one\"") != NULL);
    ASSERTm(message, strstr(message, "\"alpha\"") != NULL);
    ASSERTm(message, strstr(message, "\"b/two\"") == NULL);
    ASSERTm(message, strstr(message, "\"beta\"") == NULL);
    PASS();
}

#define PROVISION_PIN "1111111111111111111111111111111111111111111111111111111111111111"
#define PROVISION_URL "http://127.0.0.1:1/toolchain.tar"

static void use_private_provision_cache(const char *name, char *out, size_t size) {
    snprintf(out, size, "%s/verbs-provision-%s-%d", fr_test_temp_base(), name,
             fr_test_process_id());
    fr_test_remove_tree(out);
    fr_test_make_directory(out);
    fr_test_set_env("DAUKLE_CACHE_DIR", out);
}

static int write_stub_program(const char *path) {
    FILE *file = fopen(path, "wb");
    if (file == NULL) return 0;
    fputs("stub", file);
    fclose(file);
#ifndef _WIN32
    chmod(path, 0755);
#endif
    return 1;
}

/* Lays the tree PROVISION_PIN names down by hand, so daukle.provision finds it
   already cached and no test of the root handle's own rules needs a server. */
static int prepare_provisioned_root(char *root, size_t size) {
    fr_error err;
    if (fr_provision_root_path(PROVISION_PIN, root, size, &err) != FR_OK) return 0;
    fr_cache_make_directories(root);

    char directory[1024];
    snprintf(directory, sizeof directory, "%s/bin", root);
    fr_test_make_directory(directory);

    char member[1024];
    snprintf(member, sizeof member, "%s/bin/java.exe", root);
    if (!write_stub_program(member)) return 0;
    snprintf(member, sizeof member, "%s/bin/thing.bat", root);
    return write_stub_program(member);
}

static lua_State *begin_provision_env(fr_registry *registry, int *out_env) {
    fr_error err;
    if (fr_lua_runtime_begin(".", registry, &err) != FR_OK) return NULL;
    lua_State *state = fr_lua_runtime_state();
    const char *verbs[] = { "provision" };
    if (fr_lua_verbs_push_env(state, verbs, 1, &err) != FR_OK) return NULL;
    *out_env = lua_gettop(state);
    return state;
}

TEST provision_refuses_an_unknown_key_by_name(void) {
    static char message[512];
    fr_registry *registry = fr_registry_create();
    int env = 0;
    lua_State *state = begin_provision_env(registry, &env);

    fr_error err;
    err.message[0] = '\0';
    int refused = state != NULL
        && fr_lua_run_in_env(state,
               "daukle.provision{ url = '" PROVISION_URL "', sha256 = '" PROVISION_PIN "',"
               " version = '21' }", "=t", env, &err) == FR_ERR;
    snprintf(message, sizeof message, "%s", err.message);

    if (state != NULL) lua_settop(state, 0);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();

    ASSERT(refused);
    ASSERTm(message, strstr(message, "\"version\"") != NULL);
    PASS();
}

TEST provision_refuses_a_missing_sha256(void) {
    static char message[512];
    fr_registry *registry = fr_registry_create();
    int env = 0;
    lua_State *state = begin_provision_env(registry, &env);

    fr_error err;
    err.message[0] = '\0';
    int refused = state != NULL
        && fr_lua_run_in_env(state, "daukle.provision{ url = '" PROVISION_URL "' }", "=t", env,
                             &err) == FR_ERR;
    snprintf(message, sizeof message, "%s", err.message);

    if (state != NULL) lua_settop(state, 0);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();

    ASSERT(refused);
    ASSERTm(message, strstr(message, "no unpinned form") != NULL);
    PASS();
}

TEST provision_reads_its_fields_raw(void) {
    static char message[512];
    static const char *const chunk =
        "consulted = 0\n"
        "local hidden = setmetatable({ url = '" PROVISION_URL "' }, {\n"
        "  __index = function(_, key) consulted = consulted + 1; return '" PROVISION_PIN "' end\n"
        "})\n"
        "daukle.provision(hidden)\n";

    fr_registry *registry = fr_registry_create();
    int env = 0;
    lua_State *state = begin_provision_env(registry, &env);

    fr_error err;
    err.message[0] = '\0';
    int refused = state != NULL
        && fr_lua_run_in_env(state, chunk, "=t", env, &err) == FR_ERR;
    snprintf(message, sizeof message, "%s", err.message);

    int consulted = -1;
    if (state != NULL) {
        lua_getfield(state, env, "consulted");
        consulted = (int) lua_tointeger(state, -1);
        lua_settop(state, 0);
    }
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();

    ASSERT(refused);
    ASSERT_EQm("a field was read through __index", 0, consulted);
    ASSERTm(message, strstr(message, "sha256") != NULL);
    PASS();
}

TEST a_root_handle_refuses_a_member_that_climbs_out(void) {
    static char climb_message[512];
    static char drive_message[512];
    char cache[1024];
    char root[1024];
    use_private_provision_cache("climb", cache, sizeof cache);
    int prepared = prepare_provisioned_root(root, sizeof root);

    fr_registry *registry = fr_registry_create();
    int env = 0;
    lua_State *state = begin_provision_env(registry, &env);

    fr_error err;
    err.message[0] = '\0';
    int rooted = state != NULL && prepared
        && fr_lua_run_in_env(state,
               "root = daukle.provision{ url = '" PROVISION_URL "', sha256 = '" PROVISION_PIN "' }",
               "=t", env, &err) == FR_OK;

    int climb_refused = rooted
        && fr_lua_run_in_env(state, "root:tool('../../../windows/system32/cmd')", "=t", env,
                             &err) == FR_ERR;
    snprintf(climb_message, sizeof climb_message, "%s", err.message);

    int drive_refused = rooted
        && fr_lua_run_in_env(state, "root:tool('c:/x')", "=t", env, &err) == FR_ERR;
    snprintf(drive_message, sizeof drive_message, "%s", err.message);

    if (state != NULL) lua_settop(state, 0);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    fr_test_remove_tree(cache);

    ASSERT(prepared);
    ASSERT(rooted);
    ASSERT(climb_refused);
    ASSERTm(climb_message, strstr(climb_message, "climbs out") != NULL);
    ASSERT(drive_refused);
    ASSERTm(drive_message, strstr(drive_message, "climbs out") != NULL);
    PASS();
}

TEST a_root_handle_refuses_a_batch_file(void) {
    static char message[512];
    char cache[1024];
    char root[1024];
    use_private_provision_cache("batch", cache, sizeof cache);
    int prepared = prepare_provisioned_root(root, sizeof root);

    fr_registry *registry = fr_registry_create();
    int env = 0;
    lua_State *state = begin_provision_env(registry, &env);

    fr_error err;
    err.message[0] = '\0';
    int rooted = state != NULL && prepared
        && fr_lua_run_in_env(state,
               "root = daukle.provision{ url = '" PROVISION_URL "', sha256 = '" PROVISION_PIN "' }",
               "=t", env, &err) == FR_OK;

    int refused = rooted
        && fr_lua_run_in_env(state, "root:tool('bin/thing.bat')", "=t", env, &err) == FR_ERR;
    snprintf(message, sizeof message, "%s", err.message);

    if (state != NULL) lua_settop(state, 0);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    fr_test_remove_tree(cache);

    ASSERT(prepared);
    ASSERT(rooted);
    ASSERT(refused);
    ASSERTm(message, strstr(message, "cannot run a batch file") != NULL);
    PASS();
}

TEST a_root_handle_does_not_guess_an_extension(void) {
    static char message[512];
    char cache[1024];
    char root[1024];
    use_private_provision_cache("extension", cache, sizeof cache);
    int prepared = prepare_provisioned_root(root, sizeof root);

    fr_registry *registry = fr_registry_create();
    int env = 0;
    lua_State *state = begin_provision_env(registry, &env);

    fr_error err;
    err.message[0] = '\0';
    int rooted = state != NULL && prepared
        && fr_lua_run_in_env(state,
               "root = daukle.provision{ url = '" PROVISION_URL "', sha256 = '" PROVISION_PIN "' }",
               "=t", env, &err) == FR_OK;

    int named_exactly = rooted
        && fr_lua_run_in_env(state, "exact = root:tool('bin/java.exe')", "=t", env, &err) == FR_OK;
    int guessed = named_exactly
        && fr_lua_run_in_env(state, "root:tool('bin/java')", "=t", env, &err) == FR_ERR;
    snprintf(message, sizeof message, "%s", err.message);

    if (state != NULL) lua_settop(state, 0);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    fr_test_remove_tree(cache);

    ASSERT(prepared);
    ASSERT(rooted);
    ASSERT(named_exactly);
    ASSERT(guessed);
    ASSERTm(message, strstr(message, "bin/java") != NULL);
    PASS();
}

TEST provision_is_refused_while_generating(void) {
    static char message[512];
    static const char *const chunk =
        "daukle.toolchain{ name = 'stub', generate = function()\n"
        "  daukle.provision{ url = '" PROVISION_URL "', sha256 = '" PROVISION_PIN "' }\n"
        "  return {}\n"
        "end }\n";

    fr_registry *registry = fr_registry_create();
    fr_error err;
    err.message[0] = '\0';
    const char *verbs[] = { "provision" };
    int loaded = fr_lua_runtime_begin(".", registry, &err) == FR_OK
              && fr_lua_plugin_load(chunk, strlen(chunk), "stub.lua", verbs, 1, NULL, NULL,
                                    &err) == FR_OK;
    const fr_toolchain_plugin *plugin =
        loaded ? fr_registry_toolchain(registry, "daukle.toolchain/stub") : NULL;

    fr_toolchain toolchain;
    memset(&toolchain, 0, sizeof toolchain);
    toolchain.name = (char *) "stub";

    fr_generated_file *files = NULL;
    size_t file_count = 0;
    int generated = FR_OK;
    message[0] = '\0';
    if (plugin != NULL) {
        fr_error generate_err;
        generate_err.message[0] = '\0';
        generated = plugin->generate(plugin->state, &toolchain, "acme/app", "1.0.0",
                                     "derived/stub", NULL, 0, &files, &file_count, &generate_err);
        snprintf(message, sizeof message, "%s", generate_err.message);
    }

    fr_derived_free_files(files, file_count);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();

    ASSERT(loaded);
    ASSERT(plugin != NULL);
    ASSERT_EQm(message, FR_ERR, generated);
    ASSERTm(message, strstr(message, "is not available while generating") != NULL);
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
    RUN_TEST(tool_refuses_a_name_that_climbs_out);
    RUN_TEST(tool_refuses_an_absolute_looking_name);
    RUN_TEST(tool_refuses_a_name_with_an_interior_separator);
    RUN_TEST(tool_refuses_a_bare_climb);
    RUN_TEST(tool_refuses_a_drive_letter_prefix);
    RUN_TEST(tool_refuses_a_version_constraint_it_cannot_honour);
    RUN_TEST(tool_refuses_a_name_too_long_for_a_handle);
    RUN_TEST(the_tool_handle_metatable_cannot_be_reached_from_a_plugin);
    RUN_TEST(exec_runs_in_a_cwd_inside_the_project_directory);
    RUN_TEST(exec_refuses_a_cwd_outside_the_project_directory);
    RUN_TEST(exec_accepts_an_argv_well_past_the_guaranteed_lua_stack);
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
    RUN_TEST(the_library_environment_has_no_registration_functions);
    RUN_TEST(the_library_environment_has_no_verbs);
    RUN_TEST(the_library_environment_keeps_the_base_globals);
    RUN_TEST(an_older_library_environments_raiser_survives_a_newer_one_being_built);
    RUN_TEST(provision_refuses_an_unknown_key_by_name);
    RUN_TEST(provision_refuses_a_missing_sha256);
    RUN_TEST(provision_reads_its_fields_raw);
    RUN_TEST(a_root_handle_refuses_a_member_that_climbs_out);
    RUN_TEST(a_root_handle_refuses_a_batch_file);
    RUN_TEST(a_root_handle_does_not_guess_an_extension);
    RUN_TEST(provision_is_refused_while_generating);
    GREATEST_MAIN_END();
}
