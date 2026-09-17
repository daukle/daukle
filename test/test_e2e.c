#include "greatest.h"
#include "http.h"
#include "region.h"
#include "support.h"
#include "sync.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void reset_from_pristine(const char *pristine_path, const char *working_path) {
    fr_error err;
    char *text = NULL;
    fr_file_read_text(pristine_path, &text, &err);
    fr_file_write_text(working_path, text, &err);
    free(text);
}

static void reset_fixtures(void) {
    reset_from_pristine("test/fixtures/live/stub-translator/stub/build.gradle.pristine",
                        "test/fixtures/live/stub-translator/stub/build.gradle");
    reset_from_pristine("test/fixtures/live/stub-translator/teavm-stub/build.gradle.pristine",
                        "test/fixtures/live/stub-translator/teavm-stub/build.gradle");
}

TEST reproduces_both_real_consumers(void) {
    reset_fixtures();
    fr_sync_report report; fr_error err;
    ASSERT_EQ(FR_OK, fr_sync("test/fixtures/live/stub-translator/tiestone.json", 1, 1, &report, &err));
    ASSERT_EQ(2, (int) report.count);
    fr_sync_report_free(&report);

    char *stub = NULL;
    fr_file_read_text("test/fixtures/live/stub-translator/stub/build.gradle", &stub, &err);
    ASSERT(strstr(stub, "githubImplementation \"forebay:basekit:5.0.0:ir\"") != NULL);
    free(stub);

    char *teavm = NULL;
    fr_file_read_text("test/fixtures/live/stub-translator/teavm-stub/build.gradle", &teavm, &err);
    ASSERT(strstr(teavm, "githubImplementation \"forebay:basekit:5.0.0:ir\"") != NULL);
    free(teavm);
    reset_fixtures();
    PASS();
}

TEST the_second_run_changes_nothing(void) {
    reset_fixtures();
    fr_sync_report report; fr_error err;
    fr_sync("test/fixtures/live/stub-translator/tiestone.json", 1, 1, &report, &err);
    fr_sync_report_free(&report);
    ASSERT_EQ(FR_OK, fr_sync("test/fixtures/live/stub-translator/tiestone.json", 0, 1, &report, &err));
    ASSERT_EQ(0, (int) report.count);
    fr_sync_report_free(&report);
    reset_fixtures();
    PASS();
}

TEST check_names_every_drifted_consumer(void) {
    reset_fixtures();
    fr_sync_report report; fr_error err;
    ASSERT_EQ(FR_OK, fr_sync("test/fixtures/live/stub-translator/tiestone.json", 0, 1, &report, &err));
    ASSERT_EQ(2, (int) report.count);
    ASSERT(strstr(report.files[0], "/stub/build.gradle") != NULL);
    ASSERT(strstr(report.files[1], "teavm-stub/build.gradle") != NULL);
    fr_sync_report_free(&report);
    reset_fixtures();
    PASS();
}

static const char *BUILD_TEMPLATE =
    "dependencies {\n"
    "    // tiestone:begin\n"
    "    // tiestone:end\n"
    "    testImplementation \"junit\"\n"
    "}\n";

static const char *e2e_temp_root(void) {
    static char root[512];
    snprintf(root, sizeof root, "%s/tiestone_test_e2e_github_%d",
             fr_test_temp_base(), fr_test_process_id());
    return root;
}

static const char *e2e_cache_root(void) {
    static char root[512];
    snprintf(root, sizeof root, "%s/tiestone_test_e2e_github_cache_%d",
             fr_test_temp_base(), fr_test_process_id());
    return root;
}

static void copy_text_file(const char *from, const char *to) {
    fr_error err;
    char *text = NULL;
    fr_file_read_text(from, &text, &err);
    fr_file_write_text(to, text, &err);
    free(text);
}

static void remove_e2e_tree(void) {
    fr_test_remove_tree(e2e_temp_root());
    fr_test_remove_tree(e2e_cache_root());
}

/* Two isolated copies of the same consumer, one resolved through a "path"
   source and one through "github-releases", each writing its own copy of
   the build file so neither run can be tainted by, or mutate, the other's
   state or the checked-in fixtures. */
