#include "greatest.h"
#include "cache/cache.h"
#include "config/config.h"
#include "config/config_lua.h"
#include "config/manifest.h"
#include "net/http.h"
#include "plugin/plugin_deps.h"
#include "plugin/plugin_modules.h"
#include "plugin/plugins.h"
#include "plugin/registry.h"
#include "project/derived.h"
#include "project/region.h"
#include "project/sync.h"
#include "util/error.h"
#include "util/sha256.h"
#include "support.h"
#include "exec/toolreport.h"

#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

TEST a_script_beside_a_toml_manifest_changes_it(void) {
    fr_error err;
    fr_registry *registry = NULL;
    ASSERT_EQ(FR_OK, fr_build_registry(&registry, &err));
    fr_manifest manifest;
    ASSERT_EQ(FR_OK, fr_config_load_file("test/fixtures/lua-overlay/daukle.toml",
                                         registry, &manifest, &err));
    ASSERT_EQ(2, manifest.self.version.major);
    ASSERT(fr_project_module(&manifest.self, "generated") != NULL);
    fr_manifest_free(&manifest);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    PASS();
}

TEST a_failing_script_names_its_file(void) {
    fr_error err;
    fr_registry *registry = NULL;
    fr_build_registry(&registry, &err);
    fr_manifest manifest;
    ASSERT_EQ(FR_ERR, fr_config_load_file("test/fixtures/lua-broken/daukle.toml",
                                          registry, &manifest, &err));
    ASSERT(strstr(err.message, "daukle.lua") != NULL);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    PASS();
}

static char last_logged_message[256];
static int log_sink_called = 0;

static void record_log(const char *message) {
    snprintf(last_logged_message, sizeof last_logged_message, "%s", message);
    log_sink_called = 1;
}

TEST daukle_log_routes_through_the_caller_supplied_sink(void) {
    fr_error err;
    fr_registry *registry = NULL;
    ASSERT_EQ(FR_OK, fr_build_registry(&registry, &err));

    log_sink_called = 0;
    fr_lua_set_log_sink(record_log);

    fr_manifest manifest;
    ASSERT_EQ(FR_OK, fr_config_load_file("test/fixtures/lua-log/daukle.toml",
                                         registry, &manifest, &err));
    ASSERT(log_sink_called);
    ASSERT(strstr(last_logged_message, "hello from a config script") != NULL);

    fr_lua_set_log_sink(NULL);
    fr_manifest_free(&manifest);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    PASS();
}

TEST an_untouched_empty_array_survives_the_round_trip(void) {
    fr_error err;
    fr_registry *registry = NULL;
    ASSERT_EQ(FR_OK, fr_build_registry(&registry, &err));
    fr_manifest manifest;
    ASSERT_EQ(FR_OK, fr_config_load_file("test/fixtures/lua-array-untouched/daukle.toml",
                                         registry, &manifest, &err));
    const fr_module *core = fr_project_module(&manifest.self, "core");
    ASSERT(core != NULL);
    ASSERT_EQ(0, (int) core->requires_count);
    fr_manifest_free(&manifest);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    PASS();
}

TEST a_script_clearing_an_array_to_an_empty_table_still_yields_an_array(void) {
    fr_error err;
    fr_registry *registry = NULL;
    ASSERT_EQ(FR_OK, fr_build_registry(&registry, &err));
    fr_manifest manifest;
    ASSERT_EQ(FR_OK, fr_config_load_file("test/fixtures/lua-array-cleared/daukle.toml",
                                         registry, &manifest, &err));
    const fr_module *core = fr_project_module(&manifest.self, "core");
    ASSERT(core != NULL);
    ASSERT_EQ(0, (int) core->requires_count);
    fr_manifest_free(&manifest);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    PASS();
}

TEST a_script_clearing_an_object_to_an_empty_table_stays_an_object(void) {
    fr_error err;
    fr_registry *registry = NULL;
    ASSERT_EQ(FR_OK, fr_build_registry(&registry, &err));
    fr_manifest manifest;
    ASSERT_EQ(FR_OK, fr_config_load_file("test/fixtures/lua-object-cleared/daukle.toml",
                                         registry, &manifest, &err));
    ASSERT(fr_project_module(&manifest.self, "seed") == NULL);
    fr_manifest_free(&manifest);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    PASS();
}

/* A lua manifest with no toml beneath it gets no restore_empty_arrays pass,
   because there is no original document to compare against, so its empty
   tables reach the manifest layer as empty objects. Both keys here are ones a
   real project empties: a repository that declares no consumer yet, and a task
   that exists to be depended on rather than to depend. See D-5. */
TEST a_lua_only_manifest_may_empty_the_arrays_it_declares(void) {
    fr_error err;
    fr_registry *registry = NULL;
    ASSERT_EQ(FR_OK, fr_build_registry(&registry, &err));
    fr_manifest manifest;
    int status = fr_config_load_file("test/fixtures/lua-root-empty-arrays/daukle.lua",
                                     registry, &manifest, &err);
    char message[256];
    snprintf(message, sizeof message, "%s", status == FR_OK ? "" : err.message);
    size_t consumers = status == FR_OK ? manifest.consumer_count : 1;
    size_t tasks = status == FR_OK ? manifest.task_count : 0;
    size_t depends = status == FR_OK && tasks == 1 ? manifest.tasks[0].depends_on_count : 1;

    if (status == FR_OK) fr_manifest_free(&manifest);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();

    ASSERT_STR_EQ("", message);
    ASSERT_EQ(0, (int) consumers);
    ASSERT_EQ(1, (int) tasks);
    ASSERT_EQ(0, (int) depends);
    PASS();
}

TEST a_script_registers_a_language_plugin(void) {
    fr_error err;
    fr_registry *registry = NULL;
    fr_build_registry(&registry, &err);
    fr_manifest manifest;
    ASSERT_EQ(FR_OK, fr_config_load_file("test/fixtures/lua-plugin/daukle.toml",
                                         registry, &manifest, &err));
    ASSERT(fr_registry_language(registry, "daukle.language/plaintext") != NULL);
    fr_manifest_free(&manifest);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    PASS();
}

/* take_slot refuses the second declaration before fr_registry_add_language sees
   it, so this asserts the whole message: a laxer match cannot tell the two
   refusals apart, and test_registry is what covers the registry's own. */
TEST a_script_may_not_declare_one_capability_twice(void) {
    fr_error err;
    fr_registry *registry = NULL;
    fr_build_registry(&registry, &err);
    fr_manifest manifest;
    ASSERT_EQ(FR_ERR, fr_config_load_file("test/fixtures/lua-collide/daukle.toml",
                                          registry, &manifest, &err));
    ASSERT(strstr(err.message, "\"daukle.language/npm\" is declared twice") != NULL);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    PASS();
}

/* Exercises lua_source_load and lua_language_apply together through a real
   fr_sync, not just their registration: a source plugin's "load" must return
   a document that satisfies fr_project_parse in full (schema, project,
   version, modules), and a stack leak in either adapter would corrupt the
   shared Lua state the other adapter runs in right after. */
TEST a_script_registered_source_plugin_resolves_through_sync(void) {
    fr_error err;
    fr_sync_report report;
    ASSERT_EQ(FR_OK, fr_sync("test/fixtures/lua-plugin/daukle.toml", 1, 0, &report, &err));
    fr_sync_report_free(&report);

    char *deps = NULL;
    ASSERT_EQ(FR_OK, fr_file_read_text("test/fixtures/lua-plugin/deps.txt", &deps, &err));
    ASSERT_STR_EQ("forebay/basekit/ir\n", deps);
    free(deps);
    PASS();
}

/* The fixture holds no toml beside its daukle.lua, so fr_config_find reaches the
   overlay format as the root manifest and its plugins table is the only way it can
   name a language at all: the sandbox offers no file reading to write one inline. */
TEST a_lua_root_manifest_loads_the_plugins_it_declares(void) {
    fr_error err;
    fr_sync_report report;
    ASSERT_EQ(FR_OK, fr_sync("test/fixtures/lua-root-plugin/daukle.lua", 1, 0, &report, &err));
    fr_sync_report_free(&report);

    char *greeting = NULL;
    ASSERT_EQ(FR_OK, fr_file_read_text("test/fixtures/lua-root-plugin/greet.txt", &greeting, &err));
    ASSERT_STR_EQ("greet forebay/basekit/ir\n", greeting);
    free(greeting);
    PASS();
}

/* A truncated capability string could make two distinct long names collide on
   the same 128-byte buffer and spuriously trip the "declared twice" check
   instead of registering both; take_slot must detect the truncation itself
   and name the plugin rather than silently cut its capability short. */
TEST a_plugin_name_too_long_for_the_capability_buffer_is_rejected(void) {
    fr_error err;
    fr_registry *registry = NULL;
    fr_build_registry(&registry, &err);
    fr_manifest manifest;
    ASSERT_EQ(FR_ERR, fr_config_load_file("test/fixtures/lua-plugin-long-name/daukle.toml",
                                          registry, &manifest, &err));
    ASSERT(strstr(err.message, "too long") != NULL);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    PASS();
}

/* lua_language_apply now runs its whole body, marshaling included, under one
   lua_pcall so a raw Lua C API call cannot throw unprotected and abort the
   process. This proves that protection does not change the observable
   outcome of a script's own error: it still surfaces as a clean FR_ERR with
   the script's message, whether the error comes from the marshaling this
   test cannot force to fail or, as here, from the plugin's own code. */
TEST a_language_plugin_that_raises_an_error_produces_a_clean_failure(void) {
    fr_error err;
    fr_sync_report report;
    ASSERT_EQ(FR_ERR, fr_sync("test/fixtures/lua-plugin-error/daukle.toml", 1, 0, &report, &err));
    ASSERT(strstr(err.message, "boom from a language plugin") != NULL);
    PASS();
}

/* The guard this drives had no case at all until 2026-10-01, and was found by
   mutating it during D-36's file split: a plugin whose apply returns anything
   but a string reaches lua_tostring, which answers NULL for a table and for
   nil. Without the refusal the NULL goes on to strlen. */
TEST a_language_plugin_that_returns_a_non_string_is_refused(void) {
    fr_error err;
    fr_sync_report report;
    ASSERT_EQ(FR_ERR, fr_sync("test/fixtures/lua-plugin-nonstring/daukle.toml", 1, 0,
                              &report, &err));
    ASSERT(strstr(err.message, "expected a string") != NULL);
    ASSERT(strstr(err.message, "table") != NULL);
    PASS();
}

/* Same proof for lua_source_load's protected frame: the source plugin's load
   raises before fr_resolve_consumer ever reaches the language plugin. */
TEST a_source_plugin_that_raises_an_error_produces_a_clean_failure(void) {
    fr_error err;
    fr_sync_report report;
    ASSERT_EQ(FR_ERR, fr_sync("test/fixtures/lua-source-error/daukle.toml", 1, 0, &report, &err));
    ASSERT(strstr(err.message, "boom from a source plugin") != NULL);
    PASS();
}

/* Proves the limit set through fr_lua_set_limits actually reaches the state
   fr_lua_open creates, not just that the flag parses: the same script finishes
   under the default budget and is stopped once the budget is cut down. */
TEST a_low_instruction_limit_stops_a_script_that_would_otherwise_finish(void) {
    fr_error err;
    fr_registry *registry = NULL;
    fr_manifest manifest;

    /* Reset on entry, not only at the end: an ASSERT that fails below returns
       out of this test immediately and skips the trailing reset, which would
       otherwise leak a tightened override into whichever test runs next. */
    fr_lua_set_limits(0, 0);
    ASSERT_EQ(FR_OK, fr_build_registry(&registry, &err));
    ASSERT_EQ(FR_OK, fr_config_load_file("test/fixtures/lua-limit-instruction/daukle.toml",
                                         registry, &manifest, &err));
    fr_manifest_free(&manifest);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();

    fr_lua_set_limits(1000, 0);
    registry = NULL;
    ASSERT_EQ(FR_OK, fr_build_registry(&registry, &err));
    ASSERT_EQ(FR_ERR, fr_config_load_file("test/fixtures/lua-limit-instruction/daukle.toml",
                                          registry, &manifest, &err));
    ASSERT(strstr(err.message, "ran for too long") != NULL);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    fr_lua_set_limits(0, 0);
    PASS();
}

/* Same proof for the memory limit: the script's allocations fit under the
   default 64 MiB budget and do not fit under a 2 MB one, and the two failures
   are distinguishable by message ("ran for too long" vs "not enough memory"),
   so a caller cannot mistake one budget for the other. */
TEST a_low_memory_limit_fails_a_script_that_would_otherwise_finish(void) {
    fr_error err;
    fr_registry *registry = NULL;
    fr_manifest manifest;

    /* Reset on entry for the same reason as the instruction-limit test above:
       a failed ASSERT must not let this test's or the previous test's override
       survive into whatever runs next. */
    fr_lua_set_limits(0, 0);
    ASSERT_EQ(FR_OK, fr_build_registry(&registry, &err));
    ASSERT_EQ(FR_OK, fr_config_load_file("test/fixtures/lua-limit-memory/daukle.toml",
                                         registry, &manifest, &err));
    fr_manifest_free(&manifest);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();

    fr_lua_set_limits(0, 2000000);
    registry = NULL;
    ASSERT_EQ(FR_OK, fr_build_registry(&registry, &err));
    ASSERT_EQ(FR_ERR, fr_config_load_file("test/fixtures/lua-limit-memory/daukle.toml",
                                          registry, &manifest, &err));
    ASSERT(strstr(err.message, "not enough memory") != NULL);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    fr_lua_set_limits(0, 0);
    PASS();
}

/* lua_tostring yields NULL for an error object that is neither a string nor a
   number, and "%s" with NULL is undefined; a script needs one line to get there. */
TEST a_script_raising_a_table_produces_a_clean_failure(void) {
    fr_error err;
    fr_registry *registry = NULL;
    ASSERT_EQ(FR_OK, fr_build_registry(&registry, &err));
    fr_manifest manifest;
    ASSERT_EQ(FR_ERR, fr_config_load_file("test/fixtures/lua-error-object/daukle.toml",
                                          registry, &manifest, &err));
    ASSERT(err.message[0] != '\0');
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    PASS();
}

