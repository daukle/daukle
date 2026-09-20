#include "greatest.h"
#include "error.h"
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
    ASSERT_EQ(FR_OK, fr_sync("test/fixtures/live/stub-translator/daukle.toml", 1, 1, &report, &err));
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
    fr_sync("test/fixtures/live/stub-translator/daukle.toml", 1, 1, &report, &err);
    fr_sync_report_free(&report);
    ASSERT_EQ(FR_OK, fr_sync("test/fixtures/live/stub-translator/daukle.toml", 0, 1, &report, &err));
    ASSERT_EQ(0, (int) report.count);
    fr_sync_report_free(&report);
    reset_fixtures();
    PASS();
}

TEST check_names_every_drifted_consumer(void) {
    reset_fixtures();
    fr_sync_report report; fr_error err;
    ASSERT_EQ(FR_OK, fr_sync("test/fixtures/live/stub-translator/daukle.toml", 0, 1, &report, &err));
    ASSERT_EQ(2, (int) report.count);
    ASSERT(strstr(report.files[0], "/stub/build.gradle") != NULL);
    ASSERT(strstr(report.files[1], "teavm-stub/build.gradle") != NULL);
    fr_sync_report_free(&report);
    reset_fixtures();
    PASS();
}

static const char *BUILD_TEMPLATE =
    "dependencies {\n"
    "    // daukle:begin\n"
    "    // daukle:end\n"
    "    testImplementation \"junit\"\n"
    "}\n";

static const char *e2e_temp_root(void) {
    static char root[512];
    snprintf(root, sizeof root, "%s/daukle_test_e2e_github_%d",
             fr_test_temp_base(), fr_test_process_id());
    return root;
}

