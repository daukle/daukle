#include "greatest.h"
#include "config_lua.h"
#include "config_toml.h"
#include "error.h"
#include "manifest.h"
#include "plugins.h"
#include "registry.h"
#include "resolve.h"

#include "cJSON.h"

#include <string.h>

/* The path source is a lua plugin, and the plugin's daukle.read is bounded by
   the directory the runtime opens on, so the fixture directory is both where
   the plugin copy lives and where its producer must sit. The toml format is
   registered because the plugin hands what it read to daukle.parse. */
static fr_registry *with_path_source(const char *fixture_dir) {
    fr_registry *registry = fr_registry_create();
    cJSON *document = cJSON_Parse("{\"plugins\":{\"path\":\"./plugins/path.lua\"}}");
    fr_error err;
    int loaded = fr_registry_add_config(registry, &FR_CONFIG_TOML, &err) == FR_OK
                 && fr_lua_runtime_begin(fixture_dir, registry, &err) == FR_OK
                 && fr_plugins_load(registry, document, fixture_dir, &err) == FR_OK;
    cJSON_Delete(document);
    if (loaded) return registry;

    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    return NULL;
}

static void without_path_source(fr_registry *registry) {
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    fr_plugins_report_clear();
}

static void *state_seen_by_the_source;

static int recording_load(void *state, const char *project, const cJSON *block,
                          const char *base_dir, fr_project *out, fr_error *err) {
    (void) project; (void) block; (void) base_dir;
    memset(out, 0, sizeof *out);
    state_seen_by_the_source = state;
    fr_error_set(err, "recorded");
    return FR_ERR;
}

TEST hands_the_source_plugin_its_own_state(void) {
    fr_manifest manifest; fr_error err;
    fr_manifest_read("test/fixtures/consumer/daukle.json", &manifest, &err);
    fr_registry *registry = fr_registry_create();
    int own_state = 0;
    fr_source_plugin recorder = { "daukle.source/path", recording_load, &own_state };
    fr_registry_add_source(registry, &recorder, &err);
    state_seen_by_the_source = NULL;

    fr_resolved *items = NULL; size_t count = 0;
    ASSERT_EQ(FR_ERR, fr_resolve_consumer(&manifest.consumers[0], &manifest, "test/fixtures/consumer",
                                          registry, &items, &count, &err));
    ASSERT(state_seen_by_the_source == &own_state);

    fr_registry_destroy(registry);
    fr_manifest_free(&manifest);
    PASS();
}

TEST pulls_in_transitive_requires(void) {
    fr_manifest manifest; fr_error err;
    ASSERT_EQ(FR_OK, fr_manifest_read("test/fixtures/consumer/daukle.json", &manifest, &err));
    fr_registry *registry = with_path_source("test/fixtures/consumer");
    ASSERT(registry != NULL);

    fr_resolved *items = NULL; size_t count = 0;
    ASSERT_EQ(FR_OK, fr_resolve_consumer(&manifest.consumers[0], &manifest,
                                         "test/fixtures/consumer", registry, &items, &count, &err));
    ASSERT_EQ(2, (int) count);
    ASSERT_STR_EQ("contracts", items[0].module);
    ASSERT_STR_EQ("ir", items[1].module);
    const cJSON *coordinate = cJSON_GetObjectItemCaseSensitive(items[0].block, "coordinate");
    ASSERT_STR_EQ("forebay:basekit:5.0.0:contracts", coordinate->valuestring);

    fr_resolved_free(items, count);
    without_path_source(registry);
    fr_manifest_free(&manifest);
    PASS();
}

TEST reports_a_module_that_has_no_block_for_the_language(void) {
    fr_manifest manifest;
    fr_error err;
    ASSERT_EQ(FR_OK, fr_manifest_read("test/fixtures/consumer/daukle-no-language-block.json", &manifest, &err));

    fr_registry *registry = with_path_source("test/fixtures/consumer");
    ASSERT(registry != NULL);

    fr_resolved *resolved = NULL;
    size_t count = 0;
    ASSERT_EQ(FR_ERR, fr_resolve_consumer(&manifest.consumers[0], &manifest, "test/fixtures/consumer",
                                          registry, &resolved, &count, &err));
    ASSERT(strstr(err.message, "\"ir\"") != NULL);
    ASSERT(strstr(err.message, "\"npm\"") != NULL);

    without_path_source(registry);
    fr_manifest_free(&manifest);
    PASS();
}

TEST deduplicates_a_module_reached_twice(void) {
    fr_manifest manifest; fr_error err;
    fr_manifest_read("test/fixtures/consumer-dup/daukle.json", &manifest, &err);
    fr_registry *registry = with_path_source("test/fixtures/consumer-dup");
    ASSERT(registry != NULL);

    fr_resolved *items = NULL; size_t count = 0;
    ASSERT_EQ(FR_OK, fr_resolve_consumer(&manifest.consumers[0], &manifest,
                                         "test/fixtures/consumer-dup", registry, &items, &count, &err));
    ASSERT_EQ(2, (int) count);

    fr_resolved_free(items, count);
    without_path_source(registry);
    fr_manifest_free(&manifest);
    PASS();
}

