#include "greatest.h"
#include "source_github.h"
#include "support.h"

#include "cJSON.h"
#include "http.h"
#include "manifest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The plugin frees its url string once fr_http_get returns, so the stub
   copies it rather than keeping the caller's pointer. */
static char LAST_URL_BUF[512];
static const char *LAST_URL = NULL;
static int BACKEND_CALLS = 0;
static fr_http_fn ORIGINAL_BACKEND = NULL;

static int stub_get(const char *url, const fr_http_header *headers, size_t header_count,
                    char **out_body, size_t *out_length, fr_error *err) {
    (void) headers; (void) header_count; (void) err;
    snprintf(LAST_URL_BUF, sizeof LAST_URL_BUF, "%s", url);
    LAST_URL = LAST_URL_BUF;
    BACKEND_CALLS++;
    const char *body = "{\"schema\":1,\"project\":\"forebay/basekit\",\"version\":\"5.0.0\",\"modules\":{}}";
    *out_length = strlen(body);
    *out_body = malloc(*out_length + 1);
    memcpy(*out_body, body, *out_length + 1);
    return FR_OK;
}

/* Unique per process (not just per test) so a crash mid-test or a second CI job
   running this binary concurrently on the same machine can never share, and thus
   never contend over, another run's cache entries. */
static const char *test_cache_root(void) {
    static char root[512];
    snprintf(root, sizeof root, "%s/ferrule_test_source_github_cache_%d",
             fr_test_temp_base(), fr_test_process_id());
    return root;
}

/* Every test points the cache at a private temp root and installs the stub
   backend before touching FR_SOURCE_GITHUB, so no test can reach a real
   cache directory or the network. */
static void isolate_environment(void) {
    fr_test_set_env("FERRULE_CACHE_DIR", test_cache_root());
    ORIGINAL_BACKEND = fr_http_set_backend(stub_get);
}

static void restore_environment(void) {
    fr_test_set_env("FERRULE_CACHE_DIR", NULL);
    fr_http_set_backend(ORIGINAL_BACKEND);
}

/* Tests share one cache key, so every written entry must be gone before the
   next test runs or that test would see a hit instead of exercising a fetch. */
static void empty_the_cache(void) {
    fr_test_remove_tree(test_cache_root());
}

TEST builds_the_release_asset_url_from_the_block(void) {
    isolate_environment();
    empty_the_cache();

    const char *json =
        "{\"kind\":\"github-releases\",\"repo\":\"forebay/basekit\","
        "\"version\":\"5.0.0\"}";
    cJSON *block = cJSON_Parse(json);
    fr_project project;
    fr_error err;
    ASSERT_EQ(FR_OK, FR_SOURCE_GITHUB.load(FR_SOURCE_GITHUB.state, "forebay/basekit",
                                           block, ".", &project, &err));
    ASSERT_STR_EQ("https://github.com/forebay/basekit/releases/download/5.0.0/ferrule.json",
                  LAST_URL);
    fr_project_free(&project);
    cJSON_Delete(block);

    empty_the_cache();
    restore_environment();
    PASS();
}

TEST substitutes_the_version_into_a_tag_template(void) {
    isolate_environment();
    empty_the_cache();

    const char *json =
        "{\"kind\":\"github-releases\",\"repo\":\"forebay/basekit\","
        "\"version\":\"5.0.0\",\"tag\":\"v{version}\"}";
    cJSON *block = cJSON_Parse(json);
    fr_project project;
    fr_error err;
    ASSERT_EQ(FR_OK, FR_SOURCE_GITHUB.load(FR_SOURCE_GITHUB.state, "forebay/basekit",
                                           block, ".", &project, &err));
    ASSERT(strstr(LAST_URL, "/releases/download/v5.0.0/") != NULL);
    fr_project_free(&project);
    cJSON_Delete(block);

    empty_the_cache();
    restore_environment();
    PASS();
}

TEST serves_a_second_load_from_the_cache(void) {
    isolate_environment();
    empty_the_cache();

    const char *json =
        "{\"kind\":\"github-releases\",\"repo\":\"forebay/basekit\","
        "\"version\":\"5.0.0\"}";
    cJSON *block = cJSON_Parse(json);
    fr_project first;
    fr_project second;
    fr_error err;

    BACKEND_CALLS = 0;
    ASSERT_EQ(FR_OK, FR_SOURCE_GITHUB.load(FR_SOURCE_GITHUB.state, "forebay/basekit",
                                           block, ".", &first, &err));
    ASSERT_EQ(1, BACKEND_CALLS);
    fr_project_free(&first);

    ASSERT_EQ(FR_OK, FR_SOURCE_GITHUB.load(FR_SOURCE_GITHUB.state, "forebay/basekit",
                                           block, ".", &second, &err));
    ASSERT_EQ(1, BACKEND_CALLS);
    ASSERT_STR_EQ("forebay/basekit", second.project);
    fr_project_free(&second);

    cJSON_Delete(block);

    empty_the_cache();
    restore_environment();
    PASS();
}

/* One project id and version resolved from two different repositories: the
   second load must reach its own repository rather than being served the
   first's manifest out of the cache. */
TEST does_not_serve_one_repositorys_manifest_for_another(void) {
    isolate_environment();
    empty_the_cache();

    cJSON *upstream = cJSON_Parse(
        "{\"kind\":\"github-releases\",\"repo\":\"forebay/basekit\",\"version\":\"5.0.0\"}");
    cJSON *fork = cJSON_Parse(
        "{\"kind\":\"github-releases\",\"repo\":\"someone-else/basekit\",\"version\":\"5.0.0\"}");
    fr_project first;
    fr_project second;
    fr_error err;

    BACKEND_CALLS = 0;
    ASSERT_EQ(FR_OK, FR_SOURCE_GITHUB.load(FR_SOURCE_GITHUB.state, "forebay/basekit",
                                           upstream, ".", &first, &err));
    ASSERT_EQ(1, BACKEND_CALLS);
    fr_project_free(&first);

    ASSERT_EQ(FR_OK, FR_SOURCE_GITHUB.load(FR_SOURCE_GITHUB.state, "forebay/basekit",
                                           fork, ".", &second, &err));
    ASSERT_EQ(2, BACKEND_CALLS);
    ASSERT(strstr(LAST_URL, "someone-else/basekit") != NULL);
    fr_project_free(&second);

    cJSON_Delete(upstream);
    cJSON_Delete(fork);

    empty_the_cache();
    restore_environment();
    PASS();
}

TEST reports_a_missing_repo_field_against_the_source_path(void) {
    isolate_environment();

    const char *json = "{\"kind\":\"github-releases\",\"version\":\"5.0.0\"}";
    cJSON *block = cJSON_Parse(json);
    fr_project project;
    fr_error err;

    ASSERT_EQ(FR_ERR, FR_SOURCE_GITHUB.load(FR_SOURCE_GITHUB.state, "forebay/basekit",
                                            block, ".", &project, &err));
    ASSERT(strstr(err.message, "sources.forebay/basekit") != NULL);
    ASSERT(strstr(err.message, "repo") != NULL);

    cJSON_Delete(block);
    restore_environment();
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(builds_the_release_asset_url_from_the_block);
    RUN_TEST(substitutes_the_version_into_a_tag_template);
    RUN_TEST(serves_a_second_load_from_the_cache);
    RUN_TEST(does_not_serve_one_repositorys_manifest_for_another);
    RUN_TEST(reports_a_missing_repo_field_against_the_source_path);
    GREATEST_MAIN_END();
}
