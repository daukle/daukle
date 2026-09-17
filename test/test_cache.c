#include "greatest.h"
#include "cache.h"
#include "support.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *ARTIFACT =
    "https://github.com/forebay/basekit/releases/download/5.0.0/daukle.json";
static const char *FORK_ARTIFACT =
    "https://github.com/someone-else/basekit/releases/download/5.0.0/daukle.json";

/* Unique per process (not just per test) so a crash mid-test or a second CI job
   running this binary concurrently on the same machine can never share, and thus
   never contend over, another run's cache entries. */
static const char *test_cache_root(void) {
    static char root[512];
    snprintf(root, sizeof root, "%s/daukle_test_cache_%d", fr_test_temp_base(), fr_test_process_id());
    return root;
}

/* Every test points the cache at a private temp root before touching
   fr_cache_* so none of them can reach a real user cache directory. */
static void isolate_cache_dir(void) {
    fr_test_set_env("DAUKLE_CACHE_DIR", test_cache_root());
}

static void clear_cache_dir(void) {
    fr_test_set_env("DAUKLE_CACHE_DIR", NULL);
}

/* Only the byte-faithfulness test cares about the length argument; everywhere
   else the text is plain and its length is just its strlen. */
static void write_cache_text(const char *project, const char *version,
                             const char *artifact, const char *text) {
    fr_cache_write(project, version, artifact, text, strlen(text));
}

static char *dup_or_null(const char *text) {
    if (text == NULL) return NULL;
    size_t size = strlen(text) + 1;
    char *copy = malloc(size);
    if (copy != NULL) memcpy(copy, text, size);
    return copy;
}

TEST rejects_a_project_that_would_escape_the_cache_root(void) {
    isolate_cache_dir();

    fr_error err;
    char *path = NULL;
    ASSERT_EQ(FR_ERR, fr_cache_path("../evil", "1.0.0", ARTIFACT, &path, &err));
    ASSERT(strstr(err.message, "..") != NULL);
    ASSERT(path == NULL);

    clear_cache_dir();
    PASS();
}

TEST rejects_a_component_with_a_trailing_dot(void) {
    isolate_cache_dir();

    fr_error err;
    char *path = NULL;
    ASSERT_EQ(FR_ERR, fr_cache_path("forebay/basekit.", "1.0.0", ARTIFACT, &path, &err));
    ASSERT(path == NULL);

    path = NULL;
    ASSERT_EQ(FR_ERR, fr_cache_path("forebay/basekit", "1.0.0.", ARTIFACT, &path, &err));
    ASSERT(path == NULL);

    clear_cache_dir();
    PASS();
}

TEST rejects_an_entry_that_names_no_artifact(void) {
    isolate_cache_dir();

    fr_error err;
    char *path = NULL;
    ASSERT_EQ(FR_ERR, fr_cache_path("forebay/basekit", "1.0.0", NULL, &path, &err));
    ASSERT(strstr(err.message, "artifact") != NULL);
    ASSERT(path == NULL);

    path = NULL;
    ASSERT_EQ(FR_ERR, fr_cache_path("forebay/basekit", "1.0.0", "", &path, &err));
    ASSERT(path == NULL);

    clear_cache_dir();
    PASS();
}

TEST rejects_an_oversized_cache_root_rather_than_truncating_it(void) {
    char oversized[2000];
    memset(oversized, 'x', sizeof oversized - 1);
    oversized[sizeof oversized - 1] = '\0';
    fr_test_set_env("DAUKLE_CACHE_DIR", oversized);

    fr_error err;
    char *path = NULL;
    ASSERT_EQ(FR_ERR, fr_cache_path("forebay/basekit", "1.0.0", ARTIFACT, &path, &err));
    ASSERT(path == NULL);

    clear_cache_dir();
    PASS();
}