/* Loading the same configuration twice into one registry now shares the state
   the first opened, which is the point of fr_lua_runtime_begin. It still
   fails, but on the duplicate capability rather than on the lifetime guard. */
TEST refuses_a_second_load_that_redeclares_a_plugin_capability(void) {
    fr_error err;
    fr_registry *registry = NULL;
    ASSERT_EQ(FR_OK, fr_build_registry(&registry, &err));
    fr_manifest manifest;
    ASSERT_EQ(FR_OK, fr_config_load_file("test/fixtures/lua-plugin/daukle.toml",
                                         registry, &manifest, &err));
    fr_manifest_free(&manifest);

    ASSERT_EQ(FR_ERR, fr_config_load_file("test/fixtures/lua-plugin/daukle.toml",
                                          registry, &manifest, &err));
    ASSERT(strstr(err.message, "declared twice") != NULL);

    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    PASS();
}

/* Spec 3.6: the reads a configuration made are what "daukle config print"
   reports, and what a future lockfile records without a second pass here. */
TEST records_every_environment_variable_a_script_read(void) {
    fr_error err;
    fr_registry *registry = NULL;
    ASSERT_EQ(FR_OK, fr_build_registry(&registry, &err));

    fr_test_set_env("DAUKLE_TEST_CHANNEL", "nightly");
    fr_manifest manifest;
    ASSERT_EQ(FR_OK, fr_config_load_file("test/fixtures/lua-env/daukle.toml",
                                         registry, &manifest, &err));
    ASSERT_EQ(2, manifest.self.version.major);

    const cJSON *reads = fr_lua_env_reads();
    ASSERT(reads != NULL);
    const cJSON *channel = cJSON_GetObjectItemCaseSensitive(reads, "DAUKLE_TEST_CHANNEL");
    ASSERT(channel != NULL);
    ASSERT_STR_EQ("nightly", channel->valuestring);

    fr_test_set_env("DAUKLE_TEST_CHANNEL", NULL);
    fr_manifest_free(&manifest);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    ASSERT(fr_lua_env_reads() == NULL);
    PASS();
}

/* The record is diagnostic; a script that makes it unconvertible must lose the
   record and keep its config load. */
TEST an_unconvertible_env_read_record_does_not_fail_the_load(void) {
    fr_error err;
    fr_registry *registry = NULL;
    ASSERT_EQ(FR_OK, fr_build_registry(&registry, &err));

    fr_manifest manifest;
    ASSERT_EQ(FR_OK, fr_config_load_file("test/fixtures/lua-env-cycle/daukle.toml",
                                         registry, &manifest, &err));
    ASSERT_STR_EQ("forebay/env-cycle", manifest.self.project);
    ASSERT(fr_lua_env_reads() == NULL);

    fr_manifest_free(&manifest);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    PASS();
}

/* lua-env declares no plugin, so two loads into the same registry both succeed
   and fr_lua_runtime_begin takes its idempotent same-registry path for the
   second. The recorded reads must reflect the second load, not the first, or
   the document from the first load was never replaced. */
TEST a_second_load_replaces_the_recorded_environment_reads(void) {
    fr_error err;
    fr_registry *registry = NULL;
    ASSERT_EQ(FR_OK, fr_build_registry(&registry, &err));

    fr_test_set_env("DAUKLE_TEST_CHANNEL", "first");
    fr_manifest manifest;
    ASSERT_EQ(FR_OK, fr_config_load_file("test/fixtures/lua-env/daukle.toml",
                                         registry, &manifest, &err));
    fr_manifest_free(&manifest);

    fr_test_set_env("DAUKLE_TEST_CHANNEL", "second");
    ASSERT_EQ(FR_OK, fr_config_load_file("test/fixtures/lua-env/daukle.toml",
                                         registry, &manifest, &err));

    const cJSON *reads = fr_lua_env_reads();
    ASSERT(reads != NULL);
    const cJSON *channel = cJSON_GetObjectItemCaseSensitive(reads, "DAUKLE_TEST_CHANNEL");
    ASSERT(channel != NULL);
    ASSERT_STR_EQ("second", channel->valuestring);

    fr_test_set_env("DAUKLE_TEST_CHANNEL", NULL);
    fr_manifest_free(&manifest);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    PASS();
}

TEST two_plugin_loads_share_one_state(void) {
    fr_error err;
    fr_registry *registry = fr_registry_create();
    ASSERT(registry != NULL);

    ASSERT_EQ(FR_OK, fr_lua_runtime_begin(".", registry, &err));
    lua_State *first = fr_lua_runtime_state();
    ASSERT(first != NULL);

    ASSERT_EQ(FR_OK, fr_lua_runtime_begin(".", registry, &err));
    ASSERT_EQ(first, fr_lua_runtime_state());

    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    PASS();
}

TEST a_second_registry_is_refused_while_the_first_holds_plugins(void) {
    fr_error err;
    fr_registry *first = fr_registry_create();
    fr_registry *second = fr_registry_create();
    ASSERT(first != NULL && second != NULL);

    ASSERT_EQ(FR_OK, fr_lua_runtime_begin(".", first, &err));
    ASSERT_EQ(FR_ERR, fr_lua_runtime_begin(".", second, &err));
    ASSERT(strstr(err.message, "registry") != NULL);

    fr_registry_destroy(first);
    fr_registry_destroy(second);
    fr_lua_runtime_shutdown();
    PASS();
}

TEST a_second_base_directory_is_refused_naming_both(void) {
    fr_error err;
    fr_registry *registry = fr_registry_create();
    ASSERT(registry != NULL);

    ASSERT_EQ(FR_OK, fr_lua_runtime_begin("test/fixtures/lua-plugin", registry, &err));
    ASSERT_EQ(FR_ERR, fr_lua_runtime_begin("test/fixtures/plugin-local", registry, &err));
    ASSERT(strstr(err.message, "lua-plugin") != NULL);
    ASSERT(strstr(err.message, "plugin-local") != NULL);

    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    PASS();
}

TEST the_same_base_directory_spelled_differently_still_reuses_the_runtime(void) {
    fr_error err;
    fr_registry *registry = fr_registry_create();
    ASSERT(registry != NULL);

    ASSERT_EQ(FR_OK, fr_lua_runtime_begin(".", registry, &err));
    lua_State *first = fr_lua_runtime_state();
    ASSERT(first != NULL);
    ASSERT_EQ(FR_OK, fr_lua_runtime_begin("./test/..", registry, &err));
    ASSERT_EQ(first, fr_lua_runtime_state());

    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    PASS();
}

TEST a_plugin_declares_a_toolchain_and_it_reaches_the_registry(void) {
    fr_registry *registry = NULL;
    fr_manifest manifest; fr_error err;
    ASSERT_EQ(FR_OK, fr_build_registry(&registry, &err));
    int loaded = fr_config_load_file("test/fixtures/toolchain-plugin/daukle.toml", registry,
                                     &manifest, &err) == FR_OK;
    int registered = loaded && fr_registry_toolchain(registry, "daukle.toolchain/stub") != NULL;
    if (loaded) fr_manifest_free(&manifest);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();

    ASSERT(loaded);
    ASSERT(registered);
    PASS();
}

TEST a_toolchain_needs_a_generate_function(void) {
    fr_registry *registry = NULL;
    fr_manifest manifest; fr_error err;
    fr_build_registry(&registry, &err);
    int status = fr_config_load_file("test/fixtures/toolchain-nogenerate/daukle.toml", registry,
                                     &manifest, &err);
    char message[sizeof err.message];
    snprintf(message, sizeof message, "%s", err.message);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();

    ASSERT_EQ(FR_ERR, status);
    ASSERT(strstr(message, "needs a generate function") != NULL);
    PASS();
}

/* Exercises the whole marshaling protected_toolchain_generate does: project,
   root and host.os travel through as plain strings, and toolchain.config.target
   is reached through the block-walk rather than a duplicated-and-trimmed copy.
   generate() is called directly through the registry's function pointer since
   nothing yet drives it end to end (that lands with the derived-directory
   writer in a later task).

   Every result is copied into a plain local before any cleanup runs, and
   cleanup always runs before the first ASSERT: an ASSERT that fails macro-
   returns immediately, and skipping fr_registry_destroy/fr_lua_runtime_shutdown
   would leave the shared lua runtime bound to this test's registry, failing
   every unrelated test that runs after it with "another registry's plugins
   are still registered" instead of its own reason. */
TEST a_toolchain_generate_bridges_project_config_root_and_host(void) {
    fr_registry *registry = NULL;
    fr_manifest manifest = {0};
    fr_error err;
    int built = fr_build_registry(&registry, &err) == FR_OK;
    int loaded = built && fr_config_load_file("test/fixtures/toolchain-plugin/daukle.toml",
                                              registry, &manifest, &err) == FR_OK;
    const fr_toolchain_plugin *plugin =
        loaded ? fr_registry_toolchain(registry, "daukle.toolchain/stub") : NULL;
    int toolchain_count = loaded ? (int) manifest.toolchain_count : 0;

    fr_generated_file *files = NULL;
    size_t file_count = 0;
    int status = FR_ERR;
    if (plugin != NULL && toolchain_count == 1) {
        fr_error gen_err;
        status = plugin->generate(plugin->state, &manifest.toolchains[0], manifest.self.project,
                                  "9.9.9", "derived/stub", NULL, 0, &files, &file_count, &gen_err);
    }

    char path[256] = "";
    char text[256] = "";
    if (status == FR_OK && file_count == 1) {
        snprintf(path, sizeof path, "%s", files[0].path);
        snprintf(text, sizeof text, "%s", files[0].text);
    }

    fr_derived_free_files(files, file_count);
    if (loaded) fr_manifest_free(&manifest);
    if (built) fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();

    ASSERT(built);
    ASSERT(loaded);
    ASSERT(plugin != NULL);
    ASSERT_EQ(1, toolchain_count);
    ASSERT_EQ(FR_OK, status);
    ASSERT_EQ(1, (int) file_count);
    ASSERT_STR_EQ("generated.txt", path);
    ASSERT(strstr(text, "project forebay/managed\n") != NULL);
    ASSERT(strstr(text, "version 9.9.9\n") != NULL);
    ASSERT(strstr(text, "target app\n") != NULL);
    ASSERT(strstr(text, "root derived/stub\n") != NULL);
    int has_os = strstr(text, "\nos windows\n") != NULL ||
                 strstr(text, "\nos macos\n") != NULL ||
                 strstr(text, "\nos linux\n") != NULL;
    ASSERT(has_os);
    PASS();
}

/* config is built member by member skipping "dependencies" specifically, not
   by duplicating the block and deleting keys: dependencies already reaches
   the plugin as its own resolved argument, so a raw copy in config would let
   a plugin see it twice, once raw and once resolved. version carries no such
   duplication (the tests below cover it) and is no longer skipped. A plugin
   that reads toolchain.config.dependencies must see nil, and this only shows
   up in the set of keys the table actually holds. Same cleanup-before-ASSERT
   shape as above. */
TEST a_toolchain_config_excludes_the_reserved_dependencies_key(void) {
    fr_registry *registry = NULL;
    fr_manifest manifest = {0};
    fr_error err;
    int built = fr_build_registry(&registry, &err) == FR_OK;
    int loaded = built && fr_config_load_file("test/fixtures/toolchain-generate-config/daukle.toml",
                                              registry, &manifest, &err) == FR_OK;
    const fr_toolchain_plugin *plugin =
        loaded ? fr_registry_toolchain(registry, "daukle.toolchain/stub") : NULL;

    fr_generated_file *files = NULL;
    size_t file_count = 0;
    int status = FR_ERR;
    if (plugin != NULL) {
        fr_error gen_err;
        status = plugin->generate(plugin->state, &manifest.toolchains[0], manifest.self.project,
                                  "1.0.0", "derived/stub", NULL, 0, &files, &file_count, &gen_err);
    }

    char text[256] = "";
    if (status == FR_OK && file_count == 1) snprintf(text, sizeof text, "%s", files[0].text);

    fr_derived_free_files(files, file_count);
    if (loaded) fr_manifest_free(&manifest);
    if (built) fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();

    ASSERT(loaded);
    ASSERT(plugin != NULL);
    ASSERT_EQ(FR_OK, status);
    ASSERT_EQ(1, (int) file_count);
    ASSERT_STR_EQ("flag,target,version", text);
    PASS();
}

/* The task context already carries the toolchain's declared version as its
   own field (config_lua.c's protected_task_run); generate had no equivalent,
   leaving one fact readable from one plugin kind and not the other, before
   provisioning gave a generate-time plugin a use for it. */
TEST the_generate_context_carries_the_toolchain_version(void) {
    fr_registry *registry = NULL;
    fr_manifest manifest = {0};
    fr_error err;
    int built = fr_build_registry(&registry, &err) == FR_OK;
    int loaded = built && fr_config_load_file("test/fixtures/toolchain-generate-config/daukle.toml",
                                              registry, &manifest, &err) == FR_OK;
    const fr_toolchain_plugin *plugin =
        loaded ? fr_registry_toolchain(registry, "daukle.toolchain/stub") : NULL;

    fr_generated_file *files = NULL;
    size_t file_count = 0;
    int status = FR_ERR;
    if (plugin != NULL) {
        fr_error gen_err;
        status = plugin->generate(plugin->state, &manifest.toolchains[0], manifest.self.project,
                                  "1.0.0", "derived/stub", NULL, 0, &files, &file_count, &gen_err);
    }

    char text[256] = "";
    if (status == FR_OK && file_count == 1) snprintf(text, sizeof text, "%s", files[0].text);

    fr_derived_free_files(files, file_count);
    if (loaded) fr_manifest_free(&manifest);
    if (built) fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();

    ASSERT(loaded);
    ASSERT(plugin != NULL);
    ASSERT_EQ(FR_OK, status);
    ASSERT_EQ(1, (int) file_count);
    ASSERT(strstr(text, "version") != NULL);
    PASS();
}

/* The strip is narrowing, not disappearing: dependencies still reach the
   plugin as their own resolved argument and must not also appear in config,
   even once version is let through. */
