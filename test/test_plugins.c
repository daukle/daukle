#include "greatest.h"

#include "cJSON.h"
#include "config.h"
#include "config_lua.h"
#include "manifest.h"
#include "plugins.h"
#include "registry.h"
#include "sync.h"

#include <string.h>

static cJSON *document_from(const char *json) {
    cJSON *document = cJSON_Parse(json);
    return document;
}

TEST parses_the_string_coordinate_form(void) {
    cJSON *document = document_from("{\"plugins\":{\"npm\":\"daukle/npm@^1.0.0\"}}");
    fr_plugin_entry *entries = NULL; size_t count = 0; fr_error err;

    ASSERT_EQ(FR_OK, fr_plugins_parse(document, &entries, &count, &err));
    ASSERT_EQ(1, (int) count);
    ASSERT_STR_EQ("npm", entries[0].label);
    ASSERT_EQ(FR_PLUGIN_REMOTE, entries[0].kind);
    ASSERT_STR_EQ("daukle/npm", entries[0].repo);
    ASSERT_STR_EQ("^1.0.0", entries[0].version);
    ASSERT(entries[0].sha256 == NULL);

    fr_plugins_free(entries, count);
    cJSON_Delete(document);
    PASS();
}

TEST parses_the_table_form_with_a_pin(void) {
    cJSON *document = document_from(
        "{\"plugins\":{\"gradle\":{\"repo\":\"daukle/gradle\",\"version\":\"^2.0.0\","
        "\"sha256\":\"abc123\"}}}");
    fr_plugin_entry *entries = NULL; size_t count = 0; fr_error err;

    ASSERT_EQ(FR_OK, fr_plugins_parse(document, &entries, &count, &err));
    ASSERT_EQ(1, (int) count);
    ASSERT_EQ(FR_PLUGIN_REMOTE, entries[0].kind);
    ASSERT_STR_EQ("daukle/gradle", entries[0].repo);
    ASSERT_STR_EQ("abc123", entries[0].sha256);

    fr_plugins_free(entries, count);
    cJSON_Delete(document);
    PASS();
}

TEST parses_a_local_path(void) {
    cJSON *document = document_from("{\"plugins\":{\"mine\":\"./plugins/mine.lua\"}}");
    fr_plugin_entry *entries = NULL; size_t count = 0; fr_error err;

    ASSERT_EQ(FR_OK, fr_plugins_parse(document, &entries, &count, &err));
    ASSERT_EQ(FR_PLUGIN_LOCAL, entries[0].kind);
    ASSERT_STR_EQ("./plugins/mine.lua", entries[0].path);

    fr_plugins_free(entries, count);
    cJSON_Delete(document);
    PASS();
}

TEST an_absent_plugins_table_yields_no_entries(void) {
    cJSON *document = document_from("{\"schema\":1}");
    fr_plugin_entry *entries = NULL; size_t count = 0; fr_error err;

    ASSERT_EQ(FR_OK, fr_plugins_parse(document, &entries, &count, &err));
    ASSERT_EQ(0, (int) count);

    fr_plugins_free(entries, count);
    cJSON_Delete(document);
    PASS();
}

TEST rejects_a_coordinate_with_no_version(void) {
    cJSON *document = document_from("{\"plugins\":{\"npm\":\"daukle/npm\"}}");
    fr_plugin_entry *entries = NULL; size_t count = 0; fr_error err;

    ASSERT_EQ(FR_ERR, fr_plugins_parse(document, &entries, &count, &err));
    ASSERT(strstr(err.message, "npm") != NULL);

    cJSON_Delete(document);
    PASS();
}

TEST rejects_a_plugins_member_that_is_not_a_table(void) {
    cJSON *document = document_from("{\"plugins\":[1,2]}");
    fr_plugin_entry *entries = NULL; size_t count = 0; fr_error err;

    ASSERT_EQ(FR_ERR, fr_plugins_parse(document, &entries, &count, &err));
    ASSERT(strstr(err.message, "plugins") != NULL);

    cJSON_Delete(document);
    PASS();
}

