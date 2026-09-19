#include "greatest.h"

#include "cJSON.h"
#include "plugins.h"

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

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(parses_the_string_coordinate_form);
    RUN_TEST(parses_the_table_form_with_a_pin);
    RUN_TEST(parses_a_local_path);
    RUN_TEST(an_absent_plugins_table_yields_no_entries);
    RUN_TEST(rejects_a_coordinate_with_no_version);
    GREATEST_MAIN_END();
}
