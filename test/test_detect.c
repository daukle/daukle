#include "greatest.h"
#include "migrate/detect.h"
#include "support.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *CATALOG =
    "{\"plugins\":["
    " {\"id\":\"build-a\",\"asset\":null,\"detect\":[{\"files\":[\"a.build\"],\"scope\":\"tree\"}]},"
    " {\"id\":\"pkg-plain\",\"asset\":null,\"detect\":[{\"files\":[\"pkg.json\"],\"scope\":\"tree\"}]},"
    " {\"id\":\"pkg-scripts\",\"asset\":null,\"detect\":[{\"files\":[\"pkg.json\"],"
    "   \"contains\":[\"\\\"scripts\\\"%s*:\"],\"scope\":\"tree\"}]},"
    " {\"id\":\"rooted\",\"asset\":null,\"detect\":[{\"files\":[\"*.root\"]}]}"
    "]}";

static const char *TWO_PLAIN =
    "{\"plugins\":["
    " {\"id\":\"first\",\"asset\":null,\"detect\":[{\"files\":[\"pkg.json\"],\"scope\":\"tree\"}]},"
    " {\"id\":\"second\",\"asset\":null,\"detect\":[{\"files\":[\"pkg.json\"],\"scope\":\"tree\"}]}"
    "]}";

static const char *BAD_PATTERN =
    "{\"plugins\":["
    " {\"id\":\"broken\",\"asset\":null,\"detect\":[{\"files\":[\"pkg.json\"],"
    "   \"contains\":[\"[unclosed\"],\"scope\":\"tree\"}]}"
    "]}";

static const char *SKIP_DEPS =
    "{\"plugins\":["
    " {\"id\":\"pkg-scripts\",\"asset\":null,\"skip\":[\"deps\"],"
    "  \"detect\":[{\"files\":[\"pkg.json\"],\"contains\":[\"\\\"scripts\\\"%s*:\"],\"scope\":\"tree\"}]}"
    "]}";

static const char *NO_SKIP =
    "{\"plugins\":["
    " {\"id\":\"pkg-scripts\",\"asset\":null,"
    "  \"detect\":[{\"files\":[\"pkg.json\"],\"contains\":[\"\\\"scripts\\\"%s*:\"],\"scope\":\"tree\"}]}"
    "]}";

static const char *SKIP_ONLY_PLUGIN =
    "{\"plugins\":["
    " {\"id\":\"hider\",\"asset\":null,\"skip\":[\"deps\"]}"
    "]}";

static char scratch[1024];

static void begin_tree(const char *name) {
    snprintf(scratch, sizeof scratch, "%s/detect_%d_%s", fr_test_temp_base(), fr_test_process_id(),
             name);
    fr_test_remove_tree(scratch);
    fr_test_make_directory(scratch);
}

static void make_parents(const char *relative_path) {
    char path[1024];
    for (size_t index = 0; relative_path[index] != '\0'; index++) {
        if (relative_path[index] != '/') continue;
        snprintf(path, sizeof path, "%s/%.*s", scratch, (int) index, relative_path);
        fr_test_make_directory(path);
    }
}

static void write_file(const char *relative_path, const char *text) {
    make_parents(relative_path);
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", scratch, relative_path);
    FILE *file = fopen(path, "wb");
    if (file == NULL) return;
    fputs(text, file);
    fclose(file);
}

static void write_skip_tree(void) {
    write_file("pkg.json", "{}");
    write_file("deps/a/pkg.json", "{\"scripts\": {}}");
    write_file("deps/b/pkg.json", "{\"scripts\": {}}");
}

static enum greatest_test_res parse_catalog(const char *text, fr_catalog *catalog) {
    memset(catalog, 0, sizeof *catalog);
    fr_error err;
    int rc = fr_catalog_parse(text, "test", 0, catalog, &err);
    if (rc != FR_OK) fprintf(stderr, "test catalog did not parse: %s\n", err.message);
    ASSERT_EQ(FR_OK, rc);
    return GREATEST_TEST_RES_PASS;
}

