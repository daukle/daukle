#include "greatest.h"
#include "provision/provision.h"

#include "util/error.h"
#include "http_server.h"
#include "project/region.h"
#include "util/sha256.h"
#include "support.h"
#include "archive/tar.h"
#include "exec/toolreport.h"
#include "project/sync.h"
#include "project/tasks.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

GREATEST_MAIN_DEFS();

#define ARCHIVE_SIZE (FR_TAR_BLOCK * 8)

/* Stat'ing through the module's own probe would let a broken probe agree with
   itself, so this file answers "is the tree there" on its own. */
static int directory_exists(const char *path) {
#ifdef _WIN32
    DWORD attributes = GetFileAttributesA(path);
    return attributes != INVALID_FILE_ATTRIBUTES
        && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    struct stat info;
    return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
#endif
}

/* Every test provisions into a cache of its own, or it would pass against a
   tree an earlier test unpacked and prove nothing about its own run. */
static void use_private_cache(const char *name, char *out, size_t size) {
    snprintf(out, size, "%s/provision-%s-%d", fr_test_temp_base(), name, fr_test_process_id());
    fr_test_remove_tree(out);
    fr_test_make_directory(out);
    fr_test_set_env("DAUKLE_CACHE_DIR", out);
}

static size_t build_archive(char *buffer, size_t size) {
    memset(buffer, 0, size);
    size_t used = fr_test_tar_append(buffer, 0, "bin/tool", '0', "payload", 7);
    return fr_test_tar_end(buffer, used);
}

static int member_reads_back(const char *root) {
    char member[2048];
    snprintf(member, sizeof member, "%s/bin/tool", root);

    fr_error err;
    err.message[0] = '\0';
    char *content = NULL;
    size_t length = 0;
    if (fr_file_read_bytes(member, &content, &length, &err) != FR_OK) return 0;

    int correct = length == 7 && memcmp(content, "payload", 7) == 0;
    free(content);
    return correct;
}

TEST a_pinned_archive_is_fetched_verified_and_unpacked(void) {
    static char message[512];
    char cache[1024];
    use_private_cache("fetch", cache, sizeof cache);

    char archive[ARCHIVE_SIZE];
    size_t length = build_archive(archive, sizeof archive);
    char pin[65];
    fr_sha256_hex(archive, length, pin);

    fr_test_server *server = fr_test_server_create();
    fr_test_server_add_body_bytes(server, "/toolchain.tar", archive, length);
    fr_test_server_start(server);

    char url[256];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/toolchain.tar", fr_test_server_port(server));

    fr_provision_result result;
    fr_error err;
    err.message[0] = '\0';
    int provisioned = fr_provision(url, pin, &result, &err);
    fr_test_server_stop(server);
    fr_test_server_free(server);

    int unpacked = member_reads_back(result.root);
    size_t files = result.unpack.files_written;
    int was_cached = result.was_cached;
    snprintf(message, sizeof message, "result %d, cached %d, root \"%s\", err \"%s\"", provisioned,
             was_cached, result.root, err.message);
    fr_test_remove_tree(cache);

    ASSERT_EQm(message, FR_OK, provisioned);
    ASSERT_EQm(message, 0, was_cached);
    ASSERT_EQm(message, 1u, files);
    ASSERTm(message, unpacked);
    PASS();
}

TEST the_second_provision_makes_no_request_at_all(void) {
    static char message[512];
    /* The server is stopped between the two calls, which is a stronger
       assertion than a request counter: a counter still passes when the
       request is made and its answer discarded. */
    char cache[1024];
    use_private_cache("cached", cache, sizeof cache);

    char archive[ARCHIVE_SIZE];
    size_t length = build_archive(archive, sizeof archive);
    char pin[65];
    fr_sha256_hex(archive, length, pin);

    fr_test_server *server = fr_test_server_create();
    fr_test_server_add_body_bytes(server, "/toolchain.tar", archive, length);
    fr_test_server_start(server);

    char url[256];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/toolchain.tar", fr_test_server_port(server));

    fr_provision_result first;
    fr_error err;
    err.message[0] = '\0';
    int once = fr_provision(url, pin, &first, &err);

    fr_test_server_stop(server);
    fr_test_server_free(server);

    fr_provision_result second;
    fr_error again_err;
    again_err.message[0] = '\0';
    int again = fr_provision(url, pin, &second, &again_err);

    int same_root = strcmp(first.root, second.root) == 0;
    int unpacked = member_reads_back(second.root);
    snprintf(message, sizeof message, "once %d (%s), again %d (%s), roots \"%s\" and \"%s\"", once,
             err.message, again, again_err.message, first.root, second.root);
    fr_test_remove_tree(cache);

    ASSERT_EQm(message, FR_OK, once);
    ASSERT_EQm(message, FR_OK, again);
    ASSERT_EQm(message, 0, second.was_cached == 0);
    ASSERTm(message, same_root);
    ASSERTm(message, unpacked);
    PASS();
}