TEST the_generate_context_still_hides_dependencies_from_config(void) {
    fr_registry *registry = NULL;
    fr_manifest manifest = {0};
    fr_error err;
    int built = fr_build_registry(&registry, &err) == FR_OK;
    int loaded = built && fr_config_load_file("test/fixtures/toolchain-generate-config/daukle.toml",
                                              registry, &manifest, &err) == FR_OK;
    const fr_toolchain_plugin *plugin =
        loaded ? fr_registry_toolchain(registry, "daukle.toolchain/stub") : NULL;

    fr_generated_file *files = NULL;
    size_t file_count = 0;
    int status = FR_ERR;
    if (plugin != NULL) {
        fr_error gen_err;
        status = plugin->generate(plugin->state, &manifest.toolchains[0], manifest.self.project,
                                  "1.0.0", "derived/stub", NULL, 0, &files, &file_count, &gen_err);
    }

    char text[256] = "";
    if (status == FR_OK && file_count == 1) snprintf(text, sizeof text, "%s", files[0].text);

    fr_derived_free_files(files, file_count);
    if (loaded) fr_manifest_free(&manifest);
    if (built) fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();

    ASSERT(loaded);
    ASSERT(plugin != NULL);
    ASSERT_EQ(FR_OK, status);
    ASSERT_EQ(1, (int) file_count);
    ASSERT(strstr(text, "dependencies") == NULL);
    PASS();
}

/* main.c has no test binary (see its own print_plugin_report comment), so
   this exercises the row table print_toolreport() reads from
   (fr_toolreport_row_count/fr_toolreport_row_at) rather than main.c's
   printed output, which nothing here can capture. */
TEST a_provisioned_root_is_readable_as_a_toolreport_row(void) {
    static char label[128];
    static char url[1024];
    static char digest[65];
    snprintf(label, sizeof label, "temurin 21.0.5");
    snprintf(url, sizeof url, "https://example.test/jdk.tar.gz");
    snprintf(digest, sizeof digest,
             "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");

    fr_toolreport_reset();
    fr_toolreport_provisioned(label, url, digest, 0);

    size_t count = fr_toolreport_row_count();
    const fr_toolreport_row *row = count == 1 ? fr_toolreport_row_at(0) : NULL;

    fr_toolreport_reset();

    ASSERT_EQ(1u, (unsigned) count);
    ASSERT(row != NULL);
    ASSERT_STR_EQ(label, row->label);
    ASSERT_STR_EQ(url, row->url);
    ASSERT_STR_EQ(digest, row->digest);
    ASSERT_EQ(1, row->provisioned);
    PASS();
}

/* print_toolreport renders an installed row and a provisioned row
   differently (a path with no digest versus a url with one), so the row
   itself has to keep saying which one it is: a blank digest alone means
   the same thing on both kinds and cannot tell a reader apart. */
TEST an_installed_tool_is_readable_as_a_toolreport_row_with_no_provisioned_flag(void) {
    static char name[128];
    static char path[1024];
    snprintf(name, sizeof name, "gcc");
    snprintf(path, sizeof path, "/usr/bin/gcc");

    fr_toolreport_reset();
    fr_toolreport_used_installed(name, NULL, path);

    size_t count = fr_toolreport_row_count();
    const fr_toolreport_row *row = count == 1 ? fr_toolreport_row_at(0) : NULL;

    fr_toolreport_reset();

    ASSERT_EQ(1u, (unsigned) count);
    ASSERT(row != NULL);
    ASSERT_STR_EQ(name, row->label);
    ASSERT_STR_EQ(path, row->url);
    ASSERT_STR_EQ("", row->digest);
    ASSERT_EQ(0, row->provisioned);
    PASS();
}

TEST a_toolchain_generate_must_return_a_table(void) {
    fr_registry *registry = NULL;
    fr_manifest manifest = {0};
    fr_error err;
    int built = fr_build_registry(&registry, &err) == FR_OK;
    int loaded = built && fr_config_load_file("test/fixtures/toolchain-generate-badreturn/daukle.toml",
                                              registry, &manifest, &err) == FR_OK;
    const fr_toolchain_plugin *plugin =
        loaded ? fr_registry_toolchain(registry, "daukle.toolchain/stub") : NULL;

    fr_generated_file *files = NULL;
    size_t file_count = 0;
    int status = FR_ERR;
    char message[sizeof err.message] = "";
    if (plugin != NULL) {
        fr_error gen_err;
        status = plugin->generate(plugin->state, &manifest.toolchains[0], manifest.self.project,
                                  "1.0.0", "derived/stub", NULL, 0, &files, &file_count, &gen_err);
        snprintf(message, sizeof message, "%s", gen_err.message);
    }

    fr_derived_free_files(files, file_count);
    if (loaded) fr_manifest_free(&manifest);
    if (built) fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();

    ASSERT(loaded);
    ASSERT(plugin != NULL);
    ASSERT_EQ(FR_ERR, status);
    ASSERT(strstr(message, "expected a table") != NULL);
    PASS();
}

TEST a_toolchain_generate_refuses_a_non_string_file_path(void) {
    fr_registry *registry = NULL;
    fr_manifest manifest = {0};
    fr_error err;
    int built = fr_build_registry(&registry, &err) == FR_OK;
    int loaded = built && fr_config_load_file("test/fixtures/toolchain-generate-badkey/daukle.toml",
                                              registry, &manifest, &err) == FR_OK;
    const fr_toolchain_plugin *plugin =
        loaded ? fr_registry_toolchain(registry, "daukle.toolchain/stub") : NULL;

    fr_generated_file *files = NULL;
    size_t file_count = 0;
    int status = FR_ERR;
    char message[sizeof err.message] = "";
    if (plugin != NULL) {
        fr_error gen_err;
        status = plugin->generate(plugin->state, &manifest.toolchains[0], manifest.self.project,
                                  "1.0.0", "derived/stub", NULL, 0, &files, &file_count, &gen_err);
        snprintf(message, sizeof message, "%s", gen_err.message);
    }

    fr_derived_free_files(files, file_count);
    if (loaded) fr_manifest_free(&manifest);
    if (built) fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();

    ASSERT(loaded);
    ASSERT(plugin != NULL);
    ASSERT_EQ(FR_ERR, status);
    ASSERT(strstr(message, "file path must be a string") != NULL);
    PASS();
}

TEST a_toolchain_generate_refuses_a_non_string_file_contents(void) {
    fr_registry *registry = NULL;
    fr_manifest manifest = {0};
    fr_error err;
    int built = fr_build_registry(&registry, &err) == FR_OK;
    int loaded = built && fr_config_load_file("test/fixtures/toolchain-generate-badvalue/daukle.toml",
                                              registry, &manifest, &err) == FR_OK;
    const fr_toolchain_plugin *plugin =
        loaded ? fr_registry_toolchain(registry, "daukle.toolchain/stub") : NULL;

    fr_generated_file *files = NULL;
    size_t file_count = 0;
    int status = FR_ERR;
    char message[sizeof err.message] = "";
    if (plugin != NULL) {
        fr_error gen_err;
        status = plugin->generate(plugin->state, &manifest.toolchains[0], manifest.self.project,
                                  "1.0.0", "derived/stub", NULL, 0, &files, &file_count, &gen_err);
        snprintf(message, sizeof message, "%s", gen_err.message);
    }

    fr_derived_free_files(files, file_count);
    if (loaded) fr_manifest_free(&manifest);
    if (built) fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();

    ASSERT(loaded);
    ASSERT(plugin != NULL);
    ASSERT_EQ(FR_ERR, status);
    ASSERT(strstr(message, "generated file \"generated.txt\" must be a string") != NULL);
    PASS();
}

TEST a_task_is_declared_and_registered(void) {
    fr_registry *registry = fr_registry_create();
    fr_error err;
    ASSERT_EQ(FR_OK, fr_lua_runtime_begin(".", registry, &err));
    const char *uses[] = { "exec" };
    const char *chunk =
        "daukle.toolchain{ name = 'cmake', generate = function() return {} end }\n"
        "daukle.task{ name = 'cmake:build', partOf = 'build', dependsOn = { 'cmake:configure' },"
        " run = function() end }\n";
    ASSERT_EQ(FR_OK, fr_lua_plugin_load(chunk, strlen(chunk), "cmake.lua", uses, 1, NULL, NULL, &err));

    const fr_task_plugin *task = fr_registry_task(registry, "daukle.task/cmake:build");
    ASSERT(task != NULL);
    ASSERT_STR_EQ("build", task->part_of);
    ASSERT_EQ(1u, task->depends_on_count);
    ASSERT_STR_EQ("cmake:configure", task->depends_on[0]);
    ASSERT(task->run != NULL);
    fr_lua_runtime_shutdown();
    fr_registry_destroy(registry);
    PASS();
}

TEST a_task_cannot_claim_a_toolchain_its_chunk_did_not_declare(void) {
    fr_registry *registry = fr_registry_create();
    fr_error err;
    ASSERT_EQ(FR_OK, fr_lua_runtime_begin(".", registry, &err));
    const char *chunk = "daukle.task{ name = 'cmake:build', run = function() end }\n";
    ASSERT_EQ(FR_ERR, fr_lua_plugin_load(chunk, strlen(chunk), "squatter.lua", NULL, 0, NULL, NULL, &err));
    ASSERT(strstr(err.message, "no toolchain \"cmake\" is declared above this point") != NULL);
    fr_lua_runtime_shutdown();
    fr_registry_destroy(registry);
    PASS();
}

/* A task keeps exec and provision by inheriting them from the toolchain that
   owns it, and a colon-free name in a chunk declaring no toolchain owns
   nothing. Without this a plugin declares one such task and keeps the
   capability with no toolchain anywhere. */
TEST a_task_without_a_toolchain_may_not_keep_provision(void) {
    fr_registry *registry = fr_registry_create();
    fr_error err;
    ASSERT_EQ(FR_OK, fr_lua_runtime_begin(".", registry, &err));
    const char *uses[] = { "provision" };
    const char *chunk = "daukle.task{ name = 'build', run = function() end }\n";
    ASSERT_EQ(FR_ERR,
              fr_lua_plugin_load(chunk, strlen(chunk), "squatter.lua", uses, 1, NULL, NULL, &err));
    ASSERT(strstr(err.message, "daukle.provision is available only to a task whose toolchain"
                               " is declared above it") != NULL);

    fr_lua_runtime_shutdown();
    fr_registry_destroy(registry);
    PASS();
}

/* The companion: the toolchain the chunk declares is what the task inherits
   from, so the same verbs must still load beside one. */
TEST a_task_beside_a_toolchain_still_keeps_provision(void) {
    fr_registry *registry = fr_registry_create();
    fr_error err;
    ASSERT_EQ(FR_OK, fr_lua_runtime_begin(".", registry, &err));
    const char *uses[] = { "provision" };
    const char *chunk =
        "daukle.toolchain{ name = 'cmake', generate = function() return {} end }\n"
        "daukle.task{ name = 'cmake:build', run = function() end }\n";
    ASSERT_EQ(FR_OK,
              fr_lua_plugin_load(chunk, strlen(chunk), "cmake.lua", uses, 1, NULL, NULL, &err));
    ASSERT(fr_registry_task(registry, "daukle.task/cmake:build") != NULL);

    fr_lua_runtime_shutdown();
    fr_registry_destroy(registry);
    PASS();
}

TEST an_aggregator_needs_no_run(void) {
    fr_registry *registry = fr_registry_create();
    fr_error err;
    ASSERT_EQ(FR_OK, fr_lua_runtime_begin(".", registry, &err));
    const char *chunk = "daukle.task{ name = 'build' }\n";
    ASSERT_EQ(FR_OK, fr_lua_plugin_load(chunk, strlen(chunk), "lifecycle.lua", NULL, 0, NULL, NULL, &err));

    const fr_task_plugin *task = fr_registry_task(registry, "daukle.task/build");
    ASSERT(task != NULL);
    ASSERT(task->run == NULL);
    fr_lua_runtime_shutdown();
    fr_registry_destroy(registry);
    PASS();
}

/* Guards the ordering hazard directly: chunk_toolchains_clear() must run
   between loads, so a toolchain declared by one plugin's chunk can never
   authorize a task named by a later, unrelated chunk. */
TEST a_second_chunks_task_cannot_reuse_the_first_chunks_toolchain(void) {
    fr_registry *registry = fr_registry_create();
    fr_error err;
    ASSERT_EQ(FR_OK, fr_lua_runtime_begin(".", registry, &err));
    const char *first_chunk =
        "daukle.toolchain{ name = 'cmake', generate = function() return {} end }\n";
    ASSERT_EQ(FR_OK, fr_lua_plugin_load(first_chunk, strlen(first_chunk), "cmake.lua", NULL, 0, NULL, NULL, &err));

    const char *second_chunk = "daukle.task{ name = 'cmake:build', run = function() end }\n";
    ASSERT_EQ(FR_ERR, fr_lua_plugin_load(second_chunk, strlen(second_chunk), "squatter.lua", NULL, 0, NULL, NULL, &err));
    ASSERT(strstr(err.message, "no toolchain \"cmake\" is declared above this point") != NULL);

    fr_lua_runtime_shutdown();
    fr_registry_destroy(registry);
    PASS();
}

/* A plugin must declare its toolchain above the tasks that name it, and both
   refusals for getting that wrong used to describe the CHUNK ("this plugin
   declares no toolchain", "only to a toolchain plugin") when the real condition
   is the POSITION. A toolchain plugin that declared its toolchain two lines
   below was told it was not a toolchain plugin, which reads as a bug in daukle
   rather than as an ordering mistake in the plugin. */
TEST the_same_chunk_reordered_is_the_difference_between_refused_and_accepted(void) {
    const char *task = "daukle.task{ name = 'cmake:build', run = function() end }\n";
    const char *toolchain =
        "daukle.toolchain{ name = 'cmake', generate = function() return {} end }\n";
    char below[256];
    char above[256];
    snprintf(below, sizeof below, "%s%s", task, toolchain);
    snprintf(above, sizeof above, "%s%s", toolchain, task);

    fr_registry *first = fr_registry_create();
    fr_error err;
    ASSERT_EQ(FR_OK, fr_lua_runtime_begin(".", first, &err));
    int refused = fr_lua_plugin_load(below, strlen(below), "cmake.lua", NULL, 0, NULL, NULL, &err);
    static char message[256];
    snprintf(message, sizeof message, "%s", refused == FR_OK ? "" : err.message);
    fr_lua_runtime_shutdown();
    fr_registry_destroy(first);

    fr_registry *second = fr_registry_create();
    ASSERT_EQ(FR_OK, fr_lua_runtime_begin(".", second, &err));
    int accepted = fr_lua_plugin_load(above, strlen(above), "cmake.lua", NULL, 0, NULL, NULL, &err);
    char accepted_message[256];
    snprintf(accepted_message, sizeof accepted_message, "%s", accepted == FR_OK ? "" : err.message);
    fr_lua_runtime_shutdown();
    fr_registry_destroy(second);

    ASSERT_EQ_FMT(FR_ERR, refused, "%d");
    ASSERTm(message, strstr(message, "no toolchain \"cmake\" is declared above this point") != NULL);
    ASSERT_STR_EQ("", accepted_message);
    ASSERT_EQ_FMT(FR_OK, accepted, "%d");
    PASS();
}