static void setup_e2e_tree(void) {
    const char *root = e2e_temp_root();
    char path[700];
    fr_error err;

    fr_test_make_directory(root);
    snprintf(path, sizeof path, "%s/path", root); fr_test_make_directory(path);
    snprintf(path, sizeof path, "%s/path/consumer", root); fr_test_make_directory(path);
    snprintf(path, sizeof path, "%s/path/producer", root); fr_test_make_directory(path);
    snprintf(path, sizeof path, "%s/github", root); fr_test_make_directory(path);

    snprintf(path, sizeof path, "%s/path/consumer/tiestone.json", root);
    copy_text_file("test/fixtures/consumer/tiestone.json", path);

    snprintf(path, sizeof path, "%s/path/consumer/build.gradle", root);
    fr_file_write_text(path, BUILD_TEMPLATE, &err);

    snprintf(path, sizeof path, "%s/path/producer/tiestone.json", root);
    copy_text_file("test/fixtures/producer/tiestone.json", path);

    snprintf(path, sizeof path, "%s/github/tiestone-github.json", root);
    copy_text_file("test/fixtures/consumer/tiestone-github.json", path);

    snprintf(path, sizeof path, "%s/github/build.gradle", root);
    fr_file_write_text(path, BUILD_TEMPLATE, &err);
}

static char *extract_generated_region(const char *text) {
    const char *begin = strstr(text, "// tiestone:begin");
    if (begin == NULL) return NULL;
    const char *end = strstr(begin, "// tiestone:end");
    if (end == NULL) return NULL;

    const char *start = begin + strlen("// tiestone:begin");
    size_t length = (size_t) (end - start);
    char *region = malloc(length + 1);
    if (region != NULL) {
        memcpy(region, start, length);
        region[length] = '\0';
    }
    return region;
}

static int GITHUB_STUB_CALLS = 0;

/* Serves the real producer fixture's text, read fresh from disk on every
   call, so this stays byte-identical to what the "path" source reads
   directly and cannot drift into a hand-duplicated copy. */
static int github_stub_get(const char *url, const fr_http_header *headers, size_t header_count,
                           char **out_body, size_t *out_length, fr_error *err) {
    (void) url; (void) headers; (void) header_count;
    GITHUB_STUB_CALLS++;
    char *text = NULL;
    if (fr_file_read_text("test/fixtures/producer/tiestone.json", &text, err) != FR_OK) return FR_ERR;
    *out_length = strlen(text);
    *out_body = text;
    return FR_OK;
}

static int github_cache_entry_exists(void) {
    return fr_test_count_files(e2e_cache_root(), "tiestone.json") > 0;
}

TEST github_source_matches_path_source(void) {
    remove_e2e_tree();
    setup_e2e_tree();

    const char *root = e2e_temp_root();
    char path_manifest[700];
    char path_build[700];
    char github_manifest[700];
    char github_build[700];
    snprintf(path_manifest, sizeof path_manifest, "%s/path/consumer/tiestone.json", root);
    snprintf(path_build, sizeof path_build, "%s/path/consumer/build.gradle", root);
    snprintf(github_manifest, sizeof github_manifest, "%s/github/tiestone-github.json", root);
    snprintf(github_build, sizeof github_build, "%s/github/build.gradle", root);

    fr_error err;
    fr_sync_report path_report;
    ASSERT_EQ(FR_OK, fr_sync(path_manifest, 1, 1, &path_report, &err));
    ASSERT_EQ(1, (int) path_report.count);
    fr_sync_report_free(&path_report);

    fr_test_set_env("TIESTONE_CACHE_DIR", e2e_cache_root());
    fr_http_fn original_backend = fr_http_set_backend(github_stub_get);

    fr_sync_report github_report;
    int github_result = fr_sync(github_manifest, 1, 1, &github_report, &err);

    fr_http_set_backend(original_backend);
    fr_test_set_env("TIESTONE_CACHE_DIR", NULL);

    ASSERT_EQ(FR_OK, github_result);
    ASSERT_EQ(1, (int) github_report.count);
    fr_sync_report_free(&github_report);

    char *path_text = NULL;
    char *github_text = NULL;
    fr_file_read_text(path_build, &path_text, &err);
    fr_file_read_text(github_build, &github_text, &err);

    char *path_region = extract_generated_region(path_text);
    char *github_region = extract_generated_region(github_text);

    ASSERT(path_region != NULL);
    ASSERT(github_region != NULL);
    ASSERT(strstr(path_region, "githubImplementation \"forebay:basekit:5.0.0:ir\"") != NULL);
    ASSERT(strstr(path_region, "githubImplementation \"forebay:basekit:5.0.0:contracts\"") != NULL);
    ASSERT_STR_EQ(path_region, github_region);

    free(path_region);
    free(github_region);
    free(path_text);
    free(github_text);

    remove_e2e_tree();
    PASS();
}

/* The drift exit is what a regenerate-and-diff CI gate rests on, and it was
   only ever proven on the "path" source. A source that fetches has more ways
   to report nothing at all, so it is worth proving on this path too: check
   must name the drifted file and leave it untouched, and must fall silent
   once sync has written it. */
