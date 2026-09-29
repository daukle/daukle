#include "greatest.h"
#include "unpack.h"

#include "archive.h"
#include "error.h"
#include "region.h"
#include "support.h"
#include "tar.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

GREATEST_MAIN_DEFS();

static char input_path[1024];

/* fr_archive_open takes a file, so every test here stages its bytes through one.
   The caller closes the archive; main removes the file once. */
static int open_archive_from_bytes(const char *bytes, size_t length, fr_archive **out,
                                   fr_error *err) {
    snprintf(input_path, sizeof input_path, "%s/unpack-input-%d.tar", fr_test_temp_base(),
             fr_test_process_id());

    FILE *file = fopen(input_path, "wb");
    if (file == NULL) {
        fr_error_set(err, "cannot stage the archive bytes");
        return FR_ERR;
    }
    size_t written = fwrite(bytes, 1, length, file);
    fclose(file);
    if (written != length) {
        fr_error_set(err, "cannot stage the archive bytes");
        return FR_ERR;
    }

    return fr_archive_open(input_path, FR_UNPACK_DEFAULTS.max_member_bytes, out, err);
}

/* The prefix the module under test applies, spelled out a second time here so
   the long-path test checks its work rather than reusing it. */
static void to_native_form(const char *joined, char *out, size_t size) {
#ifdef _WIN32
    snprintf(out, size, "\\\\?\\%s", joined);
    for (char *cursor = out + 4; *cursor != '\0'; cursor++) {
        if (*cursor == '/') *cursor = '\\';
    }
#else
    snprintf(out, size, "%s", joined);
#endif
}

static int read_back(const char *joined, char *out, size_t size) {
    char native[2048];
    to_native_form(joined, native, sizeof native);

    FILE *file = fopen(native, "rb");
    if (file == NULL) return -1;
    size_t read = fread(out, 1, size, file);
    fclose(file);
    return (int) read;
}

static void remove_long(const char *joined) {
    char native[2048];
    to_native_form(joined, native, sizeof native);
    remove(native);
}

TEST every_escaping_member_name_is_refused(void) {
    static char message[512];
    /* Each of these is a separate mechanism, not a variation on one:
       ".." climbs, a leading slash is absolute, "c:" is a drive letter that
       fr_lua_sandbox_climbs_out catches before any split, and a backslash is
       a separator on one platform and a legal filename byte on the other. */
    static const char *const NAMES[] = {
        "../escaped.txt", "a/../../escaped.txt", "/etc/passwd", "c:/windows/x",
        "..\\escaped.txt", "a\\b.txt",
    };
    int refused_all = 1;
    for (size_t index = 0; index < sizeof NAMES / sizeof NAMES[0]; index++) {
        if (fr_unpack_name_is_safe(NAMES[index])) {
            snprintf(message, sizeof message, "\"%s\" was accepted", NAMES[index]);
            refused_all = 0;
            break;
        }
    }
    ASSERTm(message, refused_all);
    ASSERTm("an empty name joins to the destination itself and must be refused",
            !fr_unpack_name_is_safe(""));
    ASSERTm("a \".\" component must be refused", !fr_unpack_name_is_safe("bin/./javac"));
    ASSERTm("an ordinary nested name must still be accepted",
            fr_unpack_name_is_safe("bin/javac"));
    ASSERTm("a directory member keeps its trailing separator and must be accepted",
            fr_unpack_name_is_safe("bin/"));
    PASS();
}