TEST exec_for_a_task_above_its_toolchain_is_refused_naming_the_order(void) {
    fr_registry *registry = fr_registry_create();
    fr_error err;
    ASSERT_EQ(FR_OK, fr_lua_runtime_begin(".", registry, &err));
    const char *uses[] = { "exec" };
    const char *chunk =
        "daukle.task{ name = 'build', run = function() end }\n"
        "daukle.toolchain{ name = 'cmake', generate = function() return {} end }\n";
    int status = fr_lua_plugin_load(chunk, strlen(chunk), "cmake.lua", uses, 1, NULL, NULL, &err);
    static char message[256];
    snprintf(message, sizeof message, "%s", status == FR_OK ? "" : err.message);
    fr_lua_runtime_shutdown();
    fr_registry_destroy(registry);

    ASSERT_EQ(FR_ERR, status);
    ASSERTm(message, strstr(message, "daukle.exec is available only") != NULL);
    ASSERTm(message, strstr(message, "declared above it") != NULL);
    PASS();
}

/* setmetatable is a reachable base global, so a plugin can pass daukle.task a
   table whose __index raises on any miss. The declaration table here has no
   raw "name" at all, so the field lookup must not dispatch through __index:
   if it did, the load would fail with "boom" from the metamethod instead of
   the ordinary "a task needs a name" refusal, and the raise would land after
   an allocation with nothing yet freeing it. */
TEST a_hostile_index_metatable_on_the_task_table_is_never_consulted(void) {
    fr_registry *registry = fr_registry_create();
    fr_error err;
    ASSERT_EQ(FR_OK, fr_lua_runtime_begin(".", registry, &err));
    const char *chunk =
        "daukle.task(setmetatable({}, { __index = function() error('boom') end }))\n";
    ASSERT_EQ(FR_ERR, fr_lua_plugin_load(chunk, strlen(chunk), "hostile.lua", NULL, 0, NULL, NULL, &err));
    ASSERT(strstr(err.message, "a task needs a name") != NULL);
    ASSERT(strstr(err.message, "boom") == NULL);
    fr_lua_runtime_shutdown();
    fr_registry_destroy(registry);
    PASS();
}

/* daukle.toolchain's own "name" and "generate" both come from take_slot's
   ordinary (metamethod-honouring) reads, so an __index that raises
   unconditionally would already fail there, before reaching the read this
   test actually targets. A table with "generate" present but "name" absent
   forces both take_slot's read of "name" and lua_declare_toolchain's own
   second read of it (the one that records which toolchain this chunk may
   declare tasks for) through the same __index; a stateful metamethod that
   answers the first of those and raises on the second is exactly the attack
   the fix closes, and the only way to reach the targeted read at all. With
   raw_getfield in place, that second read never consults __index, so it
   simply finds no raw "name" and refuses plainly; reverting to lua_getfield
   lets the metamethod's second call fire and "boom" leaks into err.message
   instead. */
TEST a_hostile_index_metatable_on_the_toolchain_table_is_never_consulted(void) {
    fr_registry *registry = fr_registry_create();
    fr_error err;
    ASSERT_EQ(FR_OK, fr_lua_runtime_begin(".", registry, &err));
    const char *chunk =
        "local seen = false\n"
        "local mt = { __index = function(t, k)\n"
        "  if k ~= 'name' then return nil end\n"
        "  if seen then error('boom') end\n"
        "  seen = true\n"
        "  return 'cmake'\n"
        "end }\n"
        "daukle.toolchain(setmetatable({ generate = function() return {} end }, mt))\n";
    ASSERT_EQ(FR_ERR, fr_lua_plugin_load(chunk, strlen(chunk), "hostile-toolchain.lua", NULL, 0, NULL, NULL, &err));
    ASSERT(strstr(err.message, "a toolchain needs a name") != NULL);
    ASSERT(strstr(err.message, "boom") == NULL);
    fr_lua_runtime_shutdown();
    fr_registry_destroy(registry);
    PASS();
}

/* resolvers.c marks a resolver entry's own chunk as acquired as a resolver
   before it runs it, which is the only state daukle.resolver may be called
   in. These tests load such a chunk directly, so they set the same flag. */
static int load_resolver_chunk(const char *text, const char *origin,
                               const char *const *verbs, size_t verb_count,
                               fr_error *err) {
    fr_lua_set_acquiring_resolver(1);
    int status = fr_lua_plugin_load(text, strlen(text), origin, verbs, verb_count, NULL, NULL, err);
    fr_lua_set_acquiring_resolver(0);
    return status;
}

/* The guard load_resolver_chunk exists for: an ordinary plugin chunk reaches
   fr_lua_plugin_load without the flag, and a resolver it declared would
   replace the callback every later resolved entry is served through. */
TEST a_chunk_not_acquired_as_a_resolver_may_not_declare_one(void) {
    fr_error err;
    fr_registry *registry = fr_registry_create();
    int began = fr_lua_runtime_begin(".", registry, &err) == FR_OK;
    const char *chunk =
        "daukle.plugin{ api = 1, uses = {} }\n"
        "daukle.resolver{ resolve = function(c) return { url = c } end }\n";
    int status = fr_lua_plugin_load(chunk, strlen(chunk), "ordinary.lua", NULL, 0, NULL, NULL, &err);
    int declared = fr_lua_resolver_declared();
    static char message[512];
    snprintf(message, sizeof message, "%s", err.message);

    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    ASSERT(began);
    ASSERT_EQ(FR_ERR, status);
    ASSERTm(message, strstr(message, "acquired as a resolver") != NULL);
    ASSERT_EQ(0, declared);
    PASS();
}

TEST a_resolver_chunk_declares_a_resolver(void) {
    fr_error err;
    fr_registry *registry = fr_registry_create();
    int began = fr_lua_runtime_begin(".", registry, &err) == FR_OK;
    const char *chunk =
        "daukle.plugin{ api = 1, uses = {} }\n"
        "daukle.resolver{ resolve = function(c) return { url = 'https://h/' .. c } end }\n";
    int loaded = load_resolver_chunk(chunk, "r.lua", NULL, 0, &err) == FR_OK;
    int declared = fr_lua_resolver_declared();

    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    ASSERT(began);
    ASSERT(loaded);
    ASSERT(declared);
    PASS();
}

TEST a_resolver_chunk_may_not_also_declare_a_language(void) {
    fr_error err;
    fr_registry *registry = fr_registry_create();
    int began = fr_lua_runtime_begin(".", registry, &err) == FR_OK;
    const char *chunk =
        "daukle.plugin{ api = 1, uses = {} }\n"
        "daukle.resolver{ resolve = function(c) return { url = c } end }\n"
        "daukle.language{ name = 'x', apply = function() return '' end }\n";
    int status = load_resolver_chunk(chunk, "r.lua", NULL, 0, &err);
    char message[512];
    snprintf(message, sizeof message, "%s", err.message);

    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    ASSERT(began);
    ASSERT_EQ(FR_ERR, status);
    ASSERT(strstr(message, "a resolver chunk declares only a resolver") != NULL);
    PASS();
}

TEST a_chunk_that_declared_a_language_may_not_then_declare_a_resolver(void) {
    fr_error err;
    fr_registry *registry = fr_registry_create();
    int began = fr_lua_runtime_begin(".", registry, &err) == FR_OK;
    const char *chunk =
        "daukle.plugin{ api = 1, uses = {} }\n"
        "daukle.language{ name = 'x', apply = function() return '' end }\n"
        "daukle.resolver{ resolve = function(c) return { url = c } end }\n";
    int status = load_resolver_chunk(chunk, "r.lua", NULL, 0, &err);
    char message[512];
    snprintf(message, sizeof message, "%s", err.message);

    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    ASSERT(began);
    ASSERT_EQ(FR_ERR, status);
    ASSERT(strstr(message, "a resolver chunk declares only a resolver") != NULL);
    PASS();
}

TEST a_resolver_may_not_declare_exec(void) {
    fr_error err;
    fr_registry *registry = fr_registry_create();
    int began = fr_lua_runtime_begin(".", registry, &err) == FR_OK;
    const char *uses[] = { "exec" };
    const char *chunk =
        "daukle.plugin{ api = 1, uses = { 'exec' } }\n"
        "daukle.resolver{ resolve = function(c) return { url = c } end }\n";
    int status = load_resolver_chunk(chunk, "r.lua", uses, 1, &err);
    char message[512];
    snprintf(message, sizeof message, "%s", err.message);

    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    ASSERT(began);
    ASSERT_EQ(FR_ERR, status);
    ASSERT(strstr(message, "available only to a toolchain or publisher plugin") != NULL);
    PASS();
}

TEST a_resolver_may_not_declare_tool(void) {
    fr_error err;
    fr_registry *registry = fr_registry_create();
    int began = fr_lua_runtime_begin(".", registry, &err) == FR_OK;
    const char *uses[] = { "tool" };
    const char *chunk =
        "daukle.plugin{ api = 1, uses = { 'tool' } }\n"
        "daukle.resolver{ resolve = function(c) return { url = c } end }\n";
    int status = load_resolver_chunk(chunk, "r.lua", uses, 1, &err);
    char message[512];
    snprintf(message, sizeof message, "%s", err.message);

    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    ASSERT(began);
    ASSERT_EQ(FR_ERR, status);
    ASSERT(strstr(message, "a resolver may not start a process") != NULL);
    PASS();
}

TEST a_resolver_needs_a_resolve_function(void) {
    fr_error err;
    fr_registry *registry = fr_registry_create();
    int began = fr_lua_runtime_begin(".", registry, &err) == FR_OK;
    const char *chunk =
        "daukle.plugin{ api = 1, uses = {} }\n"
        "daukle.resolver{ }\n";
    int status = load_resolver_chunk(chunk, "r.lua", NULL, 0, &err);
    char message[512];
    snprintf(message, sizeof message, "%s", err.message);

    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    ASSERT(began);
    ASSERT_EQ(FR_ERR, status);
    ASSERT(strstr(message, "needs a resolve function") != NULL);
    PASS();
}

TEST a_hostile_metatable_cannot_supply_the_resolve_function(void) {
    fr_error err;
    fr_registry *registry = fr_registry_create();
    int began = fr_lua_runtime_begin(".", registry, &err) == FR_OK;
    const char *chunk =
        "daukle.plugin{ api = 1, uses = {} }\n"
        "local t = setmetatable({}, { __index = function() error('boom') end })\n"
        "daukle.resolver(t)\n";
    int status = load_resolver_chunk(chunk, "r.lua", NULL, 0, &err);
    char message[512];
    snprintf(message, sizeof message, "%s", err.message);

    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    ASSERT(began);
    ASSERT_EQ(FR_ERR, status);
    ASSERT(strstr(message, "needs a resolve function") != NULL);
    ASSERT(strstr(message, "boom") == NULL);
    PASS();
}

/* The dangerous hostile-metatable case, unlike the one above: __index does
   not raise, it RETURNS a function. lua_isfunction alone cannot tell that
   function apart from a real "resolve" field, so a lua_getfield read here
   would succeed and hand daukle a resolver whose behavior a plugin-supplied
   metamethod chose. raw_getfield never reaches __index at all, so it finds
   no raw "resolve" key and refuses, and no resolver is left declared. */
TEST a_hostile_metatable_returning_a_function_is_never_used_as_resolve(void) {
    fr_error err;
    fr_registry *registry = fr_registry_create();
    int began = fr_lua_runtime_begin(".", registry, &err) == FR_OK;
    const char *chunk =
        "daukle.plugin{ api = 1, uses = {} }\n"
        "local t = setmetatable({}, { __index = function() return function()"
        " return { url = 'x' } end end })\n"
        "daukle.resolver(t)\n";
    int status = load_resolver_chunk(chunk, "r.lua", NULL, 0, &err);
    int declared = fr_lua_resolver_declared();
    char message[512];
    snprintf(message, sizeof message, "%s", err.message);

    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    ASSERT(began);
    ASSERT_EQ(FR_ERR, status);
    ASSERT(strstr(message, "needs a resolve function") != NULL);
    ASSERT_EQ(0, declared);
    PASS();
}

/* A second chunk that declares no resolver at all must not inherit the
   first's: fr_lua_resolver_declared reports whether the chunk that JUST ran
   declared one, not whether any chunk ever has. */
TEST a_chunk_declaring_no_resolver_reports_none_declared(void) {
    fr_error err;
    fr_registry *registry = fr_registry_create();
    int began = fr_lua_runtime_begin(".", registry, &err) == FR_OK;
    const char *first_chunk =
        "daukle.plugin{ api = 1, uses = {} }\n"
        "daukle.resolver{ resolve = function(c) return { url = c } end }\n";
    int first_loaded = load_resolver_chunk(first_chunk, "first.lua", NULL, 0, &err) == FR_OK;

    const char *second_chunk =
        "daukle.plugin{ api = 1, uses = {} }\n"
        "daukle.language{ name = 'x', apply = function() return '' end }\n";
    int second_loaded = fr_lua_plugin_load(second_chunk, strlen(second_chunk), "second.lua", NULL, 0, NULL, NULL, &err) == FR_OK;
    int declared = fr_lua_resolver_declared();

    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    ASSERT(began);
    ASSERT(first_loaded);
    ASSERT(second_loaded);
    ASSERT_EQ(0, declared);
    PASS();
}

/* A chunk that declares a resolver and is then refused must leave neither a
   live resolver_callback (Critical: leak/inheritance) nor a "declared" answer
   behind for the next fr_lua_resolver_declared caller. */
