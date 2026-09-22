#include "greatest.h"

#include "derived.h"
#include "region.h"
#include "support.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char scratch[512];

static void make_scratch(const char *label) {
    snprintf(scratch, sizeof scratch, "%s/daukle_test_derived_%s_%d", fr_test_temp_base(), label,
             fr_test_process_id());
    fr_test_remove_tree(scratch);
    fr_test_make_directory(scratch);
}

static char *read_file(const char *path) {
    char *text = NULL;
    fr_error err;
    if (fr_file_read_text(path, &text, &err) != FR_OK) return NULL;
    return text;
}

TEST a_generated_file_is_written_into_the_derived_directory(void) {
    make_scratch("write");
    fr_generated_file files[1] = { { "CMakeLists.txt", "project(app)\n" } };
    fr_derived_report report; fr_error err;

    int status = fr_derived_apply(scratch, files, 1, 1, &report, &err);
    char written[700];
    snprintf(written, sizeof written, "%s/CMakeLists.txt", scratch);
    char *text = read_file(written);
    int content_is_right = text != NULL && strcmp(text, "project(app)\n") == 0;
    int reported_once = report.count == 1;
    free(text);
    fr_derived_report_free(&report);
    fr_test_remove_tree(scratch);

    ASSERT_EQ(FR_OK, status);
    ASSERT(content_is_right);
    ASSERT(reported_once);
    PASS();
}

/* Asserted on mtime, not on content: an implementation that rewrites an
   identical file every run would pass a content check, and rewriting is exactly
   what makes CMake re-run configure on every build. */
TEST an_unchanged_file_is_not_rewritten(void) {
    make_scratch("idempotent");
    fr_generated_file files[1] = { { "CMakeLists.txt", "project(app)\n" } };
    fr_derived_report first; fr_derived_report second; fr_error err;

    fr_derived_apply(scratch, files, 1, 1, &first, &err);
    fr_derived_report_free(&first);

    char written[700];
    snprintf(written, sizeof written, "%s/CMakeLists.txt", scratch);
    long long before = fr_test_file_mtime(written);
    fr_test_sleep_past_mtime_resolution();

    int status = fr_derived_apply(scratch, files, 1, 1, &second, &err);
    long long after = fr_test_file_mtime(written);
    int reported_nothing = second.count == 0;
    fr_derived_report_free(&second);
    fr_test_remove_tree(scratch);

    ASSERT_EQ(FR_OK, status);
    ASSERT(reported_nothing);
    ASSERT(before > 0);
    ASSERT_EQ(before, after);
    PASS();
}

static char last_notice[512];

static void record_notice(const char *message) {
    snprintf(last_notice, sizeof last_notice, "%s", message);
}

TEST a_generated_file_edited_since_it_was_written_is_overwritten_and_reported(void) {
    make_scratch("overwrite");
    fr_generated_file files[1] = { { "CMakeLists.txt", "project(app)\n" } };
    fr_derived_report first; fr_derived_report second; fr_error err;

    fr_derived_apply(scratch, files, 1, 1, &first, &err);
    fr_derived_report_free(&first);

    char written[700];
    snprintf(written, sizeof written, "%s/CMakeLists.txt", scratch);
    fr_file_write_text(written, "a human wrote this\n", &err);

    last_notice[0] = '\0';
    fr_derived_set_notice_sink(record_notice);
    fr_derived_apply(scratch, files, 1, 1, &second, &err);
    fr_derived_set_notice_sink(NULL);
    fr_derived_report_free(&second);

    char *text = read_file(written);
    int was_overwritten = text != NULL && strcmp(text, "project(app)\n") == 0;
    int said_so = strstr(last_notice, "CMakeLists.txt") != NULL
                  && strstr(last_notice, "was changed outside daukle") != NULL;
    free(text);
    fr_test_remove_tree(scratch);

    ASSERT(was_overwritten);
    ASSERT(said_so);
    PASS();
}

TEST a_file_no_longer_generated_is_removed(void) {
    make_scratch("sweep");
    fr_generated_file both[2] = { { "keep.txt", "keep\n" }, { "drop.txt", "drop\n" } };
    fr_generated_file one[1] = { { "keep.txt", "keep\n" } };
    fr_derived_report first; fr_derived_report second; fr_error err;

    fr_derived_apply(scratch, both, 2, 1, &first, &err);
    fr_derived_report_free(&first);
    fr_derived_apply(scratch, one, 1, 1, &second, &err);
    fr_derived_report_free(&second);

    char dropped[700];
    snprintf(dropped, sizeof dropped, "%s/drop.txt", scratch);
    char *gone = read_file(dropped);
    char kept_path[700];
    snprintf(kept_path, sizeof kept_path, "%s/keep.txt", scratch);
    char *kept = read_file(kept_path);
    int dropped_is_gone = gone == NULL;
    int kept_is_there = kept != NULL;
    free(gone);
    free(kept);
    fr_test_remove_tree(scratch);

    ASSERT(dropped_is_gone);
    ASSERT(kept_is_there);
    PASS();
}