TEST an_escaping_member_is_refused_and_writes_nothing(void) {
    static char message[512];
    /* The end-to-end form of the above. The name check alone could be correct
       while the join ignored it, which is the realistic mistake: a guard that
       nothing consults. */
    char destination[1024];
    char sentinel[1024];
    snprintf(destination, sizeof destination, "%s/unpack-escape-%d", fr_test_temp_base(),
             fr_test_process_id());
    snprintf(sentinel, sizeof sentinel, "%s/escaped.txt", fr_test_temp_base());
    remove(sentinel);
    fr_test_make_directory(destination);

    char buffer[FR_TAR_BLOCK * 8];
    memset(buffer, 0, sizeof buffer);
    size_t used = fr_test_tar_append(buffer, 0, "../escaped.txt", '0', "owned", 5);
    fr_test_tar_end(buffer, used);

    fr_archive *archive = NULL;
    fr_error err;
    err.message[0] = '\0';
    int opened = open_archive_from_bytes(buffer, sizeof buffer, &archive, &err);
    fr_unpack_report report;
    int result = opened == FR_OK
               ? fr_unpack(archive, destination, &FR_UNPACK_DEFAULTS, &report, &err)
               : FR_ERR;
    if (archive != NULL) fr_archive_close(archive);

    int escaped_exists = 0;
    FILE *probe = fopen(sentinel, "rb");
    if (probe != NULL) { escaped_exists = 1; fclose(probe); remove(sentinel); }
    fr_test_remove_tree(destination);

    snprintf(message, sizeof message, "opened %d, result %d, err \"%s\"", opened, result,
             err.message);
    ASSERT_EQm(message, FR_OK, opened);
    ASSERT_EQm(message, FR_ERR, result);
    ASSERTm(message, strstr(message, "escaped.txt") != NULL);
    /* Refused by the name check and not by a failed write: on Windows the
       "\\?\" prefix already refuses a ".." component, so an implementation that
       consults nothing still reports FR_ERR and still writes nothing here. */
    ASSERTm(message, strstr(message, "outside the destination") != NULL);
    ASSERT_EQm("a refused member must not have been written", 0, escaped_exists);
    PASS();
}

TEST a_tree_is_written_with_its_directories(void) {
    static char message[512];
    char destination[1024];
    snprintf(destination, sizeof destination, "%s/unpack-tree-%d", fr_test_temp_base(),
             fr_test_process_id());
    fr_test_make_directory(destination);

    char buffer[FR_TAR_BLOCK * 16];
    memset(buffer, 0, sizeof buffer);
    size_t used = fr_test_tar_append(buffer, 0, "bin/deep/tool", '0', "payload", 7);
    used = fr_test_tar_append(buffer, used, "lib/data.txt", '0', "hello", 5);
    fr_test_tar_end(buffer, used);

    fr_archive *archive = NULL;
    fr_error err;
    err.message[0] = '\0';
    int opened = open_archive_from_bytes(buffer, sizeof buffer, &archive, &err);
    fr_unpack_report report;
    memset(&report, 0, sizeof report);
    int result = opened == FR_OK
               ? fr_unpack(archive, destination, &FR_UNPACK_DEFAULTS, &report, &err)
               : FR_ERR;
    if (archive != NULL) fr_archive_close(archive);

    /* Read the nested one back rather than only stat-ing it: a writer that
       creates every parent and then writes an empty file passes a stat. */
    char nested[1024];
    snprintf(nested, sizeof nested, "%s/bin/deep/tool", destination);
    char *content = NULL;
    size_t length = 0;
    int read_result = fr_file_read_bytes(nested, &content, &length, &err);
    int correct = read_result == FR_OK && length == 7 && memcmp(content, "payload", 7) == 0;
    free(content);
    size_t files = report.files_written;
    fr_test_remove_tree(destination);

    snprintf(message, sizeof message, "result %d, files %zu, err \"%s\"", result, files,
             err.message);
    ASSERT_EQm(message, FR_OK, result);
    ASSERT_EQm(message, 2u, files);
    ASSERTm(message, correct);
    PASS();
}

TEST a_member_count_past_the_limit_is_refused_by_name(void) {
    static char message[512];
    char destination[1024];
    snprintf(destination, sizeof destination, "%s/unpack-count-%d", fr_test_temp_base(),
             fr_test_process_id());
    fr_test_make_directory(destination);

    fr_unpack_limits limits = FR_UNPACK_DEFAULTS;
    limits.max_members = 2;

    char buffer[FR_TAR_BLOCK * 16];
    memset(buffer, 0, sizeof buffer);
    size_t used = 0;
    for (int index = 0; index < 3; index++) {
        char name[32];
        snprintf(name, sizeof name, "file%d.txt", index);
        used = fr_test_tar_append(buffer, used, name, '0', "x", 1);
    }
    fr_test_tar_end(buffer, used);

    fr_archive *archive = NULL;
    fr_error err;
    err.message[0] = '\0';
    int opened = open_archive_from_bytes(buffer, sizeof buffer, &archive, &err);
    fr_unpack_report report;
    int result = opened == FR_OK ? fr_unpack(archive, destination, &limits, &report, &err) : FR_ERR;
    if (archive != NULL) fr_archive_close(archive);
    snprintf(message, sizeof message, "%s", err.message);
    fr_test_remove_tree(destination);

    ASSERT_EQm(message, FR_ERR, result);
    /* "more than 2" and not the member's own name: asserting on "file2.txt"
       would pass against an implementation that refused for any other reason. */
    ASSERTm(message, strstr(message, "more than 2") != NULL);
    PASS();
}