TEST nested_matches_collapse_to_the_outermost(void) {
    begin_tree("collapse");
    write_file("a.build", "");
    write_file("sub/a.build", "");
    write_file("sub/deeper/a.build", "");
    fr_catalog catalog;
    CHECK_CALL(parse_catalog(CATALOG, &catalog));
    fr_detections found;
    fr_error err;
    ASSERT_EQ(FR_OK, fr_detect(scratch, &catalog, NULL, 0, &found, &err));
    ASSERT_EQ(1, found.count);
    ASSERT_STR_EQ("build-a", found.items[0].plugin->id);
    ASSERT_STR_EQ("", found.items[0].dir);
    fr_detections_free(&found);
    fr_catalog_free(&catalog);
    fr_test_remove_tree(scratch);
    PASS();
}

TEST a_contains_match_beats_a_name_only_match_on_the_same_file(void) {
    begin_tree("specific");
    write_file("web/pkg.json", "{\"scripts\": {}}");
    fr_catalog catalog;
    CHECK_CALL(parse_catalog(CATALOG, &catalog));
    fr_detections found;
    fr_error err;
    ASSERT_EQ(FR_OK, fr_detect(scratch, &catalog, NULL, 0, &found, &err));
    ASSERT_EQ(1, found.count);
    ASSERT_STR_EQ("pkg-scripts", found.items[0].plugin->id);
    ASSERT_STR_EQ("web", found.items[0].dir);
    fr_detections_free(&found);
    fr_catalog_free(&catalog);
    fr_test_remove_tree(scratch);
    PASS();
}

TEST a_file_without_the_contains_text_falls_back_to_the_name_only_plugin(void) {
    begin_tree("fallback");
    write_file("pkg.json", "{}");
    fr_catalog catalog;
    CHECK_CALL(parse_catalog(CATALOG, &catalog));
    fr_detections found;
    fr_error err;
    ASSERT_EQ(FR_OK, fr_detect(scratch, &catalog, NULL, 0, &found, &err));
    ASSERT_EQ(1, found.count);
    ASSERT_STR_EQ("pkg-plain", found.items[0].plugin->id);
    fr_detections_free(&found);
    fr_catalog_free(&catalog);
    fr_test_remove_tree(scratch);
    PASS();
}

TEST a_name_only_tie_in_one_catalog_is_refused_naming_both(void) {
    begin_tree("tie");
    write_file("pkg.json", "{}");
    fr_catalog catalog;
    CHECK_CALL(parse_catalog(TWO_PLAIN, &catalog));
    fr_detections found;
    fr_error err;
    ASSERT_EQ(FR_ERR, fr_detect(scratch, &catalog, NULL, 0, &found, &err));
    ASSERT(strstr(err.message, "first") != NULL);
    ASSERT(strstr(err.message, "second") != NULL);
    ASSERT(strstr(err.message, "pkg.json") != NULL);
    ASSERT_EQ(0, found.count);
    fr_catalog_free(&catalog);
    fr_test_remove_tree(scratch);
    PASS();
}

TEST with_breaks_a_tie_and_adds_a_plugin(void) {
    begin_tree("with");
    write_file("pkg.json", "{}");
    fr_catalog catalog;
    CHECK_CALL(parse_catalog(TWO_PLAIN, &catalog));
    const char *with[] = { "second" };
    fr_detections found;
    fr_error err;
    ASSERT_EQ(FR_OK, fr_detect(scratch, &catalog, with, 1, &found, &err));
    ASSERT_EQ(1, found.count);
    ASSERT_STR_EQ("second", found.items[0].plugin->id);
    ASSERT_STR_EQ("", found.items[0].dir);
    fr_detections_free(&found);
    fr_catalog_free(&catalog);
    fr_test_remove_tree(scratch);
    PASS();
}

TEST with_wins_a_file_another_plugin_outranked_it_on(void) {
    begin_tree("withforces");
    write_file("pkg.json", "{\"scripts\": {}}");
    fr_catalog catalog;
    CHECK_CALL(parse_catalog(CATALOG, &catalog));
    const char *with[] = { "pkg-plain" };
    fr_detections found;
    static fr_error err;
    ASSERT_EQm(err.message, FR_OK, fr_detect(scratch, &catalog, with, 1, &found, &err));
    ASSERT_EQ(1, found.count);
    ASSERT_STR_EQ("pkg-plain", found.items[0].plugin->id);
    ASSERT_STR_EQ("", found.items[0].dir);
    fr_detections_free(&found);
    fr_catalog_free(&catalog);
    fr_test_remove_tree(scratch);
    PASS();
}

