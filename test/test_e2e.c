#include "greatest.h"
#include "cli/cli.h"
#include "net/http.h"
#include "plugin/plugins.h"
#include "plugin/registry.h"
#include "project/region.h"
#include "util/error.h"
#include "support.h"
#include "project/sync.h"
#include "project/tasks.h"

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
    return fr_test_count_files(e2e_cache_root(), "manifest") > 0;
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

/* The child writes into whatever directory it was started in, which is the
   only way to observe cwd: a run that lost the default would write into
   daukle's own working directory and the assertion below would fail. That is
   the whole point of this test, and the residual it closes. */
static int run_as_task_child(const char *marker_name) {
    FILE *marker = fopen(marker_name, "w");
    if (marker == NULL) return 1;
    fputs("ok", marker);
    fclose(marker);
    return 0;
}

/* argv[0] as ctest invokes it, kept so the fixture's plugin can resolve this
   binary by name through daukle.tool, which searches PATH only. */
static const char *self_path;

static void put_environment(const char *name, const char *value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

static int file_exists(const char *path) {
    FILE *handle = fopen(path, "r");
    if (handle == NULL) return 0;
    fclose(handle);
    return 1;
}

TEST a_task_runs_its_child_in_the_derived_directory(void) {
    char directory[1024];
    snprintf(directory, sizeof directory, "%s", self_path);
    char *last = strrchr(directory, '/');
    char *last_back = strrchr(directory, '\\');
    if (last_back != NULL && (last == NULL || last_back > last)) last = last_back;
    if (last != NULL) *last = '\0';

    fr_error err;
    fr_session session;
    ASSERT_EQ(FR_OK, fr_session_open("test/fixtures/task-cwd/daukle.toml", 1, &session, &err));

    fr_sync_report report;
    ASSERT_EQ(FR_OK, fr_sync_session(&session, 1, &report, &err));
    fr_sync_report_free(&report);

    fr_task_set set;
    ASSERT_EQ(FR_OK, fr_tasks_collect(session.registry, &session.manifest, &set, &err));
    fr_task_plan plan;
    ASSERT_EQ(FR_OK, fr_tasks_plan(&set, "runner:touch", &plan, &err));

    const char *original_path = getenv("PATH");
    char saved_path[4096];
    snprintf(saved_path, sizeof saved_path, "%s", original_path == NULL ? "" : original_path);
    char path_value[4096];
#ifdef _WIN32
    snprintf(path_value, sizeof path_value, "%s;%s", directory, saved_path);
#else
    snprintf(path_value, sizeof path_value, "%s:%s", directory, saved_path);
#endif
    put_environment("PATH", path_value);
    put_environment("DAUKLE_TEST_CHILD", last == NULL ? self_path : last + 1);

    const char *marker = "test/fixtures/task-cwd/build/daukle/runner/ran-here.txt";
    remove(marker);

    int status = fr_tasks_run(&plan, &session, &err);
    int ran_here = file_exists(marker);

    put_environment("PATH", saved_path);
    fr_tasks_plan_free(&plan);
    fr_tasks_set_free(&set);
    fr_session_close(&session);
    remove(marker);

    ASSERT_EQ(FR_OK, status);
    ASSERT(ran_here);
    PASS();
}

/* The hatch read from a real toml rather than a json document built in a
   test: an inline table is the shape a manifest actually writes `run` in, and
   nothing else in the suite parses one. The project declares no plugin at
   all, which is the shape D-53 exists for, and the marker lands beside the
   manifest because a manifest task's default working directory is the project
   itself. */
TEST a_manifest_task_runs_a_program_from_a_real_manifest(void) {
    char directory[1024];
    snprintf(directory, sizeof directory, "%s", self_path);
    char *last = strrchr(directory, '/');
    char *last_back = strrchr(directory, '\\');
    if (last_back != NULL && (last == NULL || last_back > last)) last = last_back;
    if (last != NULL) *last = '\0';

    fr_error err;
    fr_session session;
    ASSERT_EQ(FR_OK, fr_session_open("test/fixtures/task-run/daukle.toml", 1, &session, &err));

    fr_task_set set;
    ASSERT_EQ(FR_OK, fr_tasks_collect(session.registry, &session.manifest, &set, &err));
    fr_task_plan plan;
    ASSERT_EQ(FR_OK, fr_tasks_plan(&set, "hatch", &plan, &err));

    const char *original_path = getenv("PATH");
    char saved_path[4096];
    snprintf(saved_path, sizeof saved_path, "%s", original_path == NULL ? "" : original_path);
    char path_value[4096];
#ifdef _WIN32
    snprintf(path_value, sizeof path_value, "%s;%s", directory, saved_path);
#else
    snprintf(path_value, sizeof path_value, "%s:%s", directory, saved_path);
#endif
    put_environment("PATH", path_value);

    const char *marker = "test/fixtures/task-run/ran-here.txt";
    remove(marker);

    int status = fr_tasks_run(&plan, &session, &err);
    int ran_beside_the_manifest = file_exists(marker);

    put_environment("PATH", saved_path);
    fr_tasks_plan_free(&plan);
    fr_tasks_set_free(&set);
    fr_session_close(&session);
    remove(marker);

    ASSERT_EQ(FR_OK, status);
    ASSERT(ran_beside_the_manifest);
    PASS();
}

/* The publisher plugin declares no toolchain at all; the "runner" toolchain
   comes from a second plugin in the same manifest. That split is the point:
   a publisher gets exec without being, or needing, a toolchain itself, and
   the child still lands in the directory the destination's "from" derives. */
TEST a_publisher_runs_its_child_in_the_from_toolchains_directory(void) {
    char directory[1024];
    snprintf(directory, sizeof directory, "%s", self_path);
    char *last = strrchr(directory, '/');
    char *last_back = strrchr(directory, '\\');
    if (last_back != NULL && (last == NULL || last_back > last)) last = last_back;
    if (last != NULL) *last = '\0';

    fr_error err;
    fr_session session;
    ASSERT_EQ(FR_OK, fr_session_open("test/fixtures/publish-cwd/daukle.toml", 1, &session, &err));

    fr_sync_report report;
    ASSERT_EQ(FR_OK, fr_sync_session(&session, 1, &report, &err));
    fr_sync_report_free(&report);

    fr_task_set set;
    ASSERT_EQ(FR_OK, fr_tasks_collect(session.registry, &session.manifest, &set, &err));
    fr_task_plan plan;
    ASSERT_EQ(FR_OK, fr_tasks_plan(&set, "publish:somewhere", &plan, &err));

    const char *original_path = getenv("PATH");
    char saved_path[4096];
    snprintf(saved_path, sizeof saved_path, "%s", original_path == NULL ? "" : original_path);
    char path_value[4096];
#ifdef _WIN32
    snprintf(path_value, sizeof path_value, "%s;%s", directory, saved_path);
#else
    snprintf(path_value, sizeof path_value, "%s:%s", directory, saved_path);
#endif
    put_environment("PATH", path_value);
    put_environment("DAUKLE_TEST_CHILD", last == NULL ? self_path : last + 1);

    const char *marker = "test/fixtures/publish-cwd/build/daukle/runner/ran-here.txt";
    remove(marker);

    int status = fr_tasks_run(&plan, &session, &err);
    int ran_here = file_exists(marker);

    put_environment("PATH", saved_path);
    fr_tasks_plan_free(&plan);
    fr_tasks_set_free(&set);
    fr_session_close(&session);
    remove(marker);

    ASSERT_EQ(FR_OK, status);
    ASSERT(ran_here);
    PASS();
}

static void record_published_goal(const char *goal, void *state) {
    char *order = state;
    size_t used = strlen(order);
    snprintf(order + used, 256 - used, "%s;", goal);
}

/* Puts this binary's own directory on PATH and names the binary as the child a
   fixture's plugin should exec, which is the only route a plugin has to it:
   daukle.tool searches PATH and refuses a name holding a separator. The caller
   restores saved_path. */
static void point_fixtures_at_this_binary(char *saved_path, size_t size) {
    char directory[1024];
    snprintf(directory, sizeof directory, "%s", self_path);
    char *last = strrchr(directory, '/');
    char *last_back = strrchr(directory, '\\');
    if (last_back != NULL && (last == NULL || last_back > last)) last = last_back;
    if (last != NULL) *last = '\0';

    const char *original_path = getenv("PATH");
    snprintf(saved_path, size, "%s", original_path == NULL ? "" : original_path);
    char path_value[4096];
#ifdef _WIN32
    snprintf(path_value, sizeof path_value, "%s;%s", directory, saved_path);
#else
    snprintf(path_value, sizeof path_value, "%s:%s", directory, saved_path);
#endif
    put_environment("PATH", path_value);

    const char *base_name = strrchr(self_path, '/');
    const char *base_back = strrchr(self_path, '\\');
    if (base_back != NULL && (base_name == NULL || base_back > base_name)) base_name = base_back;
    put_environment("DAUKLE_TEST_CHILD", base_name == NULL ? self_path : base_name + 1);
}

/* The other half of the Critical's gap, and the half no unit test can reach:
   that daukle really does run EVERY selected destination, in the order the
   manifest declares them. It is a test at all only because the loop was moved
   out of main.c, which has no test binary, into fr_tasks_run_publish beside the
   check that guards it. Both markers and the recorded order are asserted: a
   loop that ran the first destination twice would satisfy either one alone. */
TEST every_destination_runs_in_the_order_the_manifest_declares(void) {
    fr_error err;
    fr_session session;
    ASSERT_EQ(FR_OK, fr_session_open("test/fixtures/publish-two/daukle.toml", 1, &session, &err));

    fr_sync_report report;
    ASSERT_EQ(FR_OK, fr_sync_session(&session, 1, &report, &err));
    fr_sync_report_free(&report);

    fr_task_set set;
    ASSERT_EQ(FR_OK, fr_tasks_collect(session.registry, &session.manifest, &set, &err));

    char saved_path[4096];
    point_fixtures_at_this_binary(saved_path, sizeof saved_path);

    const char *alpha_marker = "test/fixtures/publish-two/build/daukle/runner/alpha-ran.txt";
    const char *beta_marker = "test/fixtures/publish-two/build/daukle/runner/beta-ran.txt";
    remove(alpha_marker);
    remove(beta_marker);

    static char order[256];
    order[0] = '\0';
    int checked = fr_tasks_check_publish(&set, &session.manifest, NULL, &err);
    int status = fr_tasks_run_publish(&set, &session, NULL, record_published_goal, order, &err);
    int alpha_ran = file_exists(alpha_marker);
    int beta_ran = file_exists(beta_marker);

    put_environment("PATH", saved_path);
    fr_tasks_set_free(&set);
    fr_session_close(&session);
    remove(alpha_marker);
    remove(beta_marker);

    ASSERT_EQ(FR_OK, checked);
    ASSERT_EQ(FR_OK, status);
    ASSERT(alpha_ran);
    ASSERT(beta_ran);
    ASSERT_STR_EQ("publish:alpha;publish:beta;", order);
    PASS();
}

/* The filter the same loop carries, which "daukle publish beta" reaches. It is
   asserted here because moving the loop into core moved this line with it, and
   a line that changed layers without gaining a test has only moved. */
TEST naming_one_destination_runs_only_that_one(void) {
    fr_error err;
    fr_session session;
    ASSERT_EQ(FR_OK, fr_session_open("test/fixtures/publish-two/daukle.toml", 1, &session, &err));

    fr_sync_report report;
    ASSERT_EQ(FR_OK, fr_sync_session(&session, 1, &report, &err));
    fr_sync_report_free(&report);

    fr_task_set set;
    ASSERT_EQ(FR_OK, fr_tasks_collect(session.registry, &session.manifest, &set, &err));

    char saved_path[4096];
    point_fixtures_at_this_binary(saved_path, sizeof saved_path);

    const char *alpha_marker = "test/fixtures/publish-two/build/daukle/runner/alpha-ran.txt";
    const char *beta_marker = "test/fixtures/publish-two/build/daukle/runner/beta-ran.txt";
    remove(alpha_marker);
    remove(beta_marker);

    static char order[256];
    order[0] = '\0';
    int status = fr_tasks_run_publish(&set, &session, "beta", record_published_goal, order, &err);
    int alpha_ran = file_exists(alpha_marker);
    int beta_ran = file_exists(beta_marker);

    put_environment("PATH", saved_path);
    fr_tasks_set_free(&set);
    fr_session_close(&session);
    remove(alpha_marker);
    remove(beta_marker);

    ASSERT_EQ(FR_OK, status);
    ASSERT(beta_ran);
    ASSERT(!alpha_ran);
    ASSERT_STR_EQ("publish:beta;", order);
    PASS();
}

/* plugin-deps-e2e declares one root plugin ("dependent") that requires one dependency
   ("provider", replaced by a path override so nothing is fetched over the network): the report
   therefore carries two rows, and "this project declares N plugins" must still say 1, not 2, since
   the project's own [plugins] table names exactly one. Asserting on the raw report count first
   pins that a dependency really is in there; asserting on fr_plugins_report_declared_count pins
   that counting stops at the rows the project actually declared. */
TEST an_unknown_goal_names_the_plugin_count(void) {
    fr_error err;
    fr_session session;
    ASSERT_EQ(FR_OK,
             fr_session_open("test/fixtures/plugin-deps-e2e/daukle.toml", 1, &session, &err));
    fr_task_set set;
    ASSERT_EQ(FR_OK, fr_tasks_collect(session.registry, &session.manifest, &set, &err));

    fr_task_plan plan;
    int plan_status = fr_tasks_plan(&set, "build", &plan, &err);
    int found = fr_tasks_find(&set, "build") != NULL;
    size_t report_count = fr_plugins_report()->count;
    size_t plugin_count = fr_plugins_report_declared_count(fr_plugins_report());

    fr_tasks_set_free(&set);
    fr_session_close(&session);

    ASSERT_EQ(FR_ERR, plan_status);
    ASSERT(!found);
    ASSERT_EQ(2u, report_count);
    ASSERT_EQ(1u, plugin_count);
    PASS();
}

TEST check_runs_no_task(void) {
    const char *marker = "test/fixtures/task-cwd/build/daukle/runner/ran-here.txt";
    remove(marker);

    fr_sync_report report; fr_error err;
    ASSERT_EQ(FR_OK, fr_sync("test/fixtures/task-cwd/daukle.toml", 0, 1, &report, &err));
    fr_sync_report_free(&report);

    /* Neither check nor sync runs a task: daukle <task> is the only command that starts one. */
    ASSERT(!file_exists(marker));
    PASS();
}

/* Through fr_session_open rather than any of plugin_deps.c's or plugin_modules.c's
   own seams: a real daukle.toml, a dependent plugin at a local path, and a provider
   it reaches only through the manifest's override of "provider" (a fetched url
   cannot be a fixture). The capability the test looks up is the exact string the
   provider's own lib/marker.lua returns, which the dependent never writes itself,
   so a require that resolved to the wrong artifact, an empty table, or the
   dependent's own module would still let the plugin load but would leave this
   capability absent from the registry. */
TEST a_required_modules_return_value_reaches_the_registry(void) {
    static char message[512];
    fr_error err;
    fr_session session;
    int status = fr_session_open("test/fixtures/plugin-deps-e2e/daukle.toml", 1, &session, &err);
    snprintf(message, sizeof message, "%s", status == FR_OK ? "" : err.message);

    int found = 0;
    if (status == FR_OK) {
        found = fr_registry_language(session.registry, "daukle.language/e2e-dependency-marker-274")
                != NULL;
    }

    fr_session_close(&session);

    ASSERT_EQm(message, FR_OK, status);
    ASSERT(found);
    PASS();
}

GREATEST_MAIN_DEFS();


/* The claim D-70 actually makes: a project scaffolded by `daukle init` is one
   daukle can sync. Here rather than in test_cli because the manifest is TOML
   and fr_project_parse reads the JSON the toml reader produces, so only a real
   sync exercises the path a user takes. */
TEST a_scaffolded_project_syncs(void) {
    char root[512];
    snprintf(root, sizeof root, "%s/daukle_test_init_%d", fr_test_temp_base(), fr_test_process_id());
    fr_test_remove_tree(root);
    fr_test_make_directory(root);

    char text[1024];
    ASSERT(fr_cli_init_manifest("me/scaffolded", text, sizeof text));

    char manifest[700];
    snprintf(manifest, sizeof manifest, "%s/daukle.toml", root);
    fr_error err;
    ASSERT_EQ(FR_OK, fr_file_write_text(manifest, text, &err));

    fr_sync_report report;
    int status = fr_sync(manifest, 1, 1, &report, &err);
    if (status != FR_OK) fprintf(stderr, "a_scaffolded_project_syncs: %s\n", err.message);
    ASSERT_EQ(FR_OK, status);
    /* A manifest declaring no plugins has nothing to generate, and a sync that
       wrote something here would mean init had scaffolded more than it says. */
    ASSERT_EQ((size_t) 0, report.count);
    fr_sync_report_free(&report);
    fr_test_remove_tree(root);
    PASS();
}

int main(int argc, char **argv) {
    self_path = argv[0];
    if (argc >= 2 && strcmp(argv[1], "--task-child") == 0) {
        return run_as_task_child(argc >= 3 ? argv[2] : "ran-here.txt");
    }
    GREATEST_MAIN_BEGIN();
    RUN_TEST(a_scaffolded_project_syncs);
    RUN_TEST(reproduces_both_real_consumers);
    RUN_TEST(the_second_run_changes_nothing);
    RUN_TEST(check_names_every_drifted_consumer);
    RUN_TEST(github_source_matches_path_source);
    RUN_TEST(check_reports_drift_through_the_github_source);
    RUN_TEST(no_cache_bypasses_both_the_read_and_the_write);
    RUN_TEST(the_same_manifest_in_two_formats_writes_the_same_file);
    RUN_TEST(a_task_runs_its_child_in_the_derived_directory);
    RUN_TEST(a_manifest_task_runs_a_program_from_a_real_manifest);
    RUN_TEST(a_publisher_runs_its_child_in_the_from_toolchains_directory);
    RUN_TEST(every_destination_runs_in_the_order_the_manifest_declares);
    RUN_TEST(naming_one_destination_runs_only_that_one);
    RUN_TEST(an_unknown_goal_names_the_plugin_count);
    RUN_TEST(check_runs_no_task);
    RUN_TEST(a_required_modules_return_value_reaches_the_registry);
    GREATEST_MAIN_END();
}