TEST a_wrong_digest_is_refused_and_leaves_nothing_behind(void) {
    static char message[512];
    /* A failed pin that leaves an unpacked tree, or a half-written temporary
       one, is a cache poisoned for every later run. */
    char cache[1024];
    use_private_cache("wrong", cache, sizeof cache);

    char archive[ARCHIVE_SIZE];
    size_t length = build_archive(archive, sizeof archive);
    char actual[65];
    fr_sha256_hex(archive, length, actual);

    char pinned[65];
    memcpy(pinned, actual, sizeof pinned);
    pinned[0] = actual[0] == 'a' ? 'b' : 'a';

    fr_test_server *server = fr_test_server_create();
    fr_test_server_add_body_bytes(server, "/toolchain.tar", archive, length);
    fr_test_server_start(server);

    char url[256];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/toolchain.tar", fr_test_server_port(server));

    fr_provision_result result;
    fr_error err;
    err.message[0] = '\0';
    int provisioned = fr_provision(url, pinned, &result, &err);
    fr_test_server_stop(server);
    fr_test_server_free(server);

    char toolchains[1024];
    snprintf(toolchains, sizeof toolchains, "%s/toolchains", cache);

    fr_error path_err;
    path_err.message[0] = '\0';
    char pinned_root[1024];
    char actual_root[1024];
    fr_provision_root_path(pinned, pinned_root, sizeof pinned_root, &path_err);
    fr_provision_root_path(actual, actual_root, sizeof actual_root, &path_err);

    int pinned_exists = directory_exists(pinned_root);
    int actual_exists = directory_exists(actual_root);
    int downloads_left = fr_test_count_files(toolchains, "archive");
    int names_both = strstr(err.message, pinned) != NULL && strstr(err.message, actual) != NULL;
    snprintf(message, sizeof message, "result %d, err \"%s\"", provisioned, err.message);
    fr_test_remove_tree(cache);

    ASSERT_EQm(message, FR_ERR, provisioned);
    ASSERTm(message, names_both);
    ASSERT_EQm("a refused pin must leave no tree under the digest it claimed", 0, pinned_exists);
    ASSERT_EQm("a refused pin must leave no tree under the digest it had", 0, actual_exists);
    ASSERT_EQm("a refused pin must leave no partial tree behind", 0, downloads_left);
    PASS();
}