TEST with_adds_a_plugin_that_matched_nothing(void) {
    begin_tree("withadd");
    fr_catalog catalog;
    CHECK_CALL(parse_catalog(CATALOG, &catalog));
    const char *with[] = { "build-a" };
    fr_detections found;
    fr_error err;
    ASSERT_EQ(FR_OK, fr_detect(scratch, &catalog, with, 1, &found, &err));
    ASSERT_EQ(1, found.count);
    ASSERT_STR_EQ("build-a", found.items[0].plugin->id);
    ASSERT_STR_EQ("", found.items[0].dir);
    fr_detections_free(&found);
    fr_catalog_free(&catalog);
    fr_test_remove_tree(scratch);
    PASS();
}

TEST with_naming_no_catalog_id_is_an_error_naming_it(void) {
    begin_tree("withunknown");
    fr_catalog catalog;
    CHECK_CALL(parse_catalog(CATALOG, &catalog));
    const char *with[] = { "ghost" };
    fr_detections found;
    fr_error err;
    ASSERT_EQ(FR_ERR, fr_detect(scratch, &catalog, with, 1, &found, &err));
    ASSERT(strstr(err.message, "ghost") != NULL);
    fr_catalog_free(&catalog);
    fr_test_remove_tree(scratch);
    PASS();
}

TEST a_root_scoped_rule_ignores_subdirectories(void) {
    begin_tree("rootscope");
    write_file("x/y.root", "");
    fr_catalog catalog;
    CHECK_CALL(parse_catalog(CATALOG, &catalog));
    fr_detections found;
    fr_error err;
    ASSERT_EQ(FR_OK, fr_detect(scratch, &catalog, NULL, 0, &found, &err));
    ASSERT_EQ(0, found.count);
    fr_detections_free(&found);

    write_file("y.root", "");
    ASSERT_EQ(FR_OK, fr_detect(scratch, &catalog, NULL, 0, &found, &err));
    ASSERT_EQ(1, found.count);
    ASSERT_STR_EQ("rooted", found.items[0].plugin->id);
    ASSERT_STR_EQ("", found.items[0].dir);
    fr_detections_free(&found);
    fr_catalog_free(&catalog);
    fr_test_remove_tree(scratch);
    PASS();
}

TEST dot_directories_and_the_derived_root_are_not_walked(void) {
    begin_tree("skipped");
    write_file(".git/a.build", "");
    write_file("build/daukle/a.build", "");
    fr_catalog catalog;
    CHECK_CALL(parse_catalog(CATALOG, &catalog));
    fr_detections found;
    fr_error err;
    ASSERT_EQ(FR_OK, fr_detect(scratch, &catalog, NULL, 0, &found, &err));
    ASSERT_EQ(0, found.count);
    fr_detections_free(&found);
    fr_catalog_free(&catalog);
    fr_test_remove_tree(scratch);
    PASS();
}

TEST two_separate_directories_are_two_detections(void) {
    begin_tree("separate");
    write_file("two/a.build", "");
    write_file("one/a.build", "");
    fr_catalog catalog;
    CHECK_CALL(parse_catalog(CATALOG, &catalog));
    fr_detections found;
    fr_error err;
    ASSERT_EQ(FR_OK, fr_detect(scratch, &catalog, NULL, 0, &found, &err));
    ASSERT_EQ(2, found.count);
    ASSERT_STR_EQ("build-a", found.items[0].plugin->id);
    ASSERT_STR_EQ("one", found.items[0].dir);
    ASSERT_STR_EQ("build-a", found.items[1].plugin->id);
    ASSERT_STR_EQ("two", found.items[1].dir);
    fr_detections_free(&found);
    fr_catalog_free(&catalog);
    fr_test_remove_tree(scratch);
    PASS();
}

TEST a_directory_deeper_than_the_limit_is_not_read(void) {
    begin_tree("depth");
    char relative[256] = "";
    for (int level = 0; level < FR_DETECT_MAX_DEPTH + 1; level++) strcat(relative, "d/");
    strcat(relative, "a.build");
    write_file(relative, "");
    fr_catalog catalog;
    CHECK_CALL(parse_catalog(CATALOG, &catalog));
    fr_detections found;
    fr_error err;
    ASSERT_EQ(FR_OK, fr_detect(scratch, &catalog, NULL, 0, &found, &err));
    ASSERT_EQ(0, found.count);
    fr_detections_free(&found);
    fr_catalog_free(&catalog);
    fr_test_remove_tree(scratch);
    PASS();
}