TEST reports_a_miss_without_an_error(void) {
    isolate_cache_dir();

    fr_error err;
    char *text = NULL;
    ASSERT_EQ(FR_OK, fr_cache_read("forebay/absent", "9.9.9", ARTIFACT, &text, &err));
    ASSERT(text == NULL);

    clear_cache_dir();
    PASS();
}

TEST round_trips_a_written_manifest(void) {
    isolate_cache_dir();
    const char *root = test_cache_root();

    write_cache_text("forebay/basekit", "5.0.0", ARTIFACT, "{\"schema\":1}");
    fr_error err;
    char *text = NULL;
    ASSERT_EQ(FR_OK, fr_cache_read("forebay/basekit", "5.0.0", ARTIFACT, &text, &err));
    ASSERT(text != NULL);
    ASSERT_STR_EQ("{\"schema\":1}", text);
    free(text);

    /* Proves the successful path leaves no ".tmp" sibling behind: it must
       have been renamed into place, not merely written and abandoned. */
    ASSERT_EQ(0, fr_test_count_files(root, "daukle.json.tmp"));
    ASSERT_EQ(1, fr_test_count_files(root, "daukle.json"));

    fr_test_remove_tree(root);
    clear_cache_dir();
    PASS();
}

/* Two manifests can name one project id and one version and resolve them from
   different repositories. Keyed on the pair alone, the second would be served
   the first's manifest and would render its coordinates, silently and with a
   successful exit. */
TEST keeps_two_artifacts_of_one_project_and_version_apart(void) {
    isolate_cache_dir();
    const char *root = test_cache_root();

    write_cache_text("forebay/basekit", "5.0.0", ARTIFACT, "{\"origin\":\"upstream\"}");
    write_cache_text("forebay/basekit", "5.0.0", FORK_ARTIFACT, "{\"origin\":\"fork\"}");

    fr_error err;
    char *upstream = NULL;
    char *fork = NULL;
    ASSERT_EQ(FR_OK, fr_cache_read("forebay/basekit", "5.0.0", ARTIFACT, &upstream, &err));
    ASSERT_EQ(FR_OK, fr_cache_read("forebay/basekit", "5.0.0", FORK_ARTIFACT, &fork, &err));
    ASSERT(upstream != NULL);
    ASSERT(fork != NULL);
    ASSERT_STR_EQ("{\"origin\":\"upstream\"}", upstream);
    ASSERT_STR_EQ("{\"origin\":\"fork\"}", fork);
    free(upstream);
    free(fork);

    ASSERT_EQ(2, fr_test_count_files(root, "daukle.json"));

    fr_test_remove_tree(root);
    clear_cache_dir();
    PASS();
}

/* A cached entry must be byte-identical to the body that was fetched, so that
   a hit parses exactly as the fetch did. A body carrying a NUL is the case
   that separates writing by length from writing to the first NUL. */
TEST writes_the_whole_body_including_an_embedded_nul(void) {
    isolate_cache_dir();
    const char *root = test_cache_root();

    const char body[] = "{\"a\":1}\0trailing";
    const size_t body_length = sizeof body - 1;
    fr_cache_write("forebay/basekit", "5.0.0", ARTIFACT, body, body_length);

    fr_error err;
    char *path = NULL;
    ASSERT_EQ(FR_OK, fr_cache_path("forebay/basekit", "5.0.0", ARTIFACT, &path, &err));

    FILE *file = fopen(path, "rb");
    ASSERT(file != NULL);
    char written[64];
    size_t read_count = fread(written, 1, sizeof written, file);
    fclose(file);
    free(path);

    ASSERT_EQ((int) body_length, (int) read_count);
    ASSERT_EQ(0, memcmp(body, written, body_length));

    fr_test_remove_tree(root);
    clear_cache_dir();
    PASS();
}