TEST a_partial_directory_is_not_mistaken_for_a_provisioned_tree(void) {
    static char message[512];
    /* A probe that globbed rather than stat'ing the exact digest directory
       would hand back this half-finished tree as if it were provisioned. */
    char cache[1024];
    use_private_cache("partial", cache, sizeof cache);

    char toolchains[1024];
    snprintf(toolchains, sizeof toolchains, "%s/toolchains", cache);
    fr_test_make_directory(toolchains);

    char partial[1024];
    snprintf(partial, sizeof partial, "%s/.partial-999-0", toolchains);
    fr_test_make_directory(partial);

    char stale[1024];
    snprintf(stale, sizeof stale, "%s/leftover.txt", partial);
    fr_error write_err;
    write_err.message[0] = '\0';
    fr_file_write_text(stale, "half a run", &write_err);

    char archive[ARCHIVE_SIZE];
    size_t length = build_archive(archive, sizeof archive);
    char pin[65];
    fr_sha256_hex(archive, length, pin);

    fr_test_server *server = fr_test_server_create();
    fr_test_server_add_body_bytes(server, "/toolchain.tar", archive, length);
    fr_test_server_start(server);

    char url[256];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/toolchain.tar", fr_test_server_port(server));

    fr_provision_result result;
    fr_error err;
    err.message[0] = '\0';
    int provisioned = fr_provision(url, pin, &result, &err);
    fr_test_server_stop(server);
    fr_test_server_free(server);

    char expected_root[1024];
    fr_error path_err;
    path_err.message[0] = '\0';
    fr_provision_root_path(pin, expected_root, sizeof expected_root, &path_err);

    int unpacked = member_reads_back(result.root);
    int right_root = strcmp(result.root, expected_root) == 0;
    int was_cached = result.was_cached;
    snprintf(message, sizeof message, "result %d, cached %d, root \"%s\", err \"%s\"", provisioned,
             was_cached, result.root, err.message);
    fr_test_remove_tree(cache);

    ASSERT_EQm(message, FR_OK, provisioned);
    ASSERT_EQm(message, 0, was_cached);
    ASSERTm(message, right_root);
    ASSERTm(message, unpacked);
    PASS();
}

TEST two_urls_serving_identical_bytes_share_one_tree(void) {
    static char message[512];
    /* Keyed on what the bytes are, never on where they came from: a path keyed
       on the url would unpack the same toolchain once per mirror. */
    char cache[1024];
    use_private_cache("shared", cache, sizeof cache);

    char archive[ARCHIVE_SIZE];
    size_t length = build_archive(archive, sizeof archive);
    char pin[65];
    fr_sha256_hex(archive, length, pin);

    fr_test_server *server = fr_test_server_create();
    fr_test_server_add_body_bytes(server, "/a", archive, length);
    fr_test_server_add_body_bytes(server, "/b", archive, length);
    fr_test_server_start(server);

    int port = fr_test_server_port(server);
    char first_url[256];
    char second_url[256];
    snprintf(first_url, sizeof first_url, "http://127.0.0.1:%d/a", port);
    snprintf(second_url, sizeof second_url, "http://127.0.0.1:%d/b", port);

    fr_provision_result first;
    fr_provision_result second;
    fr_error err;
    err.message[0] = '\0';
    int once = fr_provision(first_url, pin, &first, &err);
    int twice = fr_provision(second_url, pin, &second, &err);

    fr_test_server_stop(server);
    int second_was_requested = fr_test_server_was_requested(server, "/b");
    fr_test_server_free(server);

    int same_root = strcmp(first.root, second.root) == 0;
    snprintf(message, sizeof message, "once %d, twice %d, roots \"%s\" and \"%s\", err \"%s\"",
             once, twice, first.root, second.root, err.message);
    fr_test_remove_tree(cache);

    ASSERT_EQm(message, FR_OK, once);
    ASSERT_EQm(message, FR_OK, twice);
    ASSERT_EQm(message, 0, second.was_cached == 0);
    ASSERTm(message, same_root);
    ASSERT_EQm("the second url must never have been asked for", 0, second_was_requested);
    PASS();
}

TEST a_digest_that_is_not_64_hex_characters_is_refused(void) {
    static char message[512];
    /* Not a length check. "g" is the character that separates a digest from a
       64-character string, and asserting only on length is the defect this
       repository has already shipped once. */
    char cache[1024];
    use_private_cache("digest", cache, sizeof cache);

    char non_hex[65];
    memset(non_hex, 'a', 64);
    non_hex[64] = '\0';
    non_hex[10] = 'g';

    char too_short[64];
    memset(too_short, 'a', 63);
    too_short[63] = '\0';

    char too_long[66];
    memset(too_long, 'a', 65);
    too_long[65] = '\0';

    char root[1024];
    fr_error err;
    err.message[0] = '\0';
    int non_hex_path = fr_provision_root_path(non_hex, root, sizeof root, &err);
    int short_path = fr_provision_root_path(too_short, root, sizeof root, &err);
    int long_path = fr_provision_root_path(too_long, root, sizeof root, &err);
    int empty_path = fr_provision_root_path("", root, sizeof root, &err);

    fr_provision_result result;
    fr_error provision_err;
    provision_err.message[0] = '\0';
    int provisioned = fr_provision("http://127.0.0.1:1/never-asked", non_hex, &result,
                                   &provision_err);
    int named_the_digest = strstr(provision_err.message, "hexadecimal") != NULL;

    snprintf(message, sizeof message, "paths %d %d %d %d, provision %d (\"%s\")", non_hex_path,
             short_path, long_path, empty_path, provisioned, provision_err.message);
    fr_test_remove_tree(cache);

    ASSERT_EQm(message, FR_ERR, non_hex_path);
    ASSERT_EQm(message, FR_ERR, short_path);
    ASSERT_EQm(message, FR_ERR, long_path);
    ASSERT_EQm(message, FR_ERR, empty_path);
    ASSERT_EQm(message, FR_ERR, provisioned);
    ASSERTm(message, named_the_digest);
    PASS();
}