TEST name_matching_handles_star_and_question(void) {
    ASSERT(fr_detect_name_matches("*.gradle", "build.gradle"));
    ASSERT(fr_detect_name_matches("build.gradle?kts", "build.gradle.kts"));
    ASSERT(!fr_detect_name_matches("*.gradle", "build.gradle.kts"));
    ASSERT(fr_detect_name_matches("*", "anything"));
    ASSERT(fr_detect_name_matches("a*b*c", "aXXbYYc"));
    ASSERT(!fr_detect_name_matches("a*b*c", "aXXbYY"));
    PASS();
}

TEST a_malformed_pattern_is_an_error_naming_plugin_and_pattern(void) {
    begin_tree("malformed");
    write_file("pkg.json", "{}");
    fr_catalog catalog;
    CHECK_CALL(parse_catalog(BAD_PATTERN, &catalog));
    fr_detections found;
    fr_error err;
    ASSERT_EQ(FR_ERR, fr_detect(scratch, &catalog, NULL, 0, &found, &err));
    ASSERT(strstr(err.message, "broken") != NULL);
    ASSERT(strstr(err.message, "[unclosed") != NULL);
    fr_catalog_free(&catalog);
    fr_test_remove_tree(scratch);
    PASS();
}

static const char *PATH_ENTRY =
    "{\"plugins\":["
    " {\"id\":\"nested\",\"asset\":null,\"detect\":[{\"files\":[\"gradle/wrapper.cfg\"],\"scope\":\"tree\"}]}"
    "]}";

TEST with_keeps_the_subdirectory_where_it_broke_a_tie(void) {
    begin_tree("withsub");
    write_file("web/pkg.json", "{}");
    fr_catalog catalog;
    CHECK_CALL(parse_catalog(TWO_PLAIN, &catalog));
    const char *with[] = { "second" };
    fr_detections found;
    fr_error err;
    ASSERT_EQ(FR_OK, fr_detect(scratch, &catalog, with, 1, &found, &err));
    ASSERT_EQ(2, found.count);
    ASSERT_STR_EQ("second", found.items[0].plugin->id);
    ASSERT_STR_EQ("", found.items[0].dir);
    ASSERT_STR_EQ("second", found.items[1].plugin->id);
    ASSERT_STR_EQ("web", found.items[1].dir);
    fr_detections_free(&found);
    fr_catalog_free(&catalog);
    fr_test_remove_tree(scratch);
    PASS();
}

TEST a_root_that_is_not_a_directory_is_an_error_naming_it(void) {
    begin_tree("noroot");
    fr_catalog catalog;
    CHECK_CALL(parse_catalog(CATALOG, &catalog));
    char missing[1100];
    snprintf(missing, sizeof missing, "%s/absent", scratch);
    fr_detections found;
    fr_error err;
    ASSERT_EQ(FR_ERR, fr_detect(missing, &catalog, NULL, 0, &found, &err));
    ASSERT(strstr(err.message, "absent") != NULL);
    ASSERT_EQ(0, found.count);
    fr_catalog_free(&catalog);
    fr_test_remove_tree(scratch);
    PASS();
}

TEST an_entry_holding_a_slash_names_a_file_below_the_directory(void) {
    begin_tree("pathentry");
    write_file("app/gradle/wrapper.cfg", "");
    write_file("other/gradle/wrapper.cfg/inner.txt", "");
    fr_catalog catalog;
    CHECK_CALL(parse_catalog(PATH_ENTRY, &catalog));
    fr_detections found;
    fr_error err;
    ASSERT_EQ(FR_OK, fr_detect(scratch, &catalog, NULL, 0, &found, &err));
    ASSERT_EQ(1, found.count);
    ASSERT_STR_EQ("nested", found.items[0].plugin->id);
    ASSERT_STR_EQ("app", found.items[0].dir);
    fr_detections_free(&found);
    fr_catalog_free(&catalog);
    fr_test_remove_tree(scratch);
    PASS();
}

TEST a_skipped_directory_name_is_not_walked_at_any_depth(void) {
    begin_tree("skip");
    write_skip_tree();
    write_file("app/deps/c/pkg.json", "{\"scripts\": {}}");
    fr_catalog catalog;
    CHECK_CALL(parse_catalog(SKIP_DEPS, &catalog));
    fr_detections found;
    fr_error err;
    ASSERT_EQ(FR_OK, fr_detect(scratch, &catalog, NULL, 0, &found, &err));
    ASSERT_EQ(0, found.count);
    fr_detections_free(&found);
    fr_catalog_free(&catalog);
    fr_test_remove_tree(scratch);
    PASS();
}