TEST a_refused_resolver_chunk_reports_none_declared(void) {
    fr_error err;
    fr_registry *registry = fr_registry_create();
    int began = fr_lua_runtime_begin(".", registry, &err) == FR_OK;
    const char *chunk =
        "daukle.plugin{ api = 1, uses = {} }\n"
        "daukle.resolver{ resolve = function(c) return { url = c } end }\n"
        "daukle.language{ name = 'x', apply = function() return '' end }\n";
    int status = load_resolver_chunk(chunk, "r.lua", NULL, 0, &err);
    int declared = fr_lua_resolver_declared();

    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    ASSERT(began);
    ASSERT_EQ(FR_ERR, status);
    ASSERT_EQ(0, declared);
    PASS();
}

/* Builds a chunk that declares "provision" in uses and then makes one further
   declaration, and loads it. is_resolver mirrors load_resolver_chunk above:
   a resolver may only ever be declared by a chunk acquired as one, so this
   routes through it for that one kind and through fr_lua_plugin_load
   directly for every other kind. */
static int load_declaring_provision(const char *declaration, int is_resolver, fr_error *err) {
    char source[1024];
    snprintf(source, sizeof source,
             "daukle.plugin{ api = 1, uses = { \"provision\" } }\n%s\n", declaration);
    const char *verbs[] = { "provision" };
    return is_resolver
        ? load_resolver_chunk(source, "provision-refusal.lua", verbs, 1, err)
        : fr_lua_plugin_load(source, strlen(source), "provision-refusal.lua", verbs, 1, NULL, NULL, err);
}

/* Three separate tests, not a loop over the three kinds: child spec 8 found
   that the exec refusal lives in daukle.language and daukle.source
   individually, so daukle.resolver refused nothing until it carried the same
   check on its own. A loop would keep passing the day a fourth kind is added
   without one; deleting one kind's check here must turn exactly one test red. */
TEST a_language_plugin_declaring_provision_is_refused(void) {
    static char message[512];
    fr_error err;
    fr_registry *registry = fr_registry_create();
    int began = fr_lua_runtime_begin(".", registry, &err) == FR_OK;
    int status = load_declaring_provision(
        "daukle.language{ name = 'x', apply = function() return {} end }", 0, &err);
    snprintf(message, sizeof message, "%s", err.message);

    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    ASSERT(began);
    ASSERT_EQm(message, FR_ERR, status);
    ASSERTm(message, strstr(message, "provision") != NULL);
    ASSERTm(message, strstr(message, "toolchain") != NULL);
    PASS();
}

TEST a_source_plugin_declaring_provision_is_refused(void) {
    static char message[512];
    fr_error err;
    fr_registry *registry = fr_registry_create();
    int began = fr_lua_runtime_begin(".", registry, &err) == FR_OK;
    int status = load_declaring_provision(
        "daukle.source{ name = 'x', load = function() return {} end }", 0, &err);
    snprintf(message, sizeof message, "%s", err.message);

    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    ASSERT(began);
    ASSERT_EQm(message, FR_ERR, status);
    ASSERTm(message, strstr(message, "provision") != NULL);
    PASS();
}

TEST a_resolver_plugin_declaring_provision_is_refused(void) {
    static char message[512];
    fr_error err;
    fr_registry *registry = fr_registry_create();
    int began = fr_lua_runtime_begin(".", registry, &err) == FR_OK;
    int status = load_declaring_provision(
        "daukle.resolver{ resolve = function(c) return { url = c } end }", 1, &err);
    snprintf(message, sizeof message, "%s", err.message);

    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    ASSERT(began);
    ASSERT_EQm(message, FR_ERR, status);
    ASSERTm(message, strstr(message, "provision") != NULL);
    PASS();
}

/* The positive case, and it is not optional: without it, all three refusals
   above would pass against an implementation that refuses "provision" to
   every kind, which would make the capability unreachable. */
TEST a_toolchain_plugin_declaring_provision_loads(void) {
    static char message[512];
    fr_error err;
    fr_registry *registry = fr_registry_create();
    int began = fr_lua_runtime_begin(".", registry, &err) == FR_OK;
    int status = load_declaring_provision(
        "daukle.toolchain{ name = 'x', generate = function() return {} end }", 0, &err);
    snprintf(message, sizeof message, "%s", err.message);

    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    ASSERT(began);
    ASSERT_EQm(message, FR_OK, status);
    PASS();
}

/* Loads a fixture that declares a directory-form plugin and reports both the
   status and the message, so each test below asserts on one clause rather than
   on whichever failure happened to come first. */
/* Static, not a local: greatest's ASSERT_*m keeps the message POINTER and prints
   it after the test function has returned, so a buffer on the test's own stack
   reads back as whatever replaced it. */
static char fixture_message[512];

static int loads_fixture(const char *manifest) {
    fr_error err;
    /* A successful load leaves err untouched, and a test that failed for some
       other reason would otherwise report whatever was on the stack. */
    err.message[0] = '\0';
    fixture_message[0] = '\0';

    fr_registry *registry = NULL;
    if (fr_build_registry(&registry, &err) != FR_OK) {
        snprintf(fixture_message, sizeof fixture_message, "%s", err.message);
        return FR_ERR;
    }

    fr_manifest parsed;
    int status = fr_config_load_file(manifest, registry, &parsed, &err);
    int registered = status == FR_OK
                     && fr_registry_language(registry, "daukle.language/hello") != NULL;
    snprintf(fixture_message, sizeof fixture_message, "%s",
             status == FR_OK && !registered ? "the plugin registered no language" : err.message);

    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    return status == FR_OK && registered ? FR_OK : FR_ERR;
}

TEST a_module_supplies_what_the_entry_chunk_requires(void) {
    int status = loads_fixture("test/fixtures/plugin-directory/daukle.toml");
    ASSERT_EQm(fixture_message, FR_OK, status);
    PASS();
}

TEST a_module_may_declare_a_language(void) {
    int status = loads_fixture("test/fixtures/plugin-module-declares/daukle.toml");
    ASSERT_EQm(fixture_message, FR_OK, status);
    PASS();
}

/* The one test that proves a module runs in the plugin's OWN environment rather
   than in a copy of it: a copy would pass everything else here. */
TEST a_module_may_not_use_a_verb_the_plugin_did_not_declare(void) {
    int status = loads_fixture("test/fixtures/plugin-module-verb/daukle.toml");
    ASSERT_EQ(FR_ERR, status);
    ASSERT(strstr(fixture_message, "was not declared in uses") != NULL);
    PASS();
}

/* daukle.require needs the plugin to have said what it uses, and the rule that
   gets it there already exists: daukle.plugin must be the first call. This is
   what says the two compose, so a change to either is not free. */
TEST daukle_require_before_daukle_plugin_is_refused(void) {
    int status = loads_fixture("test/fixtures/plugin-require-early/daukle.toml");
    ASSERT_EQ(FR_ERR, status);
    ASSERT(strstr(fixture_message, "daukle.plugin must be the first call") != NULL);
    PASS();
}

/* daukle.require is not a capability, so it is not in "uses" and a plugin that
   declares none still has it. */
TEST a_plugin_declaring_no_uses_may_still_require(void) {
    int status = loads_fixture("test/fixtures/plugin-no-uses/daukle.toml");
    ASSERT_EQm(fixture_message, FR_OK, status);
    PASS();
}

/* If the module ran twice its daukle.language would be declared twice, which an
   existing guard refuses: the memo is what makes this load succeed. */
TEST a_module_required_twice_runs_once(void) {
    int status = loads_fixture("test/fixtures/plugin-module-once/daukle.toml");
    ASSERT_EQm(fixture_message, FR_OK, status);
    PASS();
}

TEST a_cycle_between_modules_is_refused(void) {
    int status = loads_fixture("test/fixtures/plugin-module-cycle/daukle.toml");
    ASSERT_EQ(FR_ERR, status);
    /* On the clause, not on a module name: both names appear in the input, so a
       name match would pass against a message that said anything at all. */
    ASSERT(strstr(fixture_message, "is a cycle") != NULL);
    PASS();
}

/* ---- daukle.require("<alias>:<module>") across a plugin boundary ----

   Every test below goes through fr_plugins_load, because load_one is where the
   dependency graph is acquired and handed to the chunk: a test that called
   fr_lua_plugin_load itself would pass with that wiring deleted. */

/* Enough for the deepest chain a test builds: reaching the owner-stack cap of 8
   needs nine providers beside the dependent. */
#define XP_MAX_ARTIFACTS 16
#define XP_BODY_MAX 8192

typedef struct {
    const char *url;
    char body[XP_BODY_MAX];
    size_t length;
    char digest[65];
} xp_artifact;

static xp_artifact XP[XP_MAX_ARTIFACTS];
static size_t xp_count;
static char xp_message[512];

/* What xp_serve hands back when it cannot serve, so a caller reading .url and
   .digest gets empty strings rather than another artifact's. xp_loads refuses
   while this is set, so the test fails saying what could not be served instead
   of failing later on a mismatched pin. */
static xp_artifact xp_unservable;
static int xp_overflowed;

static const char XP_APPLY[] = "apply = function(consumer, resolved, text) return text end";

static void xp_reset(void) {
    xp_count = 0;
    xp_overflowed = 0;
    xp_message[0] = '\0';
    memset(&xp_unservable, 0, sizeof xp_unservable);
    xp_unservable.url = "";
}

/* The digest is computed from the bytes the stub will serve and never pasted:
   a pasted one is computed over one line ending and compared against whatever
   the checkout wrote. */
static const xp_artifact *xp_serve(const char *url, const char *bytes, size_t length) {
    xp_artifact *artifact = NULL;
    for (size_t index = 0; index < xp_count && artifact == NULL; index++) {
        if (strcmp(XP[index].url, url) == 0) artifact = &XP[index];
    }
    if (artifact == NULL) {
        if (xp_count == XP_MAX_ARTIFACTS || length > XP_BODY_MAX) {
            snprintf(xp_message, sizeof xp_message,
                     "the test cannot serve \"%s\": %s", url,
                     xp_count == XP_MAX_ARTIFACTS ? "too many artifacts" : "body too large");
            xp_overflowed = 1;
            return &xp_unservable;
        }
        artifact = &XP[xp_count];
        xp_count++;
    }
    artifact->url = url;
    memcpy(artifact->body, bytes, length);
    artifact->length = length;
    fr_sha256_hex(artifact->body, length, artifact->digest);
    return artifact;
}

static const xp_artifact *xp_serve_chunk(const char *url, const char *text) {
    return xp_serve(url, text, strlen(text));
}

/* A provider is an archive, because a single chunk carries no members to
   export. names and bodies are its modules, beside the plugin.lua in entry. */
static const xp_artifact *xp_serve_plugin(const char *url, const char *entry,
                                          const char *const *names, const char *const *bodies,
                                          size_t member_count) {
    static char archive[XP_BODY_MAX];
    memset(archive, 0, sizeof archive);
    size_t offset = fr_test_tar_append(archive, 0, "plugin.lua", '0', entry, strlen(entry));
    for (size_t index = 0; index < member_count; index++) {
        offset = fr_test_tar_append(archive, offset, names[index], '0', bodies[index],
                                    strlen(bodies[index]));
    }
    offset = fr_test_tar_end(archive, offset);
    return xp_serve(url, archive, offset);
}

static char *xp_copy(const char *data, size_t length) {
    char *copy = malloc(length + 1);
    if (copy == NULL) return NULL;
    memcpy(copy, data, length);
    copy[length] = '\0';
    return copy;
}

static int xp_http(const char *url, const fr_http_header *headers, size_t header_count,
                   char **out_body, size_t *out_length, fr_error *err) {
    (void) headers;
    (void) header_count;
    for (size_t index = 0; index < xp_count; index++) {
        if (strcmp(url, XP[index].url) != 0) continue;
        *out_body = xp_copy(XP[index].body, XP[index].length);
        if (*out_body == NULL) {
            fr_error_set(err, "out of memory serving %s", url);
            return FR_ERR;
        }
        *out_length = XP[index].length;
        return FR_OK;
    }
    fr_error_set(err, "no artifact for %s", url);
    return FR_ERR;
}

/* Writes into the caller's buffer rather than a shared static: two of these in
   one argument list would otherwise both read back the second one. */
static const char *xp_requires_one(char *out, size_t size, const char *alias,
                                   const xp_artifact *artifact) {
    snprintf(out, size, "%s = { url = \"%s\", sha256 = \"%s\" }", alias, artifact->url,
             artifact->digest);
    return out;
}

static char xp_chunk[2048];

static const char *xp_dependent(const char *uses, const char *requires_table, const char *body) {
    snprintf(xp_chunk, sizeof xp_chunk,
             "daukle.plugin{ api = 1, uses = { %s }, requires = { %s } }\n%s", uses, requires_table,
             body);
    return xp_chunk;
}

/* The one-requirement case, which is most of them: it builds the requires entry
   itself, so no caller has to hold a buffer for it. */
static const char *xp_dependent_on(const char *uses, const char *alias,
                                   const xp_artifact *artifact, const char *body) {
    char requirement[256];
    xp_requires_one(requirement, sizeof requirement, alias, artifact);
    return xp_dependent(uses, requirement, body);
}

static char xp_document[1024];

static const char *xp_manifest(const char *label, const char *url) {
    snprintf(xp_document, sizeof xp_document, "{\"plugins\":{\"%s\":{\"url\":\"%s\"}}}", label, url);
    return xp_document;
}

static const char *xp_manifest_two(const char *first, const char *first_url, const char *second,
                                   const char *second_url) {
    snprintf(xp_document, sizeof xp_document,
             "{\"plugins\":{\"%s\":{\"url\":\"%s\"},\"%s\":{\"url\":\"%s\"}}}", first, first_url,
             second, second_url);
    return xp_document;
}

static int xp_loads(const char *document_json, fr_registry **out_registry) {
    fr_error err;
    err.message[0] = '\0';
    *out_registry = NULL;

    int cache_was_enabled = fr_cache_enabled();
    fr_cache_set_enabled(0);
    fr_http_fn previous = fr_http_set_backend(xp_http);

    fr_registry *registry = NULL;
    cJSON *document = cJSON_Parse(document_json);
    int status = fr_build_registry(&registry, &err);
    if (status == FR_OK && xp_overflowed) status = FR_ERR;
    if (status == FR_OK && document == NULL) {
        fr_error_set(&err, "the test document is not json");
        status = FR_ERR;
    }
    if (status == FR_OK) status = fr_lua_runtime_begin(".", registry, &err);
    if (status == FR_OK) status = fr_plugins_load(registry, document, ".", &err);
    if (status != FR_OK && !xp_overflowed) {
        snprintf(xp_message, sizeof xp_message, "%s", err.message);
    }

    cJSON_Delete(document);
    fr_http_set_backend(previous);
    fr_cache_set_enabled(cache_was_enabled);
    *out_registry = registry;
    return status;
}