TEST a_label_holding_a_control_character_is_refused(void) {
    static char message[256];
    ASSERTm("a plain label is safe", fr_toolreport_label_is_safe("temurin 21.0.5"));
    ASSERTm(message, !fr_toolreport_label_is_safe("temurin\x1b[2K21"));
    ASSERTm(message, !fr_toolreport_label_is_safe("a\nb"));
    ASSERTm(message, !fr_toolreport_label_is_safe("a\rb"));
    ASSERTm(message, !fr_toolreport_label_is_safe("a\tb"));
    PASS();
}

/* Three distinct tools and one of them reported twice: the repeat is what
   "one line" bounds and the three distinct digests are what "per distinct tool"
   means, so a stub that ignored the key would fail the second assertion. */
TEST one_line_per_distinct_tool_per_run(void) {
    static char message[256];
    fr_toolreport_reset();

    const char *first_url = "https://example.invalid/toolchains/temurin-21.tar.gz";
    const char *second_url = "https://example.invalid/toolchains/cmake-3.29.tar.gz";
    const char *third_url = "https://example.invalid/toolchains/ninja-1.12.zip";
    const char *first = "1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcd";
    const char *second = "2234567890abcdef1234567890abcdef1234567890abcdef1234567890abcd";
    const char *third = "3234567890abcdef1234567890abcdef1234567890abcdef1234567890abcd";

    fr_toolreport_provisioned(NULL, first_url, first, 1);
    fr_toolreport_provisioned(NULL, second_url, second, 0);
    fr_toolreport_provisioned(NULL, third_url, third, 1);
    fr_toolreport_provisioned(NULL, first_url, first, 1);

    size_t count = fr_toolreport_row_count();
    const fr_toolreport_row *row_one = fr_toolreport_row_at(0);
    const fr_toolreport_row *row_two = fr_toolreport_row_at(1);
    const fr_toolreport_row *row_three = fr_toolreport_row_at(2);
    snprintf(message, sizeof message, "row_count %zu", count);

    ASSERT_EQm(message, 3u, count);
    ASSERTm(message, row_one != NULL && strcmp(row_one->digest, first) == 0);
    ASSERTm(message, row_two != NULL && strcmp(row_two->digest, second) == 0);
    ASSERTm(message, row_three != NULL && strcmp(row_three->digest, third) == 0);
    PASS();
}

/* Spec 7.4 requires a skipped link to be named, and D-6 forbids a silent
   degradation: the count fr_unpack fills has to reach the row, or a toolchain
   arrives missing links it shipped with and nothing says so. */
TEST a_skipped_symlink_reaches_the_report_by_name(void) {
    static char message[512];
    fr_toolreport_reset();

    const char *url = "https://example.invalid/toolchains/temurin-21.tar.gz";
    const char *digest = "4234567890abcdef1234567890abcdef1234567890abcdef1234567890abcd";
    const char *quiet = "5234567890abcdef1234567890abcdef1234567890abcdef1234567890abcd";

    fr_toolreport_provisioned(NULL, url, digest, 0);
    fr_toolreport_symlinks_skipped(digest, 3, "bin/java");

    fr_toolreport_provisioned(NULL, url, quiet, 0);
    fr_toolreport_symlinks_skipped(quiet, 0, "");

    const fr_toolreport_row *skipped = fr_toolreport_row_at(0);
    const fr_toolreport_row *clean = fr_toolreport_row_at(1);
    snprintf(message, sizeof message, "skipped %zu \"%s\", clean %zu \"%s\"",
             skipped == NULL ? 0u : skipped->symlinks_skipped,
             skipped == NULL ? "" : skipped->first_symlink_skipped,
             clean == NULL ? 0u : clean->symlinks_skipped,
             clean == NULL ? "" : clean->first_symlink_skipped);

    ASSERTm(message, skipped != NULL);
    ASSERTm(message, clean != NULL);
    ASSERT_EQm(message, 3u, skipped->symlinks_skipped);
    ASSERTm(message, strcmp(skipped->first_symlink_skipped, "bin/java") == 0);
    ASSERT_EQm(message, 0u, clean->symlinks_skipped);
    ASSERTm(message, clean->first_symlink_skipped[0] == '\0');
    PASS();
}