TEST rejects_a_coordinate_with_an_empty_half(void) {
    const char *bad[] = { "{\"plugins\":{\"a\":\"@\"}}",
                          "{\"plugins\":{\"a\":\"owner@\"}}",
                          "{\"plugins\":{\"a\":\"@1.0.0\"}}" };
    for (size_t index = 0; index < 3; index++) {
        cJSON *document = document_from(bad[index]);
        fr_plugin_entry *entries = NULL; size_t count = 0; fr_error err;
        ASSERT_EQ(FR_ERR, fr_plugins_parse(document, &entries, &count, &err));
        ASSERT(strstr(err.message, "a") != NULL);
        cJSON_Delete(document);
    }
    PASS();
}

TEST a_local_plugin_registers_its_language(void) {
    fr_error err;
    fr_registry *registry = NULL;
    ASSERT_EQ(FR_OK, fr_build_registry(&registry, &err));

    fr_manifest manifest;
    ASSERT_EQ(FR_OK, fr_config_load_file("test/fixtures/plugin-local/daukle.toml",
                                         registry, &manifest, &err));

    ASSERT(fr_registry_language(registry, "daukle.language/hello") != NULL);

    fr_manifest_free(&manifest);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    PASS();
}

TEST a_manifest_declaring_no_plugins_opens_no_lua_state(void) {
    fr_error err;
    fr_registry *registry = NULL;
    ASSERT_EQ(FR_OK, fr_build_registry(&registry, &err));

    fr_manifest manifest;
    ASSERT_EQ(FR_OK, fr_config_load_file("test/fixtures/consumer/daukle.json",
                                         registry, &manifest, &err));

    ASSERT(fr_lua_runtime_state() == NULL);

    fr_manifest_free(&manifest);
    fr_registry_destroy(registry);
    PASS();
}

static int load_manifest(const char *file_path, fr_error *err) {
    fr_registry *registry = NULL;
    if (fr_build_registry(&registry, err) != FR_OK) return FR_ERR;

    fr_manifest manifest;
    int status = fr_config_load_file(file_path, registry, &manifest, err);
    if (status == FR_OK) fr_manifest_free(&manifest);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    return status;
}

TEST a_verb_uses_does_not_know_is_refused(void) {
    fr_error err;
    ASSERT_EQ(FR_ERR, load_manifest("test/fixtures/plugin-unknown-verb/daukle.toml", &err));
    ASSERT(strstr(err.message, "npm") != NULL);
    ASSERT(strstr(err.message, "teleport") != NULL);
    PASS();
}

TEST a_plugin_written_against_a_later_api_says_which(void) {
    fr_error err;
    ASSERT_EQ(FR_ERR, load_manifest("test/fixtures/plugin-future-api/daukle.toml", &err));
    ASSERT(strstr(err.message, "gradle") != NULL);
    ASSERT(strstr(err.message, "needs daukle api 3, this daukle provides 1") != NULL);
    PASS();
}

TEST a_uses_entry_that_is_not_a_string_is_refused(void) {
    fr_error err;
    ASSERT_EQ(FR_ERR, load_manifest("test/fixtures/plugin-bad-uses/daukle.toml", &err));
    ASSERT(strstr(err.message, "cargo") != NULL);
    PASS();
}

TEST a_verb_used_before_the_declaration_says_so(void) {
    fr_error err;
    ASSERT_EQ(FR_ERR, load_manifest("test/fixtures/plugin-verb-before-declaration/daukle.toml", &err));
    ASSERT(strstr(err.message, "early") != NULL);
    ASSERT(strstr(err.message, "daukle.plugin must be the first call") != NULL);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(parses_the_string_coordinate_form);
    RUN_TEST(parses_the_table_form_with_a_pin);
    RUN_TEST(parses_a_local_path);
    RUN_TEST(an_absent_plugins_table_yields_no_entries);
    RUN_TEST(rejects_a_coordinate_with_no_version);
    RUN_TEST(rejects_a_plugins_member_that_is_not_a_table);
    RUN_TEST(rejects_a_coordinate_with_an_empty_half);
    RUN_TEST(a_local_plugin_registers_its_language);
    RUN_TEST(a_manifest_declaring_no_plugins_opens_no_lua_state);
    RUN_TEST(a_verb_uses_does_not_know_is_refused);
    RUN_TEST(a_plugin_written_against_a_later_api_says_which);
    RUN_TEST(a_uses_entry_that_is_not_a_string_is_refused);
    RUN_TEST(a_verb_used_before_the_declaration_says_so);
    GREATEST_MAIN_END();
}