TEST a_total_size_past_the_limit_is_refused_by_name(void) {
    static char message[512];
    /* The decompression bomb. A digest pin proves the bytes are the ones
       expected and says nothing about what they expand to, so this guard is
       about a mistake as much as an attack. */
    char destination[1024];
    snprintf(destination, sizeof destination, "%s/unpack-bomb-%d", fr_test_temp_base(),
             fr_test_process_id());
    fr_test_make_directory(destination);

    fr_unpack_limits limits = FR_UNPACK_DEFAULTS;
    limits.max_total_bytes = 1000;

    static char payload[600];
    memset(payload, 'a', sizeof payload);
    char buffer[FR_TAR_BLOCK * 16];
    memset(buffer, 0, sizeof buffer);
    size_t used = fr_test_tar_append(buffer, 0, "one.bin", '0', payload, sizeof payload);
    used = fr_test_tar_append(buffer, used, "two.bin", '0', payload, sizeof payload);
    fr_test_tar_end(buffer, used);

    fr_archive *archive = NULL;
    fr_error err;
    err.message[0] = '\0';
    int opened = open_archive_from_bytes(buffer, sizeof buffer, &archive, &err);
    fr_unpack_report report;
    int result = opened == FR_OK ? fr_unpack(archive, destination, &limits, &report, &err) : FR_ERR;
    if (archive != NULL) fr_archive_close(archive);
    snprintf(message, sizeof message, "%s", err.message);
    fr_test_remove_tree(destination);

    ASSERT_EQm(message, FR_ERR, result);
    ASSERTm(message, strstr(message, "1000") != NULL);
    PASS();
}

TEST a_path_longer_than_the_limit_is_refused_rather_than_truncated(void) {
    static char message[512];
    /* A truncated path is a different file, which is the reason this refuses
       rather than clamps. */
    char destination[1024];
    snprintf(destination, sizeof destination, "%s/unpack-long-%d", fr_test_temp_base(),
             fr_test_process_id());
    fr_test_make_directory(destination);

    fr_unpack_limits limits = FR_UNPACK_DEFAULTS;
    limits.max_path_length = 16;

    char buffer[FR_TAR_BLOCK * 8];
    memset(buffer, 0, sizeof buffer);
    size_t used = fr_test_tar_append(buffer, 0, "a-name-well-past-sixteen.txt", '0', "x", 1);
    fr_test_tar_end(buffer, used);

    fr_archive *archive = NULL;
    fr_error err;
    err.message[0] = '\0';
    int opened = open_archive_from_bytes(buffer, sizeof buffer, &archive, &err);
    fr_unpack_report report;
    int result = opened == FR_OK ? fr_unpack(archive, destination, &limits, &report, &err) : FR_ERR;
    if (archive != NULL) fr_archive_close(archive);

    /* The truncated name must not exist either, which is what separates
       "refused" from "clamped and written somewhere else". */
    char truncated[1024];
    snprintf(truncated, sizeof truncated, "%s/a-name-well-pas", destination);
    int truncated_exists = 0;
    FILE *probe = fopen(truncated, "rb");
    if (probe != NULL) { truncated_exists = 1; fclose(probe); }
    snprintf(message, sizeof message, "%s", err.message);
    fr_test_remove_tree(destination);

    ASSERT_EQm(message, FR_ERR, result);
    ASSERT_EQm("a refused path must not have been written truncated", 0, truncated_exists);
    PASS();
}

