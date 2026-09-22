#include "greatest.h"

#include "derived.h"
#include "region.h"
#include "sha256.h"
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

TEST more_files_than_the_ledger_can_hold_is_rejected(void) {
    make_scratch("toomany");
    enum { TOO_MANY = 65 };
    fr_generated_file files[TOO_MANY];
    char paths[TOO_MANY][32];
    for (int index = 0; index < TOO_MANY; index++) {
        snprintf(paths[index], sizeof paths[index], "file_%d.txt", index);
        files[index].path = paths[index];
        files[index].text = "x\n";
    }
    fr_derived_report report; fr_error err;

    int status = fr_derived_apply(scratch, files, TOO_MANY, 1, &report, &err);
    fr_derived_report_free(&report);
    fr_test_remove_tree(scratch);

    ASSERT_EQ(FR_ERR, status);
    PASS();
}

TEST a_generated_file_creates_its_own_directory(void) {
    snprintf(scratch, sizeof scratch, "%s/daukle_test_derived_newdir_%d", fr_test_temp_base(),
             fr_test_process_id());
    fr_test_remove_tree(scratch);

    fr_generated_file files[1] = { { "sub/dir/file.txt", "generated\n" } };
    fr_derived_report report; fr_error err;

    int status = fr_derived_apply(scratch, files, 1, 1, &report, &err);
    char written[700];
    snprintf(written, sizeof written, "%s/sub/dir/file.txt", scratch);
    char *text = read_file(written);
    int content_is_right = text != NULL && strcmp(text, "generated\n") == 0;
    free(text);
    fr_derived_report_free(&report);
    fr_test_remove_tree(scratch);

    ASSERT_EQ(FR_OK, status);
    ASSERT(content_is_right);
    PASS();
}

TEST a_ledger_line_that_escapes_the_derived_directory_is_ignored(void) {
    make_scratch("ledgerescape");

    char old_path[700];
    snprintf(old_path, sizeof old_path, "%s/old.txt", scratch);
    fr_error err;
    fr_file_write_text(old_path, "old\n", &err);
    char old_digest[65];
    fr_sha256_hex("old\n", strlen("old\n"), old_digest);

    char victim_name[256];
    snprintf(victim_name, sizeof victim_name, "daukle_test_derived_victim_%d.txt",
             fr_test_process_id());
    char victim_path[700];
    snprintf(victim_path, sizeof victim_path, "%s/%s", fr_test_temp_base(), victim_name);
    fr_file_write_text(victim_path, "precious\n", &err);
    char victim_digest[65];
    fr_sha256_hex("precious\n", strlen("precious\n"), victim_digest);

    char ledger_path[700];
    snprintf(ledger_path, sizeof ledger_path, "%s/.daukle-generated", scratch);
    char ledger_text[512];
    snprintf(ledger_text, sizeof ledger_text, "old.txt\t%s\n../%s\t%s\n",
             old_digest, victim_name, victim_digest);
    fr_file_write_text(ledger_path, ledger_text, &err);

    fr_derived_report report;
    int status = fr_derived_apply(scratch, NULL, 0, 1, &report, &err);
    fr_derived_report_free(&report);

    char *old_survivor = read_file(old_path);
    char *victim_survivor = read_file(victim_path);
    int old_was_swept = old_survivor == NULL;
    int victim_untouched = victim_survivor != NULL && strcmp(victim_survivor, "precious\n") == 0;
    free(old_survivor);
    free(victim_survivor);

    fr_test_remove_tree(scratch);
    remove(victim_path);

    ASSERT_EQ(FR_OK, status);
    ASSERT(old_was_swept);
    ASSERT(victim_untouched);
    PASS();
}

TEST a_generated_path_that_escapes_the_derived_directory_is_rejected(void) {
    make_scratch("escapegen");
    fr_generated_file files[1] = { { "../evil.txt", "evil\n" } };
    fr_derived_report report; fr_error err;

    int status = fr_derived_apply(scratch, files, 1, 1, &report, &err);
    fr_derived_report_free(&report);

    char evil_path[700];
    snprintf(evil_path, sizeof evil_path, "%s/../evil.txt", scratch);
    char *text = read_file(evil_path);
    int nothing_written = text == NULL;
    free(text);

    fr_test_remove_tree(scratch);
    remove(evil_path);

    ASSERT_EQ(FR_ERR, status);
    ASSERT(nothing_written);
    PASS();
}

/* Asserted on the error message, not merely on FR_ERR: a path this long also
   exceeds Windows' own MAX_PATH, so an implementation with no truncation
   guard at all would still fail here, for the OS's reason rather than
   derived_join's. Only the message proves OUR guard, not a coincidence,
   caught it. */
TEST a_path_too_long_to_join_is_rejected(void) {
    make_scratch("toolong");
    char long_name[1200];
    memset(long_name, 'a', sizeof long_name - 5);
    memcpy(long_name + sizeof long_name - 5, ".txt", 5);
    fr_generated_file files[1] = { { long_name, "x\n" } };
    fr_derived_report report; fr_error err;
    err.message[0] = '\0';

    int status = fr_derived_apply(scratch, files, 1, 1, &report, &err);
    int said_so = strstr(err.message, "too long") != NULL;
    fr_derived_report_free(&report);
    fr_test_remove_tree(scratch);

    ASSERT_EQ(FR_ERR, status);
    ASSERT(said_so);
    PASS();
}