TEST a_missing_label_falls_back_to_a_fact_rather_than_to_nothing(void) {
    static char message[512];
    fr_toolreport_reset();

    fr_toolreport_used_installed("gcc", NULL, "/usr/bin/gcc");
    const char *url = "https://example.invalid/toolchains/temurin-21.tar.gz";
    const char *digest = "abcdef1234567890abcdef1234567890abcdef1234567890abcdef12345678";
    fr_toolreport_provisioned(NULL, url, digest, 0);

    const fr_toolreport_row *installed = fr_toolreport_row_at(0);
    const fr_toolreport_row *provisioned = fr_toolreport_row_at(1);
    snprintf(message, sizeof message, "installed label \"%s\", provisioned label \"%s\"",
             installed == NULL ? "" : installed->label,
             provisioned == NULL ? "" : provisioned->label);

    ASSERTm(message, installed != NULL);
    ASSERTm(message, provisioned != NULL);
    ASSERTm(message, strcmp(installed->label, "gcc") == 0);
    ASSERTm(message, strcmp(provisioned->label, "temurin-21.tar.gz") == 0);
    PASS();
}

TEST the_row_keeps_the_url_and_digest_whatever_the_label_says(void) {
    static char message[512];
    fr_toolreport_reset();

    /* Longer than the label field, so a bug that let the label buffer bleed
       into the url field could not pass by accident. */
    const char *url = "https://example.invalid/dist/toolchains/gcc-14.1.0/"
                      "gcc-14.1.0-x86_64-linux-gnu-full-static-toolchain-with-debug-symbols-"
                      "and-extra-target-libraries-included.tar.gz";
    const char *digest = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcd";
    const char *lying_label = "system gcc 14.1.0";

    fr_toolreport_provisioned(lying_label, url, digest, 0);

    size_t count = fr_toolreport_row_count();
    const fr_toolreport_row *row = fr_toolreport_row_at(0);
    snprintf(message, sizeof message, "count %zu, label \"%s\", url \"%s\", digest \"%s\"", count,
             row == NULL ? "" : row->label, row == NULL ? "" : row->url,
             row == NULL ? "" : row->digest);

    ASSERT_EQm(message, 1u, count);
    ASSERTm(message, row != NULL);
    ASSERTm(message, strcmp(row->label, lying_label) == 0);
    ASSERTm(message, strcmp(row->url, url) == 0);
    ASSERTm(message, strcmp(row->digest, digest) == 0);
    PASS();
}

TEST an_empty_label_falls_back_to_a_fact_rather_than_to_nothing(void) {
    static char message[512];
    fr_toolreport_reset();

    fr_toolreport_used_installed("gcc", "", "/usr/bin/gcc");

    const fr_toolreport_row *installed = fr_toolreport_row_at(0);
    snprintf(message, sizeof message, "installed label \"%s\"",
             installed == NULL ? "" : installed->label);

    ASSERTm(message, installed != NULL);
    ASSERTm(message, strcmp(installed->label, "gcc") == 0);
    PASS();
}