static void xp_done(fr_registry *registry) {
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    fr_plugins_report_clear();
}

static const char JAVA_URL[] = "https://x/java.tar";
static const char FOO_URL[] = "https://x/foo.tar";
static const char GRADLE_URL[] = "https://x/gradle.lua";
static const char MAVEN_URL[] = "https://x/maven.lua";

/* The language is declared AFTER the declaration call, so it reaches the
   registry only if the whole entry chunk ran rather than the pre-pass alone. */
static const char JAVA_ENTRY[] =
    "daukle.plugin{ api = 1, uses = {}, exports = { \"lib/coords\" } }\n"
    "daukle.language{ name = \"java-ran\","
    " apply = function(consumer, resolved, text) return text end }\n";

static const xp_artifact *xp_serve_java(const char *coords_body) {
    const char *names[] = { "lib/coords.lua" };
    const char *bodies[1];
    bodies[0] = coords_body;
    return xp_serve_plugin(JAVA_URL, JAVA_ENTRY, names, bodies, 1);
}

TEST a_dependent_reaches_an_exported_module(void) {
    xp_reset();
    const xp_artifact *java = xp_serve_java("return { parse = function() return \"parsed\" end }\n");
    char body[512];
    snprintf(body, sizeof body,
             "local coords = daukle.require(\"java:lib/coords\")\n"
             "daukle.language{ name = coords.parse(), %s }\n", XP_APPLY);
    xp_serve_chunk(GRADLE_URL, xp_dependent_on("", "java", java, body));

    fr_registry *registry = NULL;
    int status = xp_loads(xp_manifest("gradle", GRADLE_URL), &registry);
    int computed = status == FR_OK
                   && fr_registry_language(registry, "daukle.language/parsed") != NULL;
    xp_done(registry);

    ASSERT_EQm(xp_message, FR_OK, status);
    ASSERTm("the module's own answer never reached the registry", computed);
    PASS();
}

TEST a_member_the_provider_does_not_export_is_refused(void) {
    xp_reset();
    const xp_artifact *java = xp_serve_java("return {}\n");
    xp_serve_chunk(GRADLE_URL, xp_dependent_on("", "java", java,
                                            "daukle.require(\"java:internal/scratch\")\n"));

    fr_registry *registry = NULL;
    int status = xp_loads(xp_manifest("gradle", GRADLE_URL), &registry);
    xp_done(registry);

    ASSERT_EQ(FR_ERR, status);
    ASSERTm(xp_message, strstr(xp_message, "does not export") != NULL);
    ASSERTm(xp_message, strstr(xp_message, "lib/coords") != NULL);
    PASS();
}

TEST a_library_module_cannot_declare(void) {
    xp_reset();
    const xp_artifact *java = xp_serve_java("daukle.toolchain{ name = \"smuggled\" }\nreturn {}\n");
    xp_serve_chunk(GRADLE_URL, xp_dependent_on("", "java", java,
                                            "daukle.require(\"java:lib/coords\")\n"));

    fr_registry *registry = NULL;
    int status = xp_loads(xp_manifest("gradle", GRADLE_URL), &registry);
    xp_done(registry);

    ASSERT_EQ(FR_ERR, status);
    ASSERTm(xp_message, strstr(xp_message, "across a plugin boundary") != NULL);
    PASS();
}

/* The one test that separates a bare environment from the provider's own: an
   implementation that ran the member in either plugin's environment passes
   everything else here. */
TEST a_library_module_cannot_use_the_dependents_verbs(void) {
    xp_reset();
    const xp_artifact *java = xp_serve_java("daukle.fetch(\"https://x/never\")\nreturn {}\n");
    xp_serve_chunk(GRADLE_URL, xp_dependent_on("\"fetch\"", "java", java,
                                               "daukle.require(\"java:lib/coords\")\n"));

    fr_registry *registry = NULL;
    int status = xp_loads(xp_manifest("gradle", GRADLE_URL), &registry);
    xp_done(registry);

    ASSERT_EQ(FR_ERR, status);
    ASSERTm(xp_message, strstr(xp_message, "across a plugin boundary") != NULL);
    ASSERTm(xp_message, strstr(xp_message, "fetch") != NULL);
    PASS();
}

TEST two_dependents_get_their_own_instance(void) {
    xp_reset();
    const xp_artifact *java = xp_serve_java("return { seen = false }\n");
    char body[512];

    snprintf(body, sizeof body,
             "local coords = daukle.require(\"java:lib/coords\")\n"
             "coords.seen = true\n"
             "daukle.language{ name = \"gradle-saw\", %s }\n", XP_APPLY);
    xp_serve_chunk(GRADLE_URL, xp_dependent_on("", "java", java, body));

    snprintf(body, sizeof body,
             "local coords = daukle.require(\"java:lib/coords\")\n"
             "if coords.seen then error(\"the two dependents share one instance\") end\n"
             "daukle.language{ name = \"maven-saw\", %s }\n", XP_APPLY);
    xp_serve_chunk(MAVEN_URL, xp_dependent_on("", "java", java, body));

    fr_registry *registry = NULL;
    int status = xp_loads(xp_manifest_two("gradle", GRADLE_URL, "maven", MAVEN_URL), &registry);
    int both = status == FR_OK
               && fr_registry_language(registry, "daukle.language/gradle-saw") != NULL
               && fr_registry_language(registry, "daukle.language/maven-saw") != NULL;
    xp_done(registry);

    ASSERT_EQm(xp_message, FR_OK, status);
    ASSERTm("one of the two dependents never loaded", both);
    PASS();
}

TEST a_require_naming_an_undeclared_alias_is_refused(void) {
    xp_reset();
    const xp_artifact *java = xp_serve_java("return {}\n");
    xp_serve_chunk(GRADLE_URL, xp_dependent_on("", "java", java,
                                            "daukle.require(\"jaba:lib/coords\")\n"));

    fr_registry *registry = NULL;
    int status = xp_loads(xp_manifest("gradle", GRADLE_URL), &registry);
    xp_done(registry);

    ASSERT_EQ(FR_ERR, status);
    ASSERTm(xp_message, strstr(xp_message, "jaba") != NULL);
    ASSERTm(xp_message, strstr(xp_message, "it requires java") != NULL);
    PASS();
}

/* Section 3, on the integrated path: an implementation that loaded a dependency
   as an ordinary plugin passes every other test in this file. */
TEST a_dependencys_entry_chunk_never_runs(void) {
    xp_reset();
    const xp_artifact *java = xp_serve_java("return {}\n");
    char body[256];
    snprintf(body, sizeof body, "daukle.language{ name = \"gradle-ran\", %s }\n", XP_APPLY);
    xp_serve_chunk(GRADLE_URL, xp_dependent_on("", "java", java, body));

    fr_registry *registry = NULL;
    int status = xp_loads(xp_manifest("gradle", GRADLE_URL), &registry);
    int dependent_ran = status == FR_OK
                        && fr_registry_language(registry, "daukle.language/gradle-ran") != NULL;
    int provider_ran = fr_registry_language(registry, "daukle.language/java-ran") != NULL;
    xp_done(registry);

    ASSERT_EQm(xp_message, FR_OK, status);
    ASSERTm("the dependent's own language never reached the registry", dependent_ran);
    ASSERTm("the dependency's entry chunk ran", !provider_ran);
    PASS();
}

/* Section 7.2, both directions of one rule: the alias a module names is its own
   plugin's, and naming it from the dependent's chunk is refused. */
TEST a_library_module_requires_against_its_own_owner(void) {
    xp_reset();
    const char *foo_names[] = { "lib/thing.lua" };
    const char *foo_bodies[] = { "return { tag = \"from-foo\" }\n" };
    const xp_artifact *foo = xp_serve_plugin(
        FOO_URL, "daukle.plugin{ api = 1, uses = {}, exports = { \"lib/thing\" } }\n", foo_names,
        foo_bodies, 1);

    char foo_requirement[256];
    xp_requires_one(foo_requirement, sizeof foo_requirement, "foo", foo);

    char java_entry[512];
    snprintf(java_entry, sizeof java_entry,
             "daukle.plugin{ api = 1, uses = {}, exports = { \"lib/coords\" },"
             " requires = { %s } }\n", foo_requirement);
    const char *java_names[] = { "lib/coords.lua" };
    const char *java_bodies[] = {
        "local thing = daukle.require(\"foo:lib/thing\")\nreturn { tag = thing.tag }\n"
    };
    const xp_artifact *java = xp_serve_plugin(JAVA_URL, java_entry, java_names, java_bodies, 1);

    char body[512];
    snprintf(body, sizeof body,
             "local coords = daukle.require(\"java:lib/coords\")\n"
             "daukle.language{ name = coords.tag, %s }\n", XP_APPLY);
    xp_serve_chunk(GRADLE_URL, xp_dependent_on("", "java", java, body));

    fr_registry *registry = NULL;
    int status = xp_loads(xp_manifest("gradle", GRADLE_URL), &registry);
    int reached = status == FR_OK
                  && fr_registry_language(registry, "daukle.language/from-foo") != NULL;
    xp_done(registry);

    ASSERT_EQm(xp_message, FR_OK, status);
    ASSERTm("the module never reached its own plugin's requires", reached);

    xp_serve_chunk(GRADLE_URL, xp_dependent_on("", "java", java,
                                            "daukle.require(\"foo:lib/thing\")\n"));
    int refused = xp_loads(xp_manifest("gradle", GRADLE_URL), &registry);
    xp_done(registry);

    ASSERT_EQ(FR_ERR, refused);
    ASSERTm(xp_message, strstr(xp_message, "foo") != NULL);
    ASSERTm(xp_message, strstr(xp_message, "it requires java") != NULL);
    PASS();
}

/* Section 14's last case: the one arrangement where the memo could hand a
   full-environment instance across a boundary. */
TEST a_provider_that_is_also_a_root_plugin_still_serves_a_bare_instance(void) {
    xp_reset();
    const xp_artifact *java = xp_serve_java("daukle.fetch(\"https://x/never\")\nreturn {}\n");
    xp_serve_chunk(GRADLE_URL, xp_dependent_on("\"fetch\"", "java", java,
                                               "daukle.require(\"java:lib/coords\")\n"));

    fr_registry *registry = NULL;
    int status = xp_loads(xp_manifest_two("java", JAVA_URL, "gradle", GRADLE_URL), &registry);
    int provider_ran = fr_registry_language(registry, "daukle.language/java-ran") != NULL;
    xp_done(registry);

    ASSERTm("the provider's own entry chunk never ran", provider_ran);
    ASSERT_EQ(FR_ERR, status);
    ASSERTm(xp_message, strstr(xp_message, "across a plugin boundary") != NULL);
    PASS();
}

/* What the artifact half of the memo key buys, in both directions: two paths to
   one artifact's member share an instance, and the same member name in two
   artifacts does not. The second path here is a bare require inside a library
   module, which is the nesting the key has to see through. */
TEST one_dependent_gets_one_copy_of_a_module_per_artifact(void) {
    xp_reset();
    const char *foo_names[] = { "lib/counter.lua" };
    const char *foo_bodies[] = { "return { tag = \"foo\" }\n" };
    const xp_artifact *foo = xp_serve_plugin(
        FOO_URL, "daukle.plugin{ api = 1, uses = {}, exports = { \"lib/counter\" } }\n", foo_names,
        foo_bodies, 1);
    char foo_requirement[256];
    xp_requires_one(foo_requirement, sizeof foo_requirement, "foo", foo);

    const char *java_names[] = { "lib/coords.lua", "lib/counter.lua" };
    const char *java_bodies[] = {
        "local counter = daukle.require(\"lib/counter\")\ncounter.n = counter.n + 1\nreturn {}\n",
        "return { n = 0 }\n"
    };
    const xp_artifact *java = xp_serve_plugin(
        JAVA_URL,
        "daukle.plugin{ api = 1, uses = {}, exports = { \"lib/coords\", \"lib/counter\" } }\n",
        java_names, java_bodies, 2);

    char java_requirement[256];
    xp_requires_one(java_requirement, sizeof java_requirement, "java", java);
    char requires_both[512];
    snprintf(requires_both, sizeof requires_both, "%s, %s", java_requirement,
             foo_requirement);

    char body[768];
    snprintf(body, sizeof body,
             "daukle.require(\"java:lib/coords\")\n"
             "local direct = daukle.require(\"java:lib/counter\")\n"
             "local other = daukle.require(\"foo:lib/counter\")\n"
             "if direct.n ~= 1 then error(\"two paths gave two instances\") end\n"
             "if other.tag ~= \"foo\" then error(\"two artifacts shared one instance\") end\n"
             "daukle.language{ name = \"one-copy\", %s }\n", XP_APPLY);
    xp_serve_chunk(GRADLE_URL, xp_dependent("", requires_both, body));

    fr_registry *registry = NULL;
    int status = xp_loads(xp_manifest("gradle", GRADLE_URL), &registry);
    int held = status == FR_OK
               && fr_registry_language(registry, "daukle.language/one-copy") != NULL;
    xp_done(registry);

    ASSERT_EQm(xp_message, FR_OK, status);
    ASSERTm("the dependent's chunk never finished", held);
    PASS();
}

/* A nested sibling require inside a library module used to build that module's
   environment with the PROVIDER's own label ("java") in the slot
   outside_the_library_env's message calls the dependent, so the raise read
   "required by \"java\"" instead of naming gradle, the plugin that actually
   pulled java in. lib/coords is what gradle reached across the boundary;
   lib/util is coords's own bare sibling require, which is where the wrong
   label was built. */