TEST rejects_a_version_outside_the_range(void) {
    fr_manifest manifest; fr_error err;
    fr_manifest_read("test/fixtures/consumer-badrange/daukle.json", &manifest, &err);
    fr_registry *registry = with_path_source("test/fixtures/consumer-badrange");
    ASSERT(registry != NULL);

    fr_resolved *items = NULL; size_t count = 0;
    ASSERT_EQ(FR_ERR, fr_resolve_consumer(&manifest.consumers[0], &manifest,
                                          "test/fixtures/consumer-badrange", registry, &items, &count, &err));
    ASSERT(strstr(err.message, "forebay/basekit") != NULL);
    ASSERT(strstr(err.message, "5.0.0") != NULL);

    without_path_source(registry);
    fr_manifest_free(&manifest);
    PASS();
}

TEST reports_a_module_absent_from_the_language(void) {
    fr_manifest manifest; fr_error err;
    fr_manifest_read("test/fixtures/consumer-noloader/daukle.json", &manifest, &err);
    fr_registry *registry = with_path_source("test/fixtures/consumer-noloader");
    ASSERT(registry != NULL);

    fr_resolved *items = NULL; size_t count = 0;
    ASSERT_EQ(FR_ERR, fr_resolve_consumer(&manifest.consumers[0], &manifest,
                                          "test/fixtures/consumer-noloader", registry, &items, &count, &err));
    ASSERT(strstr(err.message, "loader") != NULL);
    ASSERT(strstr(err.message, "gradle") != NULL);

    without_path_source(registry);
    fr_manifest_free(&manifest);
    PASS();
}

TEST reports_an_unknown_module_by_name(void) {
    fr_manifest manifest; fr_error err;
    fr_manifest_read("test/fixtures/consumer-unknown/daukle.json", &manifest, &err);
    fr_registry *registry = with_path_source("test/fixtures/consumer-unknown");
    ASSERT(registry != NULL);

    fr_resolved *items = NULL; size_t count = 0;
    ASSERT_EQ(FR_ERR, fr_resolve_consumer(&manifest.consumers[0], &manifest,
                                          "test/fixtures/consumer-unknown", registry, &items, &count, &err));
    ASSERT(strstr(err.message, "nope") != NULL);

    without_path_source(registry);
    fr_manifest_free(&manifest);
    PASS();
}

TEST rejects_a_project_that_does_not_match_its_source(void) {
    fr_manifest manifest; fr_error err;
    fr_manifest_read("test/fixtures/consumer-mismatch/daukle.json", &manifest, &err);
    fr_registry *registry = with_path_source("test/fixtures/consumer-mismatch");
    ASSERT(registry != NULL);

    fr_resolved *items = NULL; size_t count = 0;
    ASSERT_EQ(FR_ERR, fr_resolve_consumer(&manifest.consumers[0], &manifest,
                                          "test/fixtures/consumer-mismatch", registry, &items, &count, &err));
    ASSERT(strstr(err.message, "forebay/basekit") != NULL);
    ASSERT(strstr(err.message, "forebay/not-basekit") != NULL);

    without_path_source(registry);
    fr_manifest_free(&manifest);
    PASS();
}

TEST resolves_no_modules_for_any_language_without_looking_up_a_block(void) {
    fr_manifest manifest; fr_error err;
    fr_manifest_read("test/fixtures/consumer-badlanguage/daukle.json", &manifest, &err);
    fr_registry *registry = with_path_source("test/fixtures/consumer-badlanguage");
    ASSERT(registry != NULL);

    fr_resolved *items = NULL; size_t count = 0;
    ASSERT_EQ(FR_OK, fr_resolve_consumer(&manifest.consumers[0], &manifest,
                                         "test/fixtures/consumer-badlanguage", registry, &items, &count, &err));
    ASSERT_EQ(0, (int) count);

    fr_resolved_free(items, count);
    without_path_source(registry);
    fr_manifest_free(&manifest);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(hands_the_source_plugin_its_own_state);
    RUN_TEST(pulls_in_transitive_requires);
    RUN_TEST(reports_a_module_that_has_no_block_for_the_language);
    RUN_TEST(deduplicates_a_module_reached_twice);
    RUN_TEST(rejects_a_version_outside_the_range);
    RUN_TEST(reports_a_module_absent_from_the_language);
    RUN_TEST(reports_an_unknown_module_by_name);
    RUN_TEST(rejects_a_project_that_does_not_match_its_source);
    RUN_TEST(resolves_no_modules_for_any_language_without_looking_up_a_block);
    GREATEST_MAIN_END();
}