TEST a_url_ending_in_a_slash_still_falls_back_to_a_fact(void) {
    static char message[512];
    fr_toolreport_reset();

    const char *url = "https://example.invalid/toolchains/temurin-21/";
    const char *digest = "fedcba0987654321fedcba0987654321fedcba0987654321fedcba09876543";
    fr_toolreport_provisioned(NULL, url, digest, 0);

    const fr_toolreport_row *row = fr_toolreport_row_at(0);
    snprintf(message, sizeof message, "provisioned label \"%s\"", row == NULL ? "" : row->label);

    ASSERTm(message, row != NULL);
    ASSERTm(message, row->label[0] != '\0');
    ASSERTm(message, strcmp(row->label, "temurin-21") == 0);
    PASS();
}

/* argv[0] as ctest invokes it: this test binary's own bytes are what get
   tarred up and provisioned below, so the "tool" the fixture plugin execs is
   this same process, re-launched with a flag that makes it write a sentinel
   instead of running the suite. */
static const char *self_path;

static int file_exists(const char *path) {
    FILE *handle = fopen(path, "r");
    if (handle == NULL) return 0;
    fclose(handle);
    return 1;
}

#define PROVISIONED_SENTINEL "provisioned-ran-here.txt"

static int run_as_task_child(void) {
    FILE *marker = fopen(PROVISIONED_SENTINEL, "w");
    if (marker == NULL) return 1;
    fputs("ok", marker);
    fclose(marker);
    return 0;
}

/* Saves whatever DAUKLE_CACHE_DIR held before, so the test can put it back:
   an empty saved value means there was none, and unsetting is what restores
   that. */
static void use_isolated_cache_dir(const char *directory, char *saved, size_t saved_size) {
    const char *previous = getenv("DAUKLE_CACHE_DIR");
    snprintf(saved, saved_size, "%s", previous != NULL ? previous : "");
    fr_test_remove_tree(directory);
    fr_test_make_directory(directory);
    fr_test_set_env("DAUKLE_CACHE_DIR", directory);
}

static void restore_cache_dir(const char *saved) {
    fr_test_set_env("DAUKLE_CACHE_DIR", saved[0] != '\0' ? saved : NULL);
}

/* fr_test_tar_append writes mode 0644, and a provisioned member that is not
   executable is refused. On Windows executability is not a permission bit, so
   only POSIX ever reads this. */
static void mark_tar_member_executable(char *archive, size_t header_offset) {
    memcpy(archive + header_offset + 100, "0000755", 8);
    fr_test_tar_fix_checksum(archive, header_offset);
}

/* The headline case of the whole branch: a repository holding daukle.toml, a
   plugin and nothing daukle itself needs, which provisions a program from a
   pinned archive and runs it. The tar member is this test binary's own bytes,
   read back and hashed at runtime, so nothing here hardcodes a digest of
   anything on disk. The only assertion that proves the provisioned binary
   actually ran, rather than that daukle.provision merely returned FR_OK, is
   that the sentinel file the child writes appeared. */