TEST a_dropped_file_edited_since_it_was_written_is_kept(void) {
    make_scratch("keepedited");
    fr_generated_file both[2] = { { "keep.txt", "keep\n" }, { "drop.txt", "drop\n" } };
    fr_generated_file one[1] = { { "keep.txt", "keep\n" } };
    fr_derived_report first; fr_derived_report second; fr_error err;

    fr_derived_apply(scratch, both, 2, 1, &first, &err);
    fr_derived_report_free(&first);

    char dropped[700];
    snprintf(dropped, sizeof dropped, "%s/drop.txt", scratch);
    fr_file_write_text(dropped, "a human wrote this\n", &err);

    fr_derived_apply(scratch, one, 1, 1, &second, &err);
    fr_derived_report_free(&second);

    char *survivor = read_file(dropped);
    int still_there = survivor != NULL && strcmp(survivor, "a human wrote this\n") == 0;
    free(survivor);
    fr_test_remove_tree(scratch);

    ASSERT(still_there);
    PASS();
}

TEST a_file_a_tool_wrote_beside_the_generated_ones_is_never_touched(void) {
    make_scratch("toolfile");
    fr_generated_file files[1] = { { "package.json", "{}\n" } };
    fr_derived_report first; fr_derived_report second; fr_error err;

    fr_derived_apply(scratch, files, 1, 1, &first, &err);
    fr_derived_report_free(&first);

    char tool_file[700];
    snprintf(tool_file, sizeof tool_file, "%s/node_modules_marker", scratch);
    fr_file_write_text(tool_file, "installed\n", &err);

    fr_derived_apply(scratch, files, 1, 1, &second, &err);
    fr_derived_report_free(&second);

    char *survivor = read_file(tool_file);
    int untouched = survivor != NULL && strcmp(survivor, "installed\n") == 0;
    free(survivor);
    fr_test_remove_tree(scratch);

    ASSERT(untouched);
    PASS();
}

TEST a_missing_ledger_removes_nothing(void) {
    make_scratch("noledger");
    fr_generated_file both[2] = { { "keep.txt", "keep\n" }, { "drop.txt", "drop\n" } };
    fr_generated_file one[1] = { { "keep.txt", "keep\n" } };
    fr_derived_report first; fr_derived_report second; fr_error err;

    fr_derived_apply(scratch, both, 2, 1, &first, &err);
    fr_derived_report_free(&first);

    char ledger[700];
    snprintf(ledger, sizeof ledger, "%s/.daukle-generated", scratch);
    remove(ledger);

    fr_derived_apply(scratch, one, 1, 1, &second, &err);
    fr_derived_report_free(&second);

    char dropped[700];
    snprintf(dropped, sizeof dropped, "%s/drop.txt", scratch);
    char *survivor = read_file(dropped);
    int still_there = survivor != NULL;
    free(survivor);
    fr_test_remove_tree(scratch);

    ASSERT(still_there);
    PASS();
}

TEST check_writes_no_file_and_no_ledger(void) {
    make_scratch("check");
    fr_generated_file files[1] = { { "CMakeLists.txt", "project(app)\n" } };
    fr_derived_report report; fr_error err;

    int status = fr_derived_apply(scratch, files, 1, 0, &report, &err);
    int reported_once = report.count == 1;
    fr_derived_report_free(&report);

    char written[700];
    snprintf(written, sizeof written, "%s/CMakeLists.txt", scratch);
    char *text = read_file(written);
    char ledger[700];
    snprintf(ledger, sizeof ledger, "%s/.daukle-generated", scratch);
    char *ledger_text = read_file(ledger);
    int wrote_nothing = text == NULL && ledger_text == NULL;
    free(text);
    free(ledger_text);
    fr_test_remove_tree(scratch);

    ASSERT_EQ(FR_OK, status);
    ASSERT(reported_once);
    ASSERT(wrote_nothing);
    PASS();
}

TEST an_empty_file_set_creates_no_directory(void) {
    snprintf(scratch, sizeof scratch, "%s/daukle_test_derived_empty_%d", fr_test_temp_base(),
             fr_test_process_id());
    fr_test_remove_tree(scratch);

    fr_derived_report report; fr_error err;
    int status = fr_derived_apply(scratch, NULL, 0, 1, &report, &err);
    int reported_nothing = report.count == 0;
    fr_derived_report_free(&report);

    char probe[700];
    snprintf(probe, sizeof probe, "%s/.daukle-generated", scratch);
    char *ledger_text = read_file(probe);
    int nothing_created = ledger_text == NULL;
    free(ledger_text);
    fr_test_remove_tree(scratch);

    ASSERT_EQ(FR_OK, status);
    ASSERT(reported_nothing);
    ASSERT(nothing_created);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(a_generated_file_is_written_into_the_derived_directory);
    RUN_TEST(an_unchanged_file_is_not_rewritten);
    RUN_TEST(a_generated_file_edited_since_it_was_written_is_overwritten_and_reported);
    RUN_TEST(a_file_no_longer_generated_is_removed);
    RUN_TEST(a_dropped_file_edited_since_it_was_written_is_kept);
    RUN_TEST(a_file_a_tool_wrote_beside_the_generated_ones_is_never_touched);
    RUN_TEST(a_missing_ledger_removes_nothing);
    RUN_TEST(check_writes_no_file_and_no_ledger);
    RUN_TEST(an_empty_file_set_creates_no_directory);
    GREATEST_MAIN_END();
}
