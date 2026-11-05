#include "greatest.h"
#include "project/region.h"
#include "support.h"
#include "project/sync.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

TEST names_the_manifest_when_resolution_fails(void) {
    fr_sync_report report; fr_error err;
    ASSERT_EQ(FR_ERR, fr_sync("test/fixtures/deps-badrange/daukle.toml", 1, 1, &report, &err));
    ASSERT(strstr(err.message, "deps-badrange/daukle.toml") != NULL);
    fr_sync_report_free(&report);
    PASS();
}

TEST reports_an_unknown_source_kind_as_a_missing_plugin(void) {
    fr_error err;
    fr_sync_report report;
    ASSERT_EQ(FR_ERR, fr_sync("test/fixtures/deps/daukle-unknown-source-dependency.toml", 0, 1, &report, &err));
    ASSERT(strstr(err.message, "forebay/basekit") != NULL);
    ASSERT(strstr(err.message, "no plugin provides source \"some-future-kind\"") != NULL);
    ASSERT(strstr(err.message, "add it to [plugins] in daukle.toml") != NULL);
    fr_sync_report_free(&report);
    PASS();
}

TEST a_repository_with_no_tool_file_generates_into_the_derived_directory(void) {
    fr_test_remove_tree("test/fixtures/managed/build");
    fr_sync_report report; fr_error err;

    int status = fr_sync("test/fixtures/managed/daukle.toml", 1, 1, &report, &err);
    int reported_one = report.count == 1;
    fr_sync_report_free(&report);

    char *text = NULL;
    int wrote_it = fr_file_read_text("test/fixtures/managed/build/daukle/stub/build.txt",
                                     &text, &err) == FR_OK;
    int content_is_right = wrote_it && strcmp(text, "target app\n") == 0;
    free(text);
    fr_test_remove_tree("test/fixtures/managed/build");

    ASSERT_EQ(FR_OK, status);
    ASSERT(reported_one);
    ASSERT(content_is_right);
    PASS();
}

TEST generating_twice_changes_nothing_the_second_time(void) {
    fr_test_remove_tree("test/fixtures/managed/build");
    fr_sync_report first; fr_sync_report second; fr_error err;
    fr_sync("test/fixtures/managed/daukle.toml", 1, 1, &first, &err);
    int first_reported_one = first.count == 1;
    fr_sync_report_free(&first);

    char *text = NULL;
    int first_wrote_it = fr_file_read_text("test/fixtures/managed/build/daukle/stub/build.txt",
                                           &text, &err) == FR_OK;
    free(text);

    int status = fr_sync("test/fixtures/managed/daukle.toml", 1, 1, &second, &err);
    int reported_nothing = second.count == 0;
    fr_sync_report_free(&second);
    fr_test_remove_tree("test/fixtures/managed/build");

    ASSERT_EQ(FR_OK, status);
    ASSERT(first_reported_one);
    ASSERT(first_wrote_it);
    ASSERT(reported_nothing);
    PASS();
}

TEST a_session_survives_a_sync_and_closes_clean(void) {
    fr_error err;
    fr_session session;
    ASSERT_EQ(FR_OK, fr_session_open("test/fixtures/two-ways/toml/daukle.toml", 1, &session, &err));
    ASSERT(session.registry != NULL);
    ASSERT_STR_EQ("forebay/two-ways", session.manifest.self.project);

    fr_sync_report report;
    ASSERT_EQ(FR_OK, fr_sync_session(&session, 0, &report, &err));
    /* the runtime is still open here, which is exactly what a task run needs after a sync. */
    fr_sync_report_free(&report);
    ASSERT_EQ(FR_OK, fr_sync_session(&session, 0, &report, &err));
    fr_sync_report_free(&report);
    fr_session_close(&session);
    PASS();
}

TEST check_names_a_stale_derived_tree_without_writing(void) {
    fr_test_remove_tree("test/fixtures/managed/build");
    fr_sync_report report; fr_error err;

    int status = fr_sync("test/fixtures/managed/daukle.toml", 0, 1, &report, &err);
    int reported_one = report.count == 1;
    fr_sync_report_free(&report);

    char *text = NULL;
    int wrote_nothing = fr_file_read_text("test/fixtures/managed/build/daukle/stub/build.txt",
                                          &text, &err) != FR_OK;
    free(text);
    fr_test_remove_tree("test/fixtures/managed/build");

    ASSERT_EQ(FR_OK, status);
    ASSERT(reported_one);
    ASSERT(wrote_nothing);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(names_the_manifest_when_resolution_fails);
    RUN_TEST(reports_an_unknown_source_kind_as_a_missing_plugin);
    RUN_TEST(a_repository_with_no_tool_file_generates_into_the_derived_directory);
    RUN_TEST(generating_twice_changes_nothing_the_second_time);
    RUN_TEST(check_names_a_stale_derived_tree_without_writing);
    RUN_TEST(a_session_survives_a_sync_and_closes_clean);
    GREATEST_MAIN_END();
}