TEST a_run_that_drops_every_file_rewrites_the_ledger_empty(void) {
    make_scratch("dropall");
    fr_generated_file both[2] = { { "keep.txt", "keep\n" }, { "drop.txt", "drop\n" } };
    fr_derived_report first; fr_derived_report second; fr_error err;

    fr_derived_apply(scratch, both, 2, 1, &first, &err);
    fr_derived_report_free(&first);

    int status = fr_derived_apply(scratch, NULL, 0, 1, &second, &err);
    fr_derived_report_free(&second);

    char ledger_path[700];
    snprintf(ledger_path, sizeof ledger_path, "%s/.daukle-generated", scratch);
    char *ledger_text = read_file(ledger_path);
    int ledger_is_empty = ledger_text != NULL && ledger_text[0] == '\0';
    free(ledger_text);
    fr_test_remove_tree(scratch);

    ASSERT_EQ(FR_OK, status);
    ASSERT(ledger_is_empty);
    PASS();
}

TEST an_empty_toolchain_name_is_rejected(void) {
    char *dir = NULL; fr_error err;
    int status = fr_derived_dir("build_test_manifest_dir", "", &dir, &err);
    int said_so = strstr(err.message, "cannot be a directory name") != NULL;

    ASSERT_EQ(FR_ERR, status);
    ASSERT(dir == NULL);
    ASSERT(said_so);
    PASS();
}

TEST a_toolchain_name_that_climbs_out_is_rejected(void) {
    char *dir = NULL; fr_error err;
    int status = fr_derived_dir("build_test_manifest_dir", "../x", &dir, &err);
    int said_so = strstr(err.message, "cannot be a directory name") != NULL;

    ASSERT_EQ(FR_ERR, status);
    ASSERT(dir == NULL);
    ASSERT(said_so);
    PASS();
}

TEST a_toolchain_name_with_a_separator_is_rejected(void) {
    char *dir = NULL; fr_error err;
    int status = fr_derived_dir("build_test_manifest_dir", "a/b", &dir, &err);
    int said_so = strstr(err.message, "cannot be a directory name") != NULL;

    ASSERT_EQ(FR_ERR, status);
    ASSERT(dir == NULL);
    ASSERT(said_so);
    PASS();
}

TEST a_plain_toolchain_name_resolves_beneath_the_derived_root(void) {
    char *dir = NULL; fr_error err;
    int status = fr_derived_dir("build_test_manifest_dir", "npm", &dir, &err);
    int right_path = dir != NULL
                     && strcmp(dir, "build_test_manifest_dir/build/daukle/npm") == 0;
    free(dir);

    ASSERT_EQ(FR_OK, status);
    ASSERT(right_path);
    PASS();
}

TEST ensure_root_creates_the_directory_and_a_gitignore_it_never_overwrites(void) {
    snprintf(scratch, sizeof scratch, "%s/daukle_test_derived_root_%d", fr_test_temp_base(),
             fr_test_process_id());
    fr_test_remove_tree(scratch);

    fr_error err;
    int status = fr_derived_ensure_root(scratch, &err);

    char gitignore_path[700];
    snprintf(gitignore_path, sizeof gitignore_path, "%s/.gitignore", scratch);
    char *text = read_file(gitignore_path);
    int content_is_right = text != NULL && strcmp(text, "*\n") == 0;
    free(text);

    fr_file_write_text(gitignore_path, "custom\n", &err);
    int second_status = fr_derived_ensure_root(scratch, &err);
    char *after = read_file(gitignore_path);
    int untouched = after != NULL && strcmp(after, "custom\n") == 0;
    free(after);

    fr_test_remove_tree(scratch);

    ASSERT_EQ(FR_OK, status);
    ASSERT(content_is_right);
    ASSERT_EQ(FR_OK, second_status);
    ASSERT(untouched);
    PASS();
}

TEST the_derived_root_sits_under_build_daukle(void) {
    char *dir = NULL; fr_error err;
    int status = fr_derived_root("build_test_manifest_dir", &dir, &err);
    int right_path = dir != NULL && strcmp(dir, "build_test_manifest_dir/build/daukle") == 0;
    free(dir);

    ASSERT_EQ(FR_OK, status);
    ASSERT(right_path);
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
    RUN_TEST(more_files_than_the_ledger_can_hold_is_rejected);
    RUN_TEST(a_generated_file_creates_its_own_directory);
    RUN_TEST(a_ledger_line_that_escapes_the_derived_directory_is_ignored);
    RUN_TEST(a_generated_path_that_escapes_the_derived_directory_is_rejected);
    RUN_TEST(a_path_too_long_to_join_is_rejected);
    RUN_TEST(a_run_that_drops_every_file_rewrites_the_ledger_empty);
    RUN_TEST(an_empty_toolchain_name_is_rejected);
    RUN_TEST(a_toolchain_name_that_climbs_out_is_rejected);
    RUN_TEST(a_toolchain_name_with_a_separator_is_rejected);
    RUN_TEST(a_plain_toolchain_name_resolves_beneath_the_derived_root);
    RUN_TEST(ensure_root_creates_the_directory_and_a_gitignore_it_never_overwrites);
    RUN_TEST(the_derived_root_sits_under_build_daukle);
    GREATEST_MAIN_END();
}