/* Direct proof of the atomicity fix: pre-populate the cache entry with known
   good content, then force the next write's temp file to be un-openable (by
   pre-creating a directory at the exact ".tmp" path fr_cache_write uses) so
   the write cannot complete. The write must fail silently and the original
   content must still be there afterwards, untouched and not truncated. */
TEST preserves_the_existing_manifest_when_a_write_cannot_complete(void) {
    isolate_cache_dir();
    const char *root = test_cache_root();
    const char *project = "forebay/atomic-guard";
    const char *version = "1.0.0";

    write_cache_text(project, version, ARTIFACT, "{\"good\":true}");

    fr_error err;
    char *final_path = NULL;
    ASSERT_EQ(FR_OK, fr_cache_path(project, version, ARTIFACT, &final_path, &err));

    char blocker_path[512];
    snprintf(blocker_path, sizeof blocker_path, "%s.tmp", final_path);
    ASSERT_EQ(0, fr_test_make_directory(blocker_path));

    write_cache_text(project, version, ARTIFACT, "{\"corrupt");

    char *text = NULL;
    ASSERT_EQ(FR_OK, fr_cache_read(project, version, ARTIFACT, &text, &err));
    ASSERT(text != NULL);
    ASSERT_STR_EQ("{\"good\":true}", text);
    free(text);

    free(final_path);
    fr_test_remove_tree(root);
    clear_cache_dir();
    PASS();
}

#ifdef _WIN32
TEST resolves_the_localappdata_fallback_when_no_override_is_set(void) {
    clear_cache_dir();
    char *saved = dup_or_null(getenv("LOCALAPPDATA"));

    fr_test_set_env("LOCALAPPDATA", test_cache_root());

    fr_error err;
    char *path = NULL;
    ASSERT_EQ(FR_OK, fr_cache_path("forebay/basekit", "1.0.0", ARTIFACT, &path, &err));
    ASSERT(path != NULL);
    char expected_prefix[600];
    snprintf(expected_prefix, sizeof expected_prefix, "%s/daukle/cache/", test_cache_root());
    ASSERT(strstr(path, expected_prefix) == path);
    free(path);

    fr_test_set_env("LOCALAPPDATA", saved);
    free(saved);
    PASS();
}
#else
TEST resolves_the_xdg_cache_home_fallback_when_no_override_is_set(void) {
    clear_cache_dir();
    char *saved = dup_or_null(getenv("XDG_CACHE_HOME"));

    fr_test_set_env("XDG_CACHE_HOME", test_cache_root());

    fr_error err;
    char *path = NULL;
    ASSERT_EQ(FR_OK, fr_cache_path("forebay/basekit", "1.0.0", ARTIFACT, &path, &err));
    ASSERT(path != NULL);
    char expected_prefix[600];
    snprintf(expected_prefix, sizeof expected_prefix, "%s/daukle/", test_cache_root());
    ASSERT(strstr(path, expected_prefix) == path);
    free(path);

    fr_test_set_env("XDG_CACHE_HOME", saved);
    free(saved);
    PASS();
}
#endif

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(rejects_a_project_that_would_escape_the_cache_root);
    RUN_TEST(rejects_a_component_with_a_trailing_dot);
    RUN_TEST(rejects_an_entry_that_names_no_artifact);
    RUN_TEST(rejects_an_oversized_cache_root_rather_than_truncating_it);
    RUN_TEST(reports_a_miss_without_an_error);
    RUN_TEST(round_trips_a_written_manifest);
    RUN_TEST(keeps_two_artifacts_of_one_project_and_version_apart);
    RUN_TEST(writes_the_whole_body_including_an_embedded_nul);
    RUN_TEST(preserves_the_existing_manifest_when_a_write_cannot_complete);
#ifdef _WIN32
    RUN_TEST(resolves_the_localappdata_fallback_when_no_override_is_set);
#else
    RUN_TEST(resolves_the_xdg_cache_home_fallback_when_no_override_is_set);
#endif
    GREATEST_MAIN_END();
}