TEST check_reports_drift_through_the_github_source(void) {
    remove_e2e_tree();
    setup_e2e_tree();

    const char *root = e2e_temp_root();
    char github_manifest[700];
    char github_build[700];
    snprintf(github_manifest, sizeof github_manifest, "%s/github/tiestone-github.json", root);
    snprintf(github_build, sizeof github_build, "%s/github/build.gradle", root);

    fr_test_set_env("TIESTONE_CACHE_DIR", e2e_cache_root());
    fr_http_fn original_backend = fr_http_set_backend(github_stub_get);

    fr_error err;
    fr_sync_report drifted;
    int drift_result = fr_sync(github_manifest, 0, 1, &drifted, &err);

    char *after_check = NULL;
    fr_file_read_text(github_build, &after_check, &err);

    fr_sync_report written;
    int write_result = fr_sync(github_manifest, 1, 1, &written, &err);

    fr_sync_report in_sync;
    int in_sync_result = fr_sync(github_manifest, 0, 1, &in_sync, &err);

    fr_http_set_backend(original_backend);
    fr_test_set_env("TIESTONE_CACHE_DIR", NULL);

    ASSERT_EQ(FR_OK, drift_result);
    ASSERT_EQ(1, (int) drifted.count);
    ASSERT(strstr(drifted.files[0], "build.gradle") != NULL);
    fr_sync_report_free(&drifted);

    ASSERT(after_check != NULL);
    ASSERT_STR_EQ(BUILD_TEMPLATE, after_check);
    free(after_check);

    ASSERT_EQ(FR_OK, write_result);
    ASSERT_EQ(1, (int) written.count);
    fr_sync_report_free(&written);

    ASSERT_EQ(FR_OK, in_sync_result);
    ASSERT_EQ(0, (int) in_sync.count);
    fr_sync_report_free(&in_sync);

    remove_e2e_tree();
    PASS();
}

/* --no-cache must bypass both sides of the cache, not just the read: a test
   that only counted fetches would still pass if the write leaked and later
   contaminated a cached run. Each phase starts from a removed cache entry so
   the disabled-cache half and the enabled-cache half cannot contaminate
   each other's backend-call counts. */
TEST no_cache_bypasses_both_the_read_and_the_write(void) {
    remove_e2e_tree();
    setup_e2e_tree();

    const char *root = e2e_temp_root();
    char github_manifest[700];
    snprintf(github_manifest, sizeof github_manifest, "%s/github/tiestone-github.json", root);

    fr_test_set_env("TIESTONE_CACHE_DIR", e2e_cache_root());
    fr_http_fn original_backend = fr_http_set_backend(github_stub_get);

    fr_error err;
    fr_sync_report report;

    GITHUB_STUB_CALLS = 0;
    int first_no_cache = fr_sync(github_manifest, 1, 0, &report, &err);
    fr_sync_report_free(&report);
    int second_no_cache = fr_sync(github_manifest, 1, 0, &report, &err);
    fr_sync_report_free(&report);
    int calls_without_cache = GITHUB_STUB_CALLS;
    int entry_written_without_cache = github_cache_entry_exists();

    fr_test_remove_tree(e2e_cache_root());

    GITHUB_STUB_CALLS = 0;
    int first_with_cache = fr_sync(github_manifest, 1, 1, &report, &err);
    fr_sync_report_free(&report);
    int second_with_cache = fr_sync(github_manifest, 1, 1, &report, &err);
    fr_sync_report_free(&report);
    int calls_with_cache = GITHUB_STUB_CALLS;

    fr_http_set_backend(original_backend);
    fr_test_set_env("TIESTONE_CACHE_DIR", NULL);

    ASSERT_EQ(FR_OK, first_no_cache);
    ASSERT_EQ(FR_OK, second_no_cache);
    ASSERT_EQ(2, calls_without_cache);
    ASSERT_EQ(0, entry_written_without_cache);

    ASSERT_EQ(FR_OK, first_with_cache);
    ASSERT_EQ(FR_OK, second_with_cache);
    ASSERT_EQ(1, calls_with_cache);

    remove_e2e_tree();
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(reproduces_both_real_consumers);
    RUN_TEST(the_second_run_changes_nothing);
    RUN_TEST(check_names_every_drifted_consumer);
    RUN_TEST(github_source_matches_path_source);
    RUN_TEST(check_reports_drift_through_the_github_source);
    RUN_TEST(no_cache_bypasses_both_the_read_and_the_write);
    GREATEST_MAIN_END();
}