TEST a_nested_sibling_library_refusal_names_the_true_dependent(void) {
    xp_reset();
    const char *java_names[] = { "lib/coords.lua", "lib/util.lua" };
    const char *java_bodies[] = {
        "local util = daukle.require(\"lib/util\")\nreturn { util = util }\n",
        "daukle.fetch(\"https://x/never\")\nreturn {}\n"
    };
    const xp_artifact *java = xp_serve_plugin(
        JAVA_URL, "daukle.plugin{ api = 1, uses = {}, exports = { \"lib/coords\" } }\n",
        java_names, java_bodies, 2);
    xp_serve_chunk(GRADLE_URL, xp_dependent_on("", "java", java,
                                            "daukle.require(\"java:lib/coords\")\n"));

    fr_registry *registry = NULL;
    int status = xp_loads(xp_manifest("gradle", GRADLE_URL), &registry);
    xp_done(registry);

    ASSERT_EQ(FR_ERR, status);
    ASSERTm(xp_message, strstr(xp_message, "required by \"gradle\"") != NULL);
    ASSERTm(xp_message, strstr(xp_message, "required by \"java\"") == NULL);
    PASS();
}

/* A chain of nine providers, each module reaching into the next. The acquisition
   depth cap alone would bound this at eight artifacts, so the dependent requires
   l0 and l5 directly: a root require puts an artifact back at depth 0, while the
   nesting keeps accumulating, which is what makes the owner-stack cap reachable
   rather than dead. */
TEST modules_nesting_deeper_than_the_cap_are_refused(void) {
    static const char *LINK_URL[9] = {
        "https://x/l0.tar", "https://x/l1.tar", "https://x/l2.tar", "https://x/l3.tar",
        "https://x/l4.tar", "https://x/l5.tar", "https://x/l6.tar", "https://x/l7.tar",
        "https://x/l8.tar"
    };
    static char requirement[9][256];
    const char *names[] = { "m.lua" };

    xp_reset();
    /* Built from the deepest back, because a requires entry carries the pin of
       what it names and a pin cannot be written before the bytes exist. */
    for (int index = 8; index >= 0; index--) {
        char entry[512];
        char member[128];
        if (index == 8) {
            snprintf(entry, sizeof entry,
                     "daukle.plugin{ api = 1, uses = {}, exports = { \"m\" } }\n");
            snprintf(member, sizeof member, "return { reached = true }\n");
        } else {
            snprintf(entry, sizeof entry,
                     "daukle.plugin{ api = 1, uses = {}, exports = { \"m\" },"
                     " requires = { %s } }\n", requirement[index + 1]);
            snprintf(member, sizeof member, "return daukle.require(\"l%d:m\")\n", index + 1);
        }
        const char *bodies[1];
        bodies[0] = member;
        const xp_artifact *link = xp_serve_plugin(LINK_URL[index], entry, names, bodies, 1);

        char alias[8];
        snprintf(alias, sizeof alias, "l%d", index);
        xp_requires_one(requirement[index], sizeof requirement[index], alias, link);
    }

    char requires_two[512];
    snprintf(requires_two, sizeof requires_two, "%s, %s", requirement[0], requirement[5]);
    xp_serve_chunk(GRADLE_URL,
                   xp_dependent("", requires_two, "daukle.require(\"l0:m\")\n"));

    fr_registry *registry = NULL;
    int status = xp_loads(xp_manifest("gradle", GRADLE_URL), &registry);
    xp_done(registry);

    ASSERT_EQ(FR_ERR, status);
    ASSERTm(xp_message, strstr(xp_message, "reach across more than 8 plugin boundaries") != NULL);
    PASS();
}

TEST an_alias_longer_than_the_limit_is_refused_as_too_long(void) {
    xp_reset();
    const xp_artifact *java = xp_serve_java("return {}\n");
    char body[256];
    char long_alias[FR_PLUGIN_MAX_ALIAS + 8];
    memset(long_alias, 'j', sizeof long_alias - 1);
    long_alias[sizeof long_alias - 1] = '\0';
    snprintf(body, sizeof body, "daukle.require(\"%s:lib/coords\")\n", long_alias);
    xp_serve_chunk(GRADLE_URL, xp_dependent_on("", "java", java, body));

    fr_registry *registry = NULL;
    int status = xp_loads(xp_manifest("gradle", GRADLE_URL), &registry);
    xp_done(registry);

    ASSERT_EQ(FR_ERR, status);
    ASSERTm(xp_message, strstr(xp_message, "longer than") != NULL);
    /* Not reported as an alias nobody declared, which is what a silent
       truncation to the limit would have said. */
    ASSERTm(xp_message, strstr(xp_message, "no dependency is required") == NULL);
    PASS();
}

/* Named for what it pins and nothing more: after a load whose library module
   raised, the NEXT load starts with no owner frame standing. It does not pin
   the pop in require_across_plugins. Nothing in BASE can catch a raise, so a
   leaked frame is observable only across a load boundary, and
   fr_lua_plugin_load's own owner_stack_depth resets already own that boundary;
   see the note on protected_run_member for why the two cannot be told apart. */
TEST no_owner_frame_survives_into_the_next_load(void) {
    xp_reset();
    const xp_artifact *java = xp_serve_java("error(\"the module raised\")\n");
    xp_serve_chunk(GRADLE_URL, xp_dependent_on("", "java", java,
                                               "daukle.require(\"java:lib/coords\")\n"));

    fr_registry *registry = NULL;
    int raised = xp_loads(xp_manifest("gradle", GRADLE_URL), &registry);
    xp_done(registry);
    ASSERT_EQ(FR_ERR, raised);
    ASSERTm(xp_message, strstr(xp_message, "the module raised") != NULL);

    /* A single-file dependent, so its own bare require must be refused with
       "this plugin is a single file": routed through a surviving frame it would
       instead read the PROVIDER's lib/coords, exports unchecked, and load. */
    xp_serve_chunk(GRADLE_URL, xp_dependent_on("", "java", java,
                                               "daukle.require(\"lib/coords\")\n"));
    int after = xp_loads(xp_manifest("gradle", GRADLE_URL), &registry);
    xp_done(registry);

    ASSERT_EQ(FR_ERR, after);
    ASSERTm(xp_message, strstr(xp_message, "single file") != NULL);
    PASS();
}

#define LIMIT_OWN_COUNT 40
#define LIMIT_LIB_COUNT 25

static const char *limit_test_lib_url;
static const char *limit_test_lib_body;
static size_t limit_test_lib_length;

/* Serves exactly one url, the way this test's own single dependency needs: any other request is a
   test bug, not something to answer silently. */
static int limit_test_serve(const char *url, const fr_http_header *headers, size_t header_count,
                            char **out_body, size_t *out_length, fr_error *err) {
    (void) headers;
    (void) header_count;
    if (limit_test_lib_url == NULL || strcmp(url, limit_test_lib_url) != 0) {
        fr_error_set(err, "no stub for %s", url);
        return FR_ERR;
    }
    char *copy = malloc(limit_test_lib_length + 1);
    if (copy == NULL) {
        fr_error_set(err, "out of memory serving %s", url);
        return FR_ERR;
    }
    memcpy(copy, limit_test_lib_body, limit_test_lib_length);
    copy[limit_test_lib_length] = '\0';
    *out_body = copy;
    *out_length = limit_test_lib_length;
    return FR_OK;
}

/* FR_PLUGIN_MODULE_LIMIT's 64 slots are one budget shared by the whole load, not a separate 64 for
   every artifact: the dependent here owns 40 modules and requires a library that exports 25, 65 in
   total, over the limit, even though neither the dependent's own module count nor the library's
   export count individually is. An implementation that gave every artifact its own 64-slot budget
   (the pre-branch shape) would pass this. Built through fr_plugin_deps_acquire and
   fr_lua_plugin_load directly, the same two calls load_one makes, rather than through
   fr_plugins_load, because there is no manifest step this test needs. */
TEST the_module_limit_is_shared_across_a_load(void) {
    static char lib_archive[32768];
    static char root_archive[65536];
    static char lib_entry[1024];
    static char root_entry[4096];
    char lib_names[LIMIT_LIB_COUNT][16];
    char own_names[LIMIT_OWN_COUNT][16];
    static const char TINY_MODULE[] = "return {}\n";
    fr_error err;
    int index;

    size_t entry_used = (size_t) snprintf(lib_entry, sizeof lib_entry,
                                          "daukle.plugin{ api = 1, uses = {}, exports = {");
    for (index = 0; index < LIMIT_LIB_COUNT; index++) {
        entry_used += (size_t) snprintf(lib_entry + entry_used, sizeof lib_entry - entry_used,
                                        "%s\"n%d\"", index == 0 ? " " : ", ", index);
    }
    entry_used += (size_t) snprintf(lib_entry + entry_used, sizeof lib_entry - entry_used, " } }\n");

    memset(lib_archive, 0, sizeof lib_archive);
    size_t offset = fr_test_tar_append(lib_archive, 0, "plugin.lua", '0', lib_entry, entry_used);
    for (index = 0; index < LIMIT_LIB_COUNT; index++) {
        snprintf(lib_names[index], sizeof lib_names[index], "n%d.lua", index);
        offset = fr_test_tar_append(lib_archive, offset, lib_names[index], '0', TINY_MODULE,
                                    strlen(TINY_MODULE));
    }
    size_t lib_length = fr_test_tar_end(lib_archive, offset);
    char lib_digest[65];
    fr_sha256_hex(lib_archive, lib_length, lib_digest);

    entry_used = (size_t) snprintf(root_entry, sizeof root_entry,
                                   "daukle.plugin{ api = 1, uses = {}, requires = { lib = { url ="
                                   " \"https://x/limit-lib.tar\", sha256 = \"%s\" } } }\n",
                                   lib_digest);
    for (index = 0; index < LIMIT_OWN_COUNT; index++) {
        entry_used += (size_t) snprintf(root_entry + entry_used, sizeof root_entry - entry_used,
                                        "daukle.require(\"m%d\")\n", index);
    }
    for (index = 0; index < LIMIT_LIB_COUNT; index++) {
        entry_used += (size_t) snprintf(root_entry + entry_used, sizeof root_entry - entry_used,
                                        "daukle.require(\"lib:n%d\")\n", index);
    }

    memset(root_archive, 0, sizeof root_archive);
    offset = fr_test_tar_append(root_archive, 0, "plugin.lua", '0', root_entry, entry_used);
    for (index = 0; index < LIMIT_OWN_COUNT; index++) {
        snprintf(own_names[index], sizeof own_names[index], "m%d.lua", index);
        offset = fr_test_tar_append(root_archive, offset, own_names[index], '0', TINY_MODULE,
                                    strlen(TINY_MODULE));
    }
    size_t root_length = fr_test_tar_end(root_archive, offset);

    fr_registry *registry = fr_registry_create();
    ASSERT_EQ(FR_OK, fr_lua_runtime_begin(".", registry, &err));

    int cache_was_enabled = fr_cache_enabled();
    fr_cache_set_enabled(0);
    fr_http_fn previous_backend = fr_http_set_backend(limit_test_serve);
    limit_test_lib_url = "https://x/limit-lib.tar";
    limit_test_lib_body = lib_archive;
    limit_test_lib_length = lib_length;

    fr_plugin_source *source = NULL;
    int opened = fr_plugin_source_open_bytes(root_archive, root_length, &source, &err) == FR_OK;

    fr_plugin_declaration declaration;
    memset(&declaration, 0, sizeof declaration);
    int declared = opened
        && fr_plugins_read_declaration(root_entry, entry_used, "@limit-root", "plugin", "gradle",
                                       &declaration, &err) == FR_OK;

    fr_plugin_deps *deps = NULL;
    int acquired = declared
        && fr_plugin_deps_acquire(&declaration, NULL, NULL, 0, &deps, &err) == FR_OK;

    int status = FR_ERR;
    static char message[512];
    message[0] = '\0';
    if (acquired) {
        const char *chunk = NULL;
        size_t chunk_length = 0;
        fr_plugin_source_entry(source, &chunk, &chunk_length, &err);
        status = fr_lua_plugin_load(chunk, chunk_length, "@limit-root", NULL, 0, source, deps, &err);
        if (status != FR_OK) snprintf(message, sizeof message, "%s", err.message);
    } else {
        snprintf(message, sizeof message, "%s", err.message);
    }

    fr_plugin_deps_close(deps);
    fr_plugins_free_declaration(&declaration);
    fr_plugin_source_close(source);
    fr_http_set_backend(previous_backend);
    fr_cache_set_enabled(cache_was_enabled);
    fr_lua_runtime_shutdown();
    fr_registry_destroy(registry);

    ASSERTm(message, opened);
    ASSERTm(message, declared);
    ASSERTm(message, acquired);
    ASSERT_EQ(FR_ERR, status);
    ASSERTm(message, strstr(message, "a load may require at most 64 modules") != NULL);
    PASS();
}

TEST a_member_name_carrying_a_second_colon_is_refused(void) {
    xp_reset();
    const xp_artifact *java = xp_serve_java("return {}\n");
    xp_serve_chunk(GRADLE_URL, xp_dependent_on("", "java", java,
                                            "daukle.require(\"java:lib:coords\")\n"));

    fr_registry *registry = NULL;
    int status = xp_loads(xp_manifest("gradle", GRADLE_URL), &registry);
    xp_done(registry);

    ASSERT_EQ(FR_ERR, status);
    ASSERTm(xp_message, strstr(xp_message, "at most one \":\"") != NULL);
    PASS();
}

/* Child spec 3's correction 3: the escape check runs against the whole name
   before the split, because a windows drive letter carries a colon too. */
TEST a_drive_letter_is_an_escape_rather_than_an_alias(void) {
    xp_reset();
    const xp_artifact *java = xp_serve_java("return {}\n");
    xp_serve_chunk(GRADLE_URL, xp_dependent_on("", "java", java,
                                            "daukle.require(\"C:/x\")\n"));

    fr_registry *registry = NULL;
    int status = xp_loads(xp_manifest("gradle", GRADLE_URL), &registry);
    xp_done(registry);

    ASSERT_EQ(FR_ERR, status);
    ASSERTm(xp_message, strstr(xp_message, "may not leave the plugin") != NULL);
    /* On the WHOLE name, because that is the only assertion the wrong ordering
       fails: splitting first leaves the alias "C" and the member "/x", and
       "/x" is refused with the same clause, under its own spelling. */
    ASSERTm(xp_message, strstr(xp_message, "\"C:/x\"") != NULL);
    PASS();
}