static const char *e2e_cache_root(void) {
    static char root[512];
    snprintf(root, sizeof root, "%s/daukle_test_e2e_github_cache_%d",
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
   state or the checked-in fixtures. The producer and each side's plugin copy
   sit inside the manifest's own directory, because neither daukle.read nor a
   local plugin path may climb out of it. */
static void setup_e2e_tree(void) {
    const char *root = e2e_temp_root();
    char path[700];
    fr_error err;

    fr_test_make_directory(root);
    snprintf(path, sizeof path, "%s/path", root); fr_test_make_directory(path);
    snprintf(path, sizeof path, "%s/path/consumer", root); fr_test_make_directory(path);
    snprintf(path, sizeof path, "%s/path/consumer/producer", root); fr_test_make_directory(path);
    snprintf(path, sizeof path, "%s/path/consumer/plugins", root); fr_test_make_directory(path);
    snprintf(path, sizeof path, "%s/github", root); fr_test_make_directory(path);
    snprintf(path, sizeof path, "%s/github/plugins", root); fr_test_make_directory(path);

    snprintf(path, sizeof path, "%s/path/consumer/daukle.toml", root);
    copy_text_file("test/fixtures/consumer/daukle.toml", path);

    snprintf(path, sizeof path, "%s/path/consumer/build.gradle", root);
    fr_file_write_text(path, BUILD_TEMPLATE, &err);

    snprintf(path, sizeof path, "%s/path/consumer/producer/daukle.toml", root);
    copy_text_file("test/fixtures/consumer/producer/daukle.toml", path);

    snprintf(path, sizeof path, "%s/path/consumer/plugins/path.lua", root);
    copy_text_file("test/fixtures/consumer/plugins/path.lua", path);

    snprintf(path, sizeof path, "%s/path/consumer/plugins/gradle.lua", root);
    copy_text_file("test/fixtures/consumer/plugins/gradle.lua", path);

    snprintf(path, sizeof path, "%s/github/daukle-github.toml", root);
    copy_text_file("test/fixtures/consumer/daukle-github.toml", path);

    snprintf(path, sizeof path, "%s/github/plugins/github.lua", root);
    copy_text_file("test/fixtures/consumer/plugins/github.lua", path);

    snprintf(path, sizeof path, "%s/github/plugins/gradle.lua", root);
    copy_text_file("test/fixtures/consumer/plugins/gradle.lua", path);

    snprintf(path, sizeof path, "%s/github/build.gradle", root);
    fr_file_write_text(path, BUILD_TEMPLATE, &err);
}

static char *extract_generated_region(const char *text) {
    const char *begin = strstr(text, "// daukle:begin");
    if (begin == NULL) return NULL;
    const char *end = strstr(begin, "// daukle:end");
    if (end == NULL) return NULL;

    const char *start = begin + strlen("// daukle:begin");
    size_t length = (size_t) (end - start);
    char *region = malloc(length + 1);
    if (region != NULL) {
        memcpy(region, start, length);
        region[length] = '\0';
    }
    return region;
}

static int GITHUB_STUB_CALLS = 0;

static const char *RELEASE_URL =
    "https://github.com/forebay/basekit/releases/download/5.0.0/daukle.toml";

/* Serves the same toml producer the path source reads, so the equality
   assertion below compares two sources rather than two encodings. Refusing
   every other url is what stops a plugin that built the wrong one from being
   served a manifest anyway and passing. */
static int github_stub_get(const char *url, const fr_http_header *headers, size_t header_count,
                           char **out_body, size_t *out_length, fr_error *err) {
    (void) headers; (void) header_count;
    GITHUB_STUB_CALLS++;
    if (strcmp(url, RELEASE_URL) != 0) {
        fr_error_set(err, "the plugin requested \"%s\"", url);
        return FR_ERR;
    }
    char *text = NULL;
    if (fr_file_read_text("test/fixtures/consumer/producer/daukle.toml", &text, err) != FR_OK) {
        return FR_ERR;
    }
    *out_length = strlen(text);
    *out_body = text;
    return FR_OK;
}

static int github_cache_entry_exists(void) {
    return fr_test_count_files(e2e_cache_root(), "daukle.json") > 0;
}

TEST github_source_matches_path_source(void) {
    remove_e2e_tree();
    setup_e2e_tree();

    const char *root = e2e_temp_root();
    char path_manifest[700];
    char path_build[700];
    char github_manifest[700];
    char github_build[700];
    snprintf(path_manifest, sizeof path_manifest, "%s/path/consumer/daukle.toml", root);
    snprintf(path_build, sizeof path_build, "%s/path/consumer/build.gradle", root);
    snprintf(github_manifest, sizeof github_manifest, "%s/github/daukle-github.toml", root);
    snprintf(github_build, sizeof github_build, "%s/github/build.gradle", root);

    fr_error err;
    fr_sync_report path_report;
    ASSERT_EQ(FR_OK, fr_sync(path_manifest, 1, 1, &path_report, &err));
    ASSERT_EQ(1, (int) path_report.count);
    fr_sync_report_free(&path_report);

    fr_test_set_env("DAUKLE_CACHE_DIR", e2e_cache_root());
    fr_http_fn original_backend = fr_http_set_backend(github_stub_get);

    fr_sync_report github_report;
    int github_result = fr_sync(github_manifest, 1, 1, &github_report, &err);

    fr_http_set_backend(original_backend);
    fr_test_set_env("DAUKLE_CACHE_DIR", NULL);

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
    snprintf(github_manifest, sizeof github_manifest, "%s/github/daukle-github.toml", root);
    snprintf(github_build, sizeof github_build, "%s/github/build.gradle", root);

    fr_test_set_env("DAUKLE_CACHE_DIR", e2e_cache_root());
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
    fr_test_set_env("DAUKLE_CACHE_DIR", NULL);

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
    snprintf(github_manifest, sizeof github_manifest, "%s/github/daukle-github.toml", root);

    fr_test_set_env("DAUKLE_CACHE_DIR", e2e_cache_root());
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
    fr_test_set_env("DAUKLE_CACHE_DIR", NULL);

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

static const char *STALE_PACKAGE_JSON =
    "{\n"
    "  \"name\": \"example\",\n"
    "  \"version\": \"1.0.0\",\n"
    "  \"dependencies\": {\n"
    "    \"@intisy-ai/basekit-contracts\": \"%s\",\n"
    "    \"@intisy-ai/basekit-ir\": \"%s\"\n"
    "  },\n"
    "  \"daukle\": {\n"
    "    \"managed\": {\n"
    "      \"dependencies\": [\n"
    "        \"@intisy-ai/basekit-contracts\",\n"
    "        \"@intisy-ai/basekit-ir\"\n"
    "      ]\n"
    "    }\n"
    "  }\n"
    "}\n";

static void seed_two_ways_consumer(const char *path, const char *stale_range) {
    char text[800];
    snprintf(text, sizeof text, STALE_PACKAGE_JSON, stale_range, stale_range);
    fr_error err;
    fr_file_write_text(path, text, &err);
}

/* Each consumer starts from a different stale range, so two identical files
   at the end cannot be what two syncs that both did nothing would leave, and
   a format plugin that dropped "consumers" and reported success would show up
   here instead of passing unnoticed. Each file is put back before the first
   assertion, so a failure does not leave the tree dirty either way. */
TEST the_same_manifest_in_two_formats_writes_the_same_file(void) {
    const char *toml_consumer = "test/fixtures/two-ways/toml/package.json";
    const char *lua_consumer = "test/fixtures/two-ways/lua/package.json";

    seed_two_ways_consumer(toml_consumer, "^2.0.0");
    seed_two_ways_consumer(lua_consumer, "^3.0.0");

    fr_error err;
    fr_sync_report report;

    int toml_status = fr_sync("test/fixtures/two-ways/toml/daukle.toml", 1, 0, &report, &err);
    size_t toml_writes = report.count;
    fr_sync_report_free(&report);

    int lua_status = fr_sync("test/fixtures/two-ways/lua/daukle.lua", 1, 0, &report, &err);
    size_t lua_writes = report.count;
    fr_sync_report_free(&report);

    char *from_toml = NULL;
    char *from_lua = NULL;
    int toml_read = fr_file_read_text(toml_consumer, &from_toml, &err);
    int lua_read = fr_file_read_text(lua_consumer, &from_lua, &err);

    seed_two_ways_consumer(toml_consumer, "^2.0.0");
    seed_two_ways_consumer(lua_consumer, "^3.0.0");

    ASSERT_EQ(FR_OK, toml_status);
    ASSERT_EQ(FR_OK, lua_status);
    ASSERT_EQ(1, (int) toml_writes);
    ASSERT_EQ(1, (int) lua_writes);

    ASSERT_EQ(FR_OK, toml_read);
    ASSERT_EQ(FR_OK, lua_read);
    ASSERT_STR_EQ(from_toml, from_lua);
    ASSERT(strstr(from_toml, "^5.0.0") != NULL);

    free(from_toml);
    free(from_lua);
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
    RUN_TEST(the_same_manifest_in_two_formats_writes_the_same_file);
    GREATEST_MAIN_END();
}
