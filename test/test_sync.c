#include "greatest.h"
#include "region.h"
#include "sync.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TEMPLATE =
    "dependencies {\n"
    "    // ferrule:begin\n"
    "    // ferrule:end\n"
    "    testImplementation \"junit\"\n"
    "}\n";

static void reset_build_file(void) {
    fr_error err;
    fr_file_write_text("test/fixtures/consumer/build.gradle", TEMPLATE, &err);
}

TEST sync_writes_the_resolved_lines(void) {
    reset_build_file();
    fr_sync_report report; fr_error err;
    ASSERT_EQ(FR_OK, fr_sync("test/fixtures/consumer/ferrule.json", 1, 1, &report, &err));
    ASSERT_EQ(1, (int) report.count);
    fr_sync_report_free(&report);

    char *text = NULL;
    fr_file_read_text("test/fixtures/consumer/build.gradle", &text, &err);
    ASSERT(strstr(text, "    githubImplementation \"forebay:basekit:5.0.0:contracts\"\n") != NULL);
    ASSERT(strstr(text, "    githubImplementation \"forebay:basekit:5.0.0:ir\"\n") != NULL);
    ASSERT(strstr(text, "testImplementation \"junit\"") != NULL);
    free(text);
    reset_build_file();
    PASS();
}

TEST sync_is_idempotent(void) {
    reset_build_file();
    fr_sync_report report; fr_error err;
    fr_sync("test/fixtures/consumer/ferrule.json", 1, 1, &report, &err);
    fr_sync_report_free(&report);
    ASSERT_EQ(FR_OK, fr_sync("test/fixtures/consumer/ferrule.json", 1, 1, &report, &err));
    ASSERT_EQ(0, (int) report.count);
    fr_sync_report_free(&report);
    reset_build_file();
    PASS();
}

TEST check_names_the_drifted_file_without_writing(void) {
    reset_build_file();
    fr_sync_report report; fr_error err;
    ASSERT_EQ(FR_OK, fr_sync("test/fixtures/consumer/ferrule.json", 0, 1, &report, &err));
    ASSERT_EQ(1, (int) report.count);
    ASSERT(strstr(report.files[0], "consumer/build.gradle") != NULL);
    fr_sync_report_free(&report);

    char *text = NULL;
    fr_file_read_text("test/fixtures/consumer/build.gradle", &text, &err);
    ASSERT_STR_EQ(TEMPLATE, text);
    free(text);
    reset_build_file();
    PASS();
}

TEST check_is_quiet_when_in_sync(void) {
    reset_build_file();
    fr_sync_report report; fr_error err;
    fr_sync("test/fixtures/consumer/ferrule.json", 1, 1, &report, &err);
    fr_sync_report_free(&report);
    ASSERT_EQ(FR_OK, fr_sync("test/fixtures/consumer/ferrule.json", 0, 1, &report, &err));
    ASSERT_EQ(0, (int) report.count);
    fr_sync_report_free(&report);
    reset_build_file();
    PASS();
}

TEST reports_a_build_file_without_markers(void) {
    fr_error err;
    fr_file_write_text("test/fixtures/consumer/build.gradle", "dependencies {\n}\n", &err);
    fr_sync_report report;
    ASSERT_EQ(FR_ERR, fr_sync("test/fixtures/consumer/ferrule.json", 1, 1, &report, &err));
    ASSERT(strstr(err.message, "build.gradle") != NULL);
    ASSERT(strstr(err.message, "ferrule:begin") != NULL);
    fr_sync_report_free(&report);
    reset_build_file();
    PASS();
}

TEST names_the_manifest_when_resolution_fails(void) {
    fr_sync_report report; fr_error err;
    ASSERT_EQ(FR_ERR, fr_sync("test/fixtures/consumer-badrange/ferrule.json", 1, 1, &report, &err));
    ASSERT(strstr(err.message, "consumer-badrange/ferrule.json") != NULL);
    ASSERT(strstr(err.message, "build.gradle") == NULL);
    fr_sync_report_free(&report);
    PASS();
}

TEST counts_the_files_written_before_a_failure(void) {
    fr_error err;
    fr_file_write_text("test/fixtures/consumer-partial/a.gradle", TEMPLATE, &err);
    fr_sync_report report;
    ASSERT_EQ(FR_ERR, fr_sync("test/fixtures/consumer-partial/ferrule.json", 1, 1, &report, &err));
    ASSERT_EQ(1, (int) report.count);
    ASSERT(strstr(report.files[0], "a.gradle") != NULL);
    ASSERT(strstr(err.message, "b.gradle") != NULL);
    fr_sync_report_free(&report);

    char *text = NULL;
    fr_file_read_text("test/fixtures/consumer-partial/a.gradle", &text, &err);
    ASSERT(strstr(text, "githubImplementation \"forebay:basekit:5.0.0:ir\"") != NULL);
    free(text);
    PASS();
}

TEST reports_an_unknown_source_kind_as_a_missing_plugin(void) {
    fr_error err;
    fr_sync_report report;
    ASSERT_EQ(FR_ERR, fr_sync("test/fixtures/consumer/ferrule-unknown-source-consumer.json", 0, 1, &report, &err));
    ASSERT(strstr(err.message, "ferrule.source/some-future-kind") != NULL);
    fr_sync_report_free(&report);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(sync_writes_the_resolved_lines);
    RUN_TEST(sync_is_idempotent);
    RUN_TEST(check_names_the_drifted_file_without_writing);
    RUN_TEST(check_is_quiet_when_in_sync);
    RUN_TEST(reports_a_build_file_without_markers);
    RUN_TEST(names_the_manifest_when_resolution_fails);
    RUN_TEST(counts_the_files_written_before_a_failure);
    RUN_TEST(reports_an_unknown_source_kind_as_a_missing_plugin);
    GREATEST_MAIN_END();
}