TEST a_publisher_reaches_the_registry_under_the_publish_prefix(void) {
    fr_registry *registry = fr_registry_create();
    fr_error err;
    int began = fr_lua_runtime_begin(".", registry, &err) == FR_OK;
    const char *chunk =
        "daukle.publisher{ name = 'github', publish = function(context) end }\n";
    int status = fr_lua_plugin_load(chunk, strlen(chunk), "github.lua", NULL, 0, NULL, NULL, &err);
    const fr_task_plugin *task = fr_registry_task(registry, "daukle.task/publish:github");
    int found = task != NULL && task->run != NULL;

    fr_lua_runtime_shutdown();
    fr_registry_destroy(registry);

    ASSERT(began);
    ASSERT_EQ(FR_OK, status);
    ASSERT(found);
    PASS();
}

TEST a_publisher_may_declare_exec(void) {
    /* The grant itself: the same chunk declaring a source or a language is refused
       outright for having declared exec. */
    fr_registry *registry = fr_registry_create();
    fr_error err;
    int began = fr_lua_runtime_begin(".", registry, &err) == FR_OK;
    const char *uses[] = { "exec" };
    const char *chunk =
        "daukle.publisher{ name = 'github', publish = function(context) end }\n";
    int status = fr_lua_plugin_load(chunk, strlen(chunk), "github.lua", uses, 1, NULL, NULL, &err);
    static char message[512];
    snprintf(message, sizeof message, "%s", err.message);

    fr_lua_runtime_shutdown();
    fr_registry_destroy(registry);

    ASSERT(began);
    ASSERT_EQm(message, FR_OK, status);
    PASS();
}

TEST a_publisher_needs_a_publish_function(void) {
    fr_registry *registry = fr_registry_create();
    fr_error err;
    int began = fr_lua_runtime_begin(".", registry, &err) == FR_OK;
    const char *chunk = "daukle.publisher{ name = 'github' }\n";
    int status = fr_lua_plugin_load(chunk, strlen(chunk), "github.lua", NULL, 0, NULL, NULL, &err);
    char message[512];
    snprintf(message, sizeof message, "%s", err.message);

    fr_lua_runtime_shutdown();
    fr_registry_destroy(registry);

    ASSERT(began);
    ASSERT_EQ(FR_ERR, status);
    ASSERT(strstr(message, "needs a publish function") != NULL);
    PASS();
}

TEST a_resolver_chunk_may_not_declare_a_publisher(void) {
    fr_error err;
    fr_registry *registry = fr_registry_create();
    int began = fr_lua_runtime_begin(".", registry, &err) == FR_OK;
    const char *chunk =
        "daukle.plugin{ api = 1, uses = {} }\n"
        "daukle.resolver{ resolve = function(c) return { url = c } end }\n"
        "daukle.publisher{ name = 'github', publish = function(context) end }\n";
    int status = load_resolver_chunk(chunk, "r.lua", NULL, 0, &err);
    char message[512];
    snprintf(message, sizeof message, "%s", err.message);

    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();

    ASSERT(began);
    ASSERT_EQ(FR_ERR, status);
    ASSERT(strstr(message, "a resolver chunk declares only a resolver") != NULL);
    PASS();
}

TEST a_publisher_chunk_declaring_a_task_is_told_why(void) {
    fr_registry *registry = fr_registry_create();
    fr_error err;
    int began = fr_lua_runtime_begin(".", registry, &err) == FR_OK;
    const char *uses[] = { "exec" };
    const char *chunk =
        "daukle.publisher{ name = 'github', publish = function(context) end }\n"
        "daukle.task{ name = 'extra', run = function(context) end }\n";
    int status = fr_lua_plugin_load(chunk, strlen(chunk), "github.lua", uses, 1, NULL, NULL, &err);
    char message[512];
    snprintf(message, sizeof message, "%s", err.message);

    fr_lua_runtime_shutdown();
    fr_registry_destroy(registry);

    ASSERT(began);
    ASSERT_EQ(FR_ERR, status);
    ASSERT(strstr(message, "a publisher declares publishers, not tasks") != NULL);
    PASS();
}

TEST a_publish_callback_reads_its_destination(void) {
    fr_registry *registry = fr_registry_create();
    fr_error err;
    int began = fr_lua_runtime_begin(".", registry, &err) == FR_OK;
    const char *chunk =
        "daukle.publisher{ name = 'github', publish = function(context)\n"
        "  error(context.publish.name .. '|' .. tostring(context.publish.config.tokenEnv)"
        " .. '|' .. tostring(context.publish.config.from), 0)\n"
        "end }\n";
    int loaded =
        fr_lua_plugin_load(chunk, strlen(chunk), "github.lua", NULL, 0, NULL, NULL, &err) == FR_OK;
    const fr_task_plugin *task = fr_registry_task(registry, "daukle.task/publish:github");

    cJSON *root = cJSON_Parse(
        "{\"schema\":1,\"project\":\"me/app\",\"version\":\"1.0.0\",\"modules\":{},"
        "\"toolchains\":{\"gradle\":\"8.10\"},"
        "\"publish\":{\"github\":{\"from\":\"gradle\",\"tokenEnv\":\"GITHUB_TOKEN\"}}}");
    fr_manifest manifest;
    int parsed = fr_manifest_from_document(root, "daukle.toml", &manifest, &err) == FR_OK;

    char message[512];
    message[0] = '\0';
    int status = FR_OK;
    if (task != NULL && parsed) {
        fr_task_run_context context;
        memset(&context, 0, sizeof context);
        context.name = "publish:github";
        context.toolchain = &manifest.toolchains[0];
        context.publish = &manifest.publishes[0];
        context.project = manifest.self.project;
        context.version = "1.0.0";
        context.root = "build/daukle";
        context.derived_dir_relative = "build/daukle/gradle";
        status = task->run(task->state, &context, &err);
        snprintf(message, sizeof message, "%s", err.message);
    }
    int found = task != NULL;

    if (parsed) fr_manifest_free(&manifest);
    fr_lua_runtime_shutdown();
    fr_registry_destroy(registry);

    ASSERT(began);
    ASSERT(loaded);
    ASSERT(found);
    ASSERT(parsed);
    ASSERT_EQ(FR_ERR, status);
    ASSERT(strstr(message, "github|GITHUB_TOKEN|nil") != NULL);
    PASS();
}

GREATEST_MAIN_DEFS();

/* Most cases in this file assert before their cleanup, so a single failure used
   to leave the lua runtime open and every later case failed against it: three
   separate root causes once hid behind nine failures. Shutting down here makes
   that impossible whatever order a case asserts in, because the call is
   idempotent and a no-op when nothing is open. It does NOT excuse the ordering,
   which still leaks the registry of whichever case failed; it stops one failure
   from reporting as dozens. */
static void close_any_leaked_runtime(void *udata) {
    (void) udata;
    fr_lua_runtime_shutdown();
}

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    GREATEST_SET_TEARDOWN_CB(close_any_leaked_runtime, NULL);
    RUN_TEST(a_script_beside_a_toml_manifest_changes_it);
    RUN_TEST(a_failing_script_names_its_file);
    RUN_TEST(daukle_log_routes_through_the_caller_supplied_sink);
    RUN_TEST(an_untouched_empty_array_survives_the_round_trip);
    RUN_TEST(a_script_clearing_an_array_to_an_empty_table_still_yields_an_array);
    RUN_TEST(a_script_clearing_an_object_to_an_empty_table_stays_an_object);
    RUN_TEST(a_lua_only_manifest_may_empty_the_arrays_it_declares);
    RUN_TEST(a_script_registers_a_language_plugin);
    RUN_TEST(a_script_may_not_declare_one_capability_twice);
    RUN_TEST(a_script_registered_source_plugin_resolves_through_sync);
    RUN_TEST(a_lua_root_manifest_loads_the_plugins_it_declares);
    RUN_TEST(a_plugin_name_too_long_for_the_capability_buffer_is_rejected);
    RUN_TEST(a_language_plugin_that_raises_an_error_produces_a_clean_failure);
    RUN_TEST(a_language_plugin_that_returns_a_non_string_is_refused);
    RUN_TEST(a_source_plugin_that_raises_an_error_produces_a_clean_failure);
    RUN_TEST(a_low_instruction_limit_stops_a_script_that_would_otherwise_finish);
    RUN_TEST(a_low_memory_limit_fails_a_script_that_would_otherwise_finish);
    RUN_TEST(a_script_raising_a_table_produces_a_clean_failure);
    RUN_TEST(refuses_a_second_load_that_redeclares_a_plugin_capability);
    RUN_TEST(records_every_environment_variable_a_script_read);
    RUN_TEST(an_unconvertible_env_read_record_does_not_fail_the_load);
    RUN_TEST(a_second_load_replaces_the_recorded_environment_reads);
    RUN_TEST(two_plugin_loads_share_one_state);
    RUN_TEST(a_second_registry_is_refused_while_the_first_holds_plugins);
    RUN_TEST(a_second_base_directory_is_refused_naming_both);
    RUN_TEST(the_same_base_directory_spelled_differently_still_reuses_the_runtime);
    RUN_TEST(a_plugin_declares_a_toolchain_and_it_reaches_the_registry);
    RUN_TEST(a_toolchain_needs_a_generate_function);
    RUN_TEST(a_toolchain_generate_bridges_project_config_root_and_host);
    RUN_TEST(a_toolchain_config_excludes_the_reserved_dependencies_key);
    RUN_TEST(the_generate_context_carries_the_toolchain_version);
    RUN_TEST(the_generate_context_still_hides_dependencies_from_config);
    RUN_TEST(a_provisioned_root_is_readable_as_a_toolreport_row);
    RUN_TEST(an_installed_tool_is_readable_as_a_toolreport_row_with_no_provisioned_flag);
    RUN_TEST(a_toolchain_generate_must_return_a_table);
    RUN_TEST(a_toolchain_generate_refuses_a_non_string_file_path);
    RUN_TEST(a_toolchain_generate_refuses_a_non_string_file_contents);
    RUN_TEST(a_task_is_declared_and_registered);
    RUN_TEST(a_task_cannot_claim_a_toolchain_its_chunk_did_not_declare);
    RUN_TEST(a_task_without_a_toolchain_may_not_keep_provision);
    RUN_TEST(a_task_beside_a_toolchain_still_keeps_provision);
    RUN_TEST(an_aggregator_needs_no_run);
    RUN_TEST(a_second_chunks_task_cannot_reuse_the_first_chunks_toolchain);
    RUN_TEST(the_same_chunk_reordered_is_the_difference_between_refused_and_accepted);
    RUN_TEST(exec_for_a_task_above_its_toolchain_is_refused_naming_the_order);
    RUN_TEST(a_hostile_index_metatable_on_the_task_table_is_never_consulted);
    RUN_TEST(a_hostile_index_metatable_on_the_toolchain_table_is_never_consulted);
    RUN_TEST(a_chunk_not_acquired_as_a_resolver_may_not_declare_one);
    RUN_TEST(a_resolver_chunk_declares_a_resolver);
    RUN_TEST(a_resolver_chunk_may_not_also_declare_a_language);
    RUN_TEST(a_chunk_that_declared_a_language_may_not_then_declare_a_resolver);
    RUN_TEST(a_resolver_may_not_declare_exec);
    RUN_TEST(a_resolver_may_not_declare_tool);
    RUN_TEST(a_resolver_needs_a_resolve_function);
    RUN_TEST(a_hostile_metatable_cannot_supply_the_resolve_function);
    RUN_TEST(a_hostile_metatable_returning_a_function_is_never_used_as_resolve);
    RUN_TEST(a_chunk_declaring_no_resolver_reports_none_declared);
    RUN_TEST(a_refused_resolver_chunk_reports_none_declared);
    RUN_TEST(a_language_plugin_declaring_provision_is_refused);
    RUN_TEST(a_source_plugin_declaring_provision_is_refused);
    RUN_TEST(a_resolver_plugin_declaring_provision_is_refused);
    RUN_TEST(a_toolchain_plugin_declaring_provision_loads);
    RUN_TEST(a_module_supplies_what_the_entry_chunk_requires);
    RUN_TEST(a_module_may_declare_a_language);
    RUN_TEST(a_module_may_not_use_a_verb_the_plugin_did_not_declare);
    RUN_TEST(daukle_require_before_daukle_plugin_is_refused);
    RUN_TEST(a_plugin_declaring_no_uses_may_still_require);
    RUN_TEST(a_module_required_twice_runs_once);
    RUN_TEST(a_cycle_between_modules_is_refused);
    RUN_TEST(a_dependent_reaches_an_exported_module);
    RUN_TEST(a_member_the_provider_does_not_export_is_refused);
    RUN_TEST(a_library_module_cannot_declare);
    RUN_TEST(a_library_module_cannot_use_the_dependents_verbs);
    RUN_TEST(two_dependents_get_their_own_instance);
    RUN_TEST(a_require_naming_an_undeclared_alias_is_refused);
    RUN_TEST(a_dependencys_entry_chunk_never_runs);
    RUN_TEST(a_library_module_requires_against_its_own_owner);
    RUN_TEST(a_provider_that_is_also_a_root_plugin_still_serves_a_bare_instance);
    RUN_TEST(one_dependent_gets_one_copy_of_a_module_per_artifact);
    RUN_TEST(a_nested_sibling_library_refusal_names_the_true_dependent);
    RUN_TEST(modules_nesting_deeper_than_the_cap_are_refused);
    RUN_TEST(an_alias_longer_than_the_limit_is_refused_as_too_long);
    RUN_TEST(no_owner_frame_survives_into_the_next_load);
    RUN_TEST(the_module_limit_is_shared_across_a_load);
    RUN_TEST(a_member_name_carrying_a_second_colon_is_refused);
    RUN_TEST(a_drive_letter_is_an_escape_rather_than_an_alias);
    RUN_TEST(a_publisher_reaches_the_registry_under_the_publish_prefix);
    RUN_TEST(a_publisher_may_declare_exec);
    RUN_TEST(a_publisher_needs_a_publish_function);
    RUN_TEST(a_resolver_chunk_may_not_declare_a_publisher);
    RUN_TEST(a_publisher_chunk_declaring_a_task_is_told_why);
    RUN_TEST(a_publish_callback_reads_its_destination);
    GREATEST_MAIN_END();
}
