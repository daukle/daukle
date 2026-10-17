#include "greatest.h"
#include "migrate/catalog.h"

#include "cJSON.h"

#include <stdlib.h>
#include <string.h>

static const char *GOOD =
    "{\"plugins\":["
    " {\"id\":\"gradle\",\"asset\":{\"url\":\"https://example.test/g.lua\",\"sha256\":\"ab\"},"
    "  \"detect\":[{\"files\":[\"build.gradle\",\"settings.gradle.kts\"],\"scope\":\"tree\"}],"
    "  \"replaces\":{}},"
    " {\"id\":\"node\",\"asset\":null,"
    "  \"detect\":[{\"files\":[\"package.json\"],\"contains\":[\"\\\"scripts\\\"%s*:\"]}],"
    "  \"replaces\":{\"npm\":{\"packages\":[\"release-it\"]}}}"
    "]}";

TEST a_catalog_reads_ids_assets_rules_and_replaces(void) {
    fr_catalog catalog;
    memset(&catalog, 0, sizeof catalog);
    fr_error err;
    ASSERT_EQ(FR_OK, fr_catalog_parse(GOOD, "test", 0, &catalog, &err));
    ASSERT_EQ(2, catalog.count);
    const fr_catalog_plugin *gradle = fr_catalog_find(&catalog, "gradle");
    ASSERT_STR_EQ("https://example.test/g.lua", gradle->asset_url);
    ASSERT_EQ(FR_DETECT_TREE, gradle->rules[0].scope);
    ASSERT_EQ(2, gradle->rules[0].file_count);
    const fr_catalog_plugin *node = fr_catalog_find(&catalog, "node");
    ASSERT(node->asset_url == NULL);
    ASSERT_EQ(FR_DETECT_ROOT, node->rules[0].scope);
    ASSERT_STR_EQ("\"scripts\"%s*:", node->rules[0].contains[0]);
    fr_catalog_free(&catalog);
    PASS();
}

TEST a_bare_hyphen_is_found_outside_a_set_and_not_inside_one(void) {
    ASSERT(fr_catalog_pattern_has_bare_hyphen("^daukle-task"));
    ASSERT(!fr_catalog_pattern_has_bare_hyphen("^daukle%-task"));
    ASSERT(!fr_catalog_pattern_has_bare_hyphen("[%w%-]+"));
    ASSERT(!fr_catalog_pattern_has_bare_hyphen("[a-z]+"));
    ASSERT(!fr_catalog_pattern_has_bare_hyphen("[^-]"));
    ASSERT(fr_catalog_pattern_has_bare_hyphen("[]]-"));
    PASS();
}

TEST an_entry_with_a_bare_hyphen_is_refused_and_named(void) {
    fr_catalog catalog;
    memset(&catalog, 0, sizeof catalog);
    fr_error err;
    ASSERT_EQ(FR_OK, fr_catalog_parse(
        "{\"plugins\":[{\"id\":\"bad\",\"asset\":null,"
        "\"detect\":[{\"files\":[\"x\"],\"contains\":[\"a-b\"]}]}]}", "cat", 0, &catalog, &err));
    ASSERT_EQ(0, catalog.count);
    ASSERT_EQ(1, catalog.warning_count);
    ASSERT(strstr(catalog.warnings[0], "bad") != NULL);
    ASSERT(strstr(catalog.warnings[0], "a-b") != NULL);
    fr_catalog_free(&catalog);
    PASS();
}

TEST a_wildcard_inside_a_path_is_refused(void) {
    fr_catalog catalog;
    memset(&catalog, 0, sizeof catalog);
    fr_error err;
    ASSERT_EQ(FR_OK, fr_catalog_parse(
        "{\"plugins\":[{\"id\":\"w\",\"asset\":null,"
        "\"detect\":[{\"files\":[\"gradle/*.properties\"]}]}]}", "cat", 0, &catalog, &err));
    ASSERT_EQ(0, catalog.count);
    ASSERT_EQ(1, catalog.warning_count);
    fr_catalog_free(&catalog);
    PASS();
}