TEST without_skip_the_same_tree_reports_the_dependencies(void) {
    begin_tree("noskip");
    write_skip_tree();
    fr_catalog catalog;
    CHECK_CALL(parse_catalog(NO_SKIP, &catalog));
    fr_detections found;
    fr_error err;
    ASSERT_EQ(FR_OK, fr_detect(scratch, &catalog, NULL, 0, &found, &err));
    ASSERT_EQ(2, found.count);
    ASSERT_STR_EQ("deps/a", found.items[0].dir);
    ASSERT_STR_EQ("deps/b", found.items[1].dir);
    fr_detections_free(&found);
    fr_catalog_free(&catalog);
    fr_test_remove_tree(scratch);
    PASS();
}

TEST a_skip_from_another_plugin_hides_the_directory_from_every_plugin(void) {
    begin_tree("skipunion");
    write_skip_tree();
    fr_catalog catalog;
    CHECK_CALL(parse_catalog(NO_SKIP, &catalog));
    fr_catalog hider;
    CHECK_CALL(parse_catalog(SKIP_ONLY_PLUGIN, &hider));
    fr_catalog_plugin *grown = realloc(catalog.plugins, (catalog.count + 1) * sizeof *grown);
    ASSERT(grown != NULL);
    catalog.plugins = grown;
    catalog.plugins[catalog.count++] = hider.plugins[0];
    free(hider.plugins);
    fr_detections found;
    fr_error err;
    ASSERT_EQ(FR_OK, fr_detect(scratch, &catalog, NULL, 0, &found, &err));
    ASSERT_EQ(0, found.count);
    fr_detections_free(&found);
    fr_catalog_free(&catalog);
    fr_test_remove_tree(scratch);
    PASS();
}

TEST a_file_named_like_a_skip_entry_is_still_matched(void) {
    begin_tree("skipfile");
    write_file("web/pkg.json", "{\"scripts\": {}}");
    write_file("web/deps", "not a directory");
    fr_catalog catalog;
    CHECK_CALL(parse_catalog(SKIP_DEPS, &catalog));
    fr_detections found;
    fr_error err;
    ASSERT_EQ(FR_OK, fr_detect(scratch, &catalog, NULL, 0, &found, &err));
    ASSERT_EQ(1, found.count);
    ASSERT_STR_EQ("web", found.items[0].dir);
    fr_detections_free(&found);
    fr_catalog_free(&catalog);
    fr_test_remove_tree(scratch);
    PASS();
}

SUITE(detect) {
    RUN_TEST(nested_matches_collapse_to_the_outermost);
    RUN_TEST(a_contains_match_beats_a_name_only_match_on_the_same_file);
    RUN_TEST(a_file_without_the_contains_text_falls_back_to_the_name_only_plugin);
    RUN_TEST(a_name_only_tie_in_one_catalog_is_refused_naming_both);
    RUN_TEST(with_breaks_a_tie_and_adds_a_plugin);
    RUN_TEST(with_wins_a_file_another_plugin_outranked_it_on);
    RUN_TEST(with_adds_a_plugin_that_matched_nothing);
    RUN_TEST(with_naming_no_catalog_id_is_an_error_naming_it);
    RUN_TEST(a_root_scoped_rule_ignores_subdirectories);
    RUN_TEST(dot_directories_and_the_derived_root_are_not_walked);
    RUN_TEST(two_separate_directories_are_two_detections);
    RUN_TEST(a_directory_deeper_than_the_limit_is_not_read);
    RUN_TEST(with_keeps_the_subdirectory_where_it_broke_a_tie);
    RUN_TEST(a_root_that_is_not_a_directory_is_an_error_naming_it);
    RUN_TEST(an_entry_holding_a_slash_names_a_file_below_the_directory);
    RUN_TEST(a_skipped_directory_name_is_not_walked_at_any_depth);
    RUN_TEST(without_skip_the_same_tree_reports_the_dependencies);
    RUN_TEST(a_skip_from_another_plugin_hides_the_directory_from_every_plugin);
    RUN_TEST(a_file_named_like_a_skip_entry_is_still_matched);
    RUN_TEST(name_matching_handles_star_and_question);
    RUN_TEST(a_malformed_pattern_is_an_error_naming_plugin_and_pattern);
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(detect);
    GREATEST_MAIN_END();
}