TEST a_project_provisions_a_tool_and_runs_it(void) {
    static char message[1024];

    char cache[1024];
    char saved_cache[1024];
    snprintf(cache, sizeof cache, "%s/provision-e2e-%d", fr_test_temp_base(),
             fr_test_process_id());
    use_isolated_cache_dir(cache, saved_cache, sizeof saved_cache);

    fr_error read_err;
    read_err.message[0] = '\0';
    char *self_bytes = NULL;
    size_t self_length = 0;
    int self_read = fr_file_read_bytes(self_path, &self_bytes, &self_length, &read_err) == FR_OK;

    size_t archive_capacity = self_read
        ? FR_TAR_BLOCK + ((self_length + FR_TAR_BLOCK - 1) / FR_TAR_BLOCK) * FR_TAR_BLOCK
              + 2 * FR_TAR_BLOCK
        : 0;
    char *archive = self_read ? malloc(archive_capacity) : NULL;
    size_t archive_length = 0;
    if (archive != NULL) {
        memset(archive, 0, archive_capacity);
        size_t used = fr_test_tar_append(archive, 0, "bin/tool.exe", '0', self_bytes, self_length);
        mark_tar_member_executable(archive, 0);
        archive_length = fr_test_tar_end(archive, used);
    }

    char digest[65];
    digest[0] = '\0';
    if (archive != NULL) fr_sha256_hex(archive, archive_length, digest);

    fr_test_server *server = archive != NULL ? fr_test_server_create() : NULL;
    if (server != NULL) {
        fr_test_server_add_body_bytes(server, "/toolchain.tar", archive, archive_length);
        fr_test_server_start(server);
    }

    char url[256];
    url[0] = '\0';
    if (server != NULL) {
        snprintf(url, sizeof url, "http://127.0.0.1:%d/toolchain.tar", fr_test_server_port(server));
    }

    fr_test_set_env("DAUKLE_TEST_ARCHIVE_URL", url);
    fr_test_set_env("DAUKLE_TEST_ARCHIVE_SHA256", digest);
    fr_test_set_env("DAUKLE_TEST_MEMBER", "bin/tool.exe");

    const char *marker =
        "test/fixtures/provisioned-tool/build/daukle/provisioner/" PROVISIONED_SENTINEL;
    remove(marker);

    fr_error err;
    err.message[0] = '\0';
    fr_session session;
    int opened = fr_session_open("test/fixtures/provisioned-tool/daukle.toml", 1, &session, &err);

    fr_sync_report sync_report;
    int synced = FR_ERR;
    if (opened == FR_OK) {
        synced = fr_sync_session(&session, 1, &sync_report, &err);
        if (synced == FR_OK) fr_sync_report_free(&sync_report);
    }

    fr_task_set set;
    int collected = FR_ERR;
    if (synced == FR_OK) {
        collected = fr_tasks_collect(session.registry, &session.manifest, &set, &err);
    }

    fr_task_plan plan;
    int planned = FR_ERR;
    if (collected == FR_OK) planned = fr_tasks_plan(&set, "provisioner:run", &plan, &err);

    int run_status = FR_ERR;
    if (planned == FR_OK) run_status = fr_tasks_run(&plan, &session, &err);

    int ran_here = file_exists(marker);

    if (planned == FR_OK) fr_tasks_plan_free(&plan);
    if (collected == FR_OK) fr_tasks_set_free(&set);
    if (opened == FR_OK) fr_session_close(&session);

    if (server != NULL) {
        fr_test_server_stop(server);
        fr_test_server_free(server);
    }
    free(archive);
    free(self_bytes);
    remove(marker);
    restore_cache_dir(saved_cache);

    snprintf(message, sizeof message,
             "self_read %d, opened %d, synced %d, collected %d, planned %d, run %d, err \"%s\"",
             self_read, opened, synced, collected, planned, run_status, err.message);

    ASSERT_EQm(message, 1, self_read);
    ASSERT_EQm(message, FR_OK, opened);
    ASSERT_EQm(message, FR_OK, synced);
    ASSERT_EQm(message, FR_OK, collected);
    ASSERT_EQm(message, FR_OK, planned);
    ASSERT_EQm(message, FR_OK, run_status);
    ASSERTm(message, ran_here);
    PASS();
}

int main(int argc, char **argv) {
    self_path = argv[0];
    if (argc == 2 && strcmp(argv[1], "--task-child") == 0) return run_as_task_child();
    GREATEST_MAIN_BEGIN();
    RUN_TEST(a_pinned_archive_is_fetched_verified_and_unpacked);
    RUN_TEST(the_second_provision_makes_no_request_at_all);
    RUN_TEST(a_wrong_digest_is_refused_and_leaves_nothing_behind);
    RUN_TEST(a_partial_directory_is_not_mistaken_for_a_provisioned_tree);
    RUN_TEST(two_urls_serving_identical_bytes_share_one_tree);
    RUN_TEST(a_digest_that_is_not_64_hex_characters_is_refused);
    RUN_TEST(a_label_holding_a_control_character_is_refused);
    RUN_TEST(one_line_per_distinct_tool_per_run);
    RUN_TEST(a_skipped_symlink_reaches_the_report_by_name);
    RUN_TEST(a_missing_label_falls_back_to_a_fact_rather_than_to_nothing);
    RUN_TEST(the_row_keeps_the_url_and_digest_whatever_the_label_says);
    RUN_TEST(an_empty_label_falls_back_to_a_fact_rather_than_to_nothing);
    RUN_TEST(a_url_ending_in_a_slash_still_falls_back_to_a_fact);
    RUN_TEST(a_project_provisions_a_tool_and_runs_it);
    GREATEST_MAIN_END();
}