TEST an_id_that_cannot_be_a_label_is_refused(void) {
    fr_catalog catalog;
    memset(&catalog, 0, sizeof catalog);
    fr_error err;
    ASSERT_EQ(FR_OK, fr_catalog_parse(
        "{\"plugins\":[{\"id\":\"Has Space\",\"asset\":null}]}", "cat", 0, &catalog, &err));
    ASSERT_EQ(0, catalog.count);
    fr_catalog_free(&catalog);
    PASS();
}

TEST a_document_with_no_plugins_array_is_an_error(void) {
    fr_catalog catalog;
    memset(&catalog, 0, sizeof catalog);
    fr_error err;
    ASSERT_EQ(FR_ERR, fr_catalog_parse("{\"nope\":1}", "where", 0, &catalog, &err));
    ASSERT(strstr(err.message, "where") != NULL);
    fr_catalog_free(&catalog);
    PASS();
}

TEST locations_take_the_environment_then_the_flags(void) {
    const char *flags[] = { "c3" };
    const char **locations = NULL;
    size_t count = 0;
    fr_error err;
    ASSERT_EQ(FR_OK, fr_catalog_locations("  c1\tc2 ", flags, 1, &locations, &count, &err));
    ASSERT_EQ(3, count);
    ASSERT_STR_EQ("c3", locations[2]);
    free((void *) locations);
    PASS();
}

TEST the_first_catalog_wins_an_id_and_the_second_is_a_warning(void) {
    fr_catalog catalog;
    memset(&catalog, 0, sizeof catalog);
    fr_error err;
    ASSERT_EQ(FR_OK, fr_catalog_parse(GOOD, "one", 0, &catalog, &err));
    ASSERT_EQ(FR_OK, fr_catalog_parse(GOOD, "two", 1, &catalog, &err));
    ASSERT_EQ(2, catalog.count);
    ASSERT_EQ(0, fr_catalog_find(&catalog, "gradle")->catalog_index);
    ASSERT_EQ(2, catalog.warning_count);
    fr_catalog_free(&catalog);
    PASS();
}

TEST locations_with_no_environment_are_just_the_flags(void) {
    const char *flags[] = { "c1" };
    const char **locations = NULL;
    size_t count = 0;
    fr_error err;
    ASSERT_EQ(FR_OK, fr_catalog_locations(NULL, flags, 1, &locations, &count, &err));
    ASSERT_EQ(1, count);
    ASSERT_STR_EQ("c1", locations[0]);
    free((void *) locations);
    PASS();
}

TEST an_unreadable_location_is_an_error_naming_it(void) {
    const char *locations[] = { "no-such-dir/no-such-catalog.json" };
    fr_catalog catalog;
    fr_error err;
    ASSERT_EQ(FR_ERR, fr_catalog_load(locations, 1, &catalog, &err));
    ASSERT(strstr(err.message, "no-such-catalog.json") != NULL);
    ASSERT_EQ(0, catalog.count);
    PASS();
}

SUITE(catalog) {
    RUN_TEST(a_catalog_reads_ids_assets_rules_and_replaces);
    RUN_TEST(a_bare_hyphen_is_found_outside_a_set_and_not_inside_one);
    RUN_TEST(an_entry_with_a_bare_hyphen_is_refused_and_named);
    RUN_TEST(a_wildcard_inside_a_path_is_refused);
    RUN_TEST(an_id_that_cannot_be_a_label_is_refused);
    RUN_TEST(a_document_with_no_plugins_array_is_an_error);
    RUN_TEST(locations_take_the_environment_then_the_flags);
    RUN_TEST(the_first_catalog_wins_an_id_and_the_second_is_a_warning);
    RUN_TEST(locations_with_no_environment_are_just_the_flags);
    RUN_TEST(an_unreadable_location_is_an_error_naming_it);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(catalog);
    GREATEST_MAIN_END();
}