TEST a_duplicate_member_name_is_refused(void) {
    static char message[512];
    char destination[1024];
    snprintf(destination, sizeof destination, "%s/unpack-dup-%d", fr_test_temp_base(),
             fr_test_process_id());
    fr_test_make_directory(destination);

    char buffer[FR_TAR_BLOCK * 8];
    memset(buffer, 0, sizeof buffer);
    size_t used = fr_test_tar_append(buffer, 0, "same.txt", '0', "first", 5);
    used = fr_test_tar_append(buffer, used, "same.txt", '0', "second", 6);
    fr_test_tar_end(buffer, used);

    fr_archive *archive = NULL;
    fr_error err;
    err.message[0] = '\0';
    int opened = open_archive_from_bytes(buffer, sizeof buffer, &archive, &err);
    fr_unpack_report report;
    int result = opened == FR_OK
               ? fr_unpack(archive, destination, &FR_UNPACK_DEFAULTS, &report, &err)
               : FR_ERR;
    if (archive != NULL) fr_archive_close(archive);
    snprintf(message, sizeof message, "%s", err.message);
    fr_test_remove_tree(destination);

    ASSERT_EQm(message, FR_ERR, result);
    ASSERTm(message, strstr(message, "twice") != NULL);
    PASS();
}

/* A ustar name field holds 99 bytes, which alone cannot pass 260, so the
   destination is padded to carry the joined path over the limit. */
#define PADDING_LEVELS 5
static const char PADDING_NAME[] = "padding-level-of-thirty-chars";

TEST a_deeply_nested_member_past_the_legacy_path_limit_is_written(void) {
    static char message[512];
    /* Build a member whose joined path exceeds 260 characters and assert it
       is readable afterwards. Without the prefix this fails on Windows and
       passes everywhere else, which is exactly the shape of defect that ships. */
    char root[1024];
    snprintf(root, sizeof root, "%s/unpack-deep-%d", fr_test_temp_base(), fr_test_process_id());
    fr_test_make_directory(root);

    char destination[1024];
    snprintf(destination, sizeof destination, "%s", root);
    for (int level = 0; level < PADDING_LEVELS; level++) {
        size_t used = strlen(destination);
        snprintf(destination + used, sizeof destination - used, "/%s%d", PADDING_NAME, level);
        fr_test_make_directory(destination);
    }

    char member_name[128];
    memcpy(member_name, "deep/", 5);
    memset(member_name + 5, 'n', 80);
    member_name[85] = '\0';

    char buffer[FR_TAR_BLOCK * 8];
    memset(buffer, 0, sizeof buffer);
    size_t used = fr_test_tar_append(buffer, 0, member_name, '0', "far", 3);
    fr_test_tar_end(buffer, used);

    fr_archive *archive = NULL;
    fr_error err;
    err.message[0] = '\0';
    int opened = open_archive_from_bytes(buffer, sizeof buffer, &archive, &err);
    fr_unpack_report report;
    int result = opened == FR_OK
               ? fr_unpack(archive, destination, &FR_UNPACK_DEFAULTS, &report, &err)
               : FR_ERR;
    if (archive != NULL) fr_archive_close(archive);

    char joined[2048];
    snprintf(joined, sizeof joined, "%s/%s", destination, member_name);
    size_t joined_length = strlen(joined);
    char content[16];
    memset(content, 0, sizeof content);
    int read = read_back(joined, content, sizeof content);
    remove_long(joined);
    fr_test_remove_tree(root);

    snprintf(message, sizeof message, "result %d, joined %zu chars, read %d, err \"%s\"", result,
             joined_length, read, err.message);
    ASSERTm(message, joined_length > 260);
    ASSERT_EQm(message, FR_OK, result);
    ASSERT_EQm(message, 3, read);
    ASSERTm(message, memcmp(content, "far", 3) == 0);
    PASS();
}

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(every_escaping_member_name_is_refused);
    RUN_TEST(an_escaping_member_is_refused_and_writes_nothing);
    RUN_TEST(a_tree_is_written_with_its_directories);
    RUN_TEST(a_member_count_past_the_limit_is_refused_by_name);
    RUN_TEST(a_total_size_past_the_limit_is_refused_by_name);
    RUN_TEST(a_path_longer_than_the_limit_is_refused_rather_than_truncated);
    RUN_TEST(a_duplicate_member_name_is_refused);
    RUN_TEST(a_deeply_nested_member_past_the_legacy_path_limit_is_written);
    remove(input_path);
    GREATEST_MAIN_END();
}
