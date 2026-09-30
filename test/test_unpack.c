#include "greatest.h"
#include "archive/unpack.h"

#include "archive/archive.h"
#include "util/error.h"
#include "project/region.h"
#include "support.h"
#include "archive/tar.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <sys/stat.h>
#include <sys/types.h>
#endif

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

/* The first member's executable flag, or -1 when the bytes do not open. */
static int first_member_is_executable(const char *bytes, size_t length) {
    fr_archive *archive = NULL;
    fr_error err;
    err.message[0] = '\0';
    if (open_archive_from_bytes(bytes, length, &archive, &err) != FR_OK) return -1;

    const fr_archive_member *member = NULL;
    int executable = fr_archive_next(archive, &member, &err) == FR_OK && member != NULL
                   ? member->executable
                   : -1;
    fr_archive_close(archive);
    return executable;
}

/* Unpacks an archive holding "lib/real" and a symlink "bin/link" pointing at
   target. Returns fr_unpack's result, fills report, and copies the error. */
static int unpack_one_symlink(const char *target, fr_unpack_report *report, char *out_message,
                              size_t size) {
    char destination[1024];
    snprintf(destination, sizeof destination, "%s/unpack-link-%d", fr_test_temp_base(),
             fr_test_process_id());
    fr_test_remove_tree(destination);
    fr_test_make_directory(destination);

    char buffer[FR_TAR_BLOCK * 16];
    memset(buffer, 0, sizeof buffer);
    size_t used = fr_test_tar_append(buffer, 0, "lib/real", '0', "payload", 7);
    size_t link_offset = used;
    used = fr_test_tar_append(buffer, used, "bin/link", '2', "", 0);
    memcpy(buffer + link_offset + 157, target, strlen(target));
    fr_test_tar_fix_checksum(buffer, link_offset);
    fr_test_tar_end(buffer, used);

    fr_archive *archive = NULL;
    fr_error err;
    err.message[0] = '\0';
    int opened = open_archive_from_bytes(buffer, sizeof buffer, &archive, &err);
    memset(report, 0, sizeof *report);
    int result = opened == FR_OK
               ? fr_unpack(archive, destination, &FR_UNPACK_DEFAULTS, report, &err)
               : FR_ERR;
    if (archive != NULL) fr_archive_close(archive);
    snprintf(out_message, size, "%s", err.message);
    fr_test_remove_tree(destination);
    return result;
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

TEST a_symlink_with_an_absolute_target_is_refused(void) {
    static char message[512];
    /* Refused outright on every platform, not skipped: this is the
       escape-through-a-link case, and it is the one member kind that can turn
       a later innocent-looking member into a write outside the cache. */
    fr_unpack_report report;
    int posix_form = unpack_one_symlink("/etc/passwd", &report, message, sizeof message);
    ASSERT_EQm(message, FR_ERR, posix_form);
    ASSERTm(message, strstr(message, "absolute") != NULL);

    int drive_form = unpack_one_symlink("c:/windows/system32/cmd.exe", &report, message,
                                        sizeof message);
    ASSERT_EQm(message, FR_ERR, drive_form);
    PASS();
}

TEST a_symlink_whose_relative_target_climbs_out_is_refused(void) {
    static char message[512];
    /* "bin/link" -> "../../outside" resolves lexically against "bin/", so it
       leaves the root. The companion assertion below is what makes this a
       containment test rather than a ".." test: "../lib/real" also contains
       "..", stays inside, and MUST be accepted. */
    fr_unpack_report report;
    int climbing = unpack_one_symlink("../../outside", &report, message, sizeof message);
    ASSERT_EQm(message, FR_ERR, climbing);
    ASSERTm(message, strstr(message, "outside") != NULL || strstr(message, "escape") != NULL);

    int contained = unpack_one_symlink("../lib/real", &report, message, sizeof message);
    ASSERT_EQm(message, FR_OK, contained);
    PASS();
}

TEST a_contained_symlink_is_created_on_posix_and_named_on_windows(void) {
    static char message[512];
    /* One test with two platform arms, because the behaviour is deliberately
       different and a test per platform would let the unrun one rot. */
    fr_unpack_report report;
    int result = unpack_one_symlink("../lib/real", &report, message, sizeof message);
    ASSERT_EQm(message, FR_OK, result);
#ifdef _WIN32
    ASSERT_EQm(message, 0u, report.symlinks_created);
    ASSERT_EQm(message, 1u, report.symlinks_skipped);
    ASSERT_STR_EQm(message, "bin/link", report.first_symlink_skipped);
#else
    ASSERT_EQm(message, 1u, report.symlinks_created);
    ASSERT_EQm(message, 0u, report.symlinks_skipped);
#endif
    PASS();
}

/* Three members, each contained when judged on its own: "a/b" -> ".." has
   depth 1 and climbs one, "a/b/c" -> ".." has depth 2 and climbs one, and a
   plain file needs no judgement at all. Written in order they walk the file to
   the cache root's parent, which only a record of the links already created can
   see. The assertion is on daukle's own refusal (ruling 16): Windows creates no
   link, so "nothing appeared" would pass there whatever the code decided. */
TEST a_chain_of_symlinks_cannot_walk_a_later_member_out(void) {
    static char failure[768];
    char enclosing[1024];
    snprintf(enclosing, sizeof enclosing, "%s/unpack-chain-%d", fr_test_temp_base(),
             fr_test_process_id());
    fr_test_remove_tree(enclosing);
    fr_test_make_directory(enclosing);

    char destination[1152];
    snprintf(destination, sizeof destination, "%s/tree", enclosing);
    fr_test_make_directory(destination);

    char buffer[FR_TAR_BLOCK * 16];
    memset(buffer, 0, sizeof buffer);
    size_t first = 0;
    size_t used = fr_test_tar_append(buffer, first, "a/b", '2', "", 0);
    memcpy(buffer + first + 157, "..", 2);
    fr_test_tar_fix_checksum(buffer, first);

    size_t second = used;
    used = fr_test_tar_append(buffer, second, "a/b/c", '2', "", 0);
    memcpy(buffer + second + 157, "..", 2);
    fr_test_tar_fix_checksum(buffer, second);

    used = fr_test_tar_append(buffer, used, "a/b/c/evil", '0', "owned", 5);
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

    char escaped[1152];
    snprintf(escaped, sizeof escaped, "%s/evil", enclosing);
    char probe[16];
    int landed_outside = read_back(escaped, probe, sizeof probe) >= 0;

    int named_the_link = strstr(err.message, "through the symlink") != NULL
                      && strstr(err.message, "a/b") != NULL;
    snprintf(failure, sizeof failure, "opened %d, result %d, outside %d, err \"%s\"", opened,
             result, landed_outside, err.message);
    fr_test_remove_tree(enclosing);

    ASSERT_EQm(failure, FR_OK, opened);
    ASSERT_EQm(failure, FR_ERR, result);
    ASSERTm(failure, named_the_link);
    ASSERTm(failure, !landed_outside);
    PASS();
}

/* The ceiling bounds what the reader expands, and the reader consumes (and for
   a .tar.gz inflates) the payload of every typeflag. A member counted only when
   it is a regular file leaves the bound naming a number it does not enforce. */
TEST a_non_file_member_counts_against_the_expansion_ceiling(void) {
    static char message[512];
    char destination[1024];
    snprintf(destination, sizeof destination, "%s/unpack-dirbomb-%d", fr_test_temp_base(),
             fr_test_process_id());
    fr_test_remove_tree(destination);
    fr_test_make_directory(destination);

    fr_unpack_limits limits = FR_UNPACK_DEFAULTS;
    limits.max_total_bytes = 1000;

    static char payload[600];
    memset(payload, 'a', sizeof payload);
    char buffer[FR_TAR_BLOCK * 16];
    memset(buffer, 0, sizeof buffer);
    size_t used = fr_test_tar_append(buffer, 0, "one/", '5', payload, sizeof payload);
    used = fr_test_tar_append(buffer, used, "two/", '5', payload, sizeof payload);
    fr_test_tar_end(buffer, used);

    fr_archive *archive = NULL;
    fr_error err;
    err.message[0] = '\0';
    int opened = open_archive_from_bytes(buffer, sizeof buffer, &archive, &err);
    fr_unpack_report report;
    int result = opened == FR_OK ? fr_unpack(archive, destination, &limits, &report, &err) : FR_ERR;
    if (archive != NULL) fr_archive_close(archive);
    snprintf(message, sizeof message, "opened %d, err \"%s\"", opened, err.message);
    fr_test_remove_tree(destination);

    ASSERT_EQm(message, FR_ERR, result);
    ASSERTm(message, strstr(message, "1000") != NULL);
    PASS();
}

TEST a_tar_members_execute_bit_reaches_the_file(void) {
#ifdef _WIN32
    SKIPm("apply_permissions carries no mode on Windows, so there is nothing here to assert");
#else
    static char message[512];
    char destination[1024];
    snprintf(destination, sizeof destination, "%s/unpack-mode-%d", fr_test_temp_base(),
             fr_test_process_id());
    fr_test_make_directory(destination);

    char buffer[FR_TAR_BLOCK * 16];
    memset(buffer, 0, sizeof buffer);
    size_t used = fr_test_tar_append(buffer, 0, "bin/runnable", '0', "x", 1);
    memcpy(buffer + 100, "0000755", 7);
    fr_test_tar_fix_checksum(buffer, 0);
    size_t plain_offset = used;
    used = fr_test_tar_append(buffer, used, "lib/plain.txt", '0', "x", 1);
    memcpy(buffer + plain_offset + 100, "0000644", 7);
    fr_test_tar_fix_checksum(buffer, plain_offset);
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

    char runnable[1024];
    char plain[1024];
    snprintf(runnable, sizeof runnable, "%s/bin/runnable", destination);
    snprintf(plain, sizeof plain, "%s/lib/plain.txt", destination);
    struct stat runnable_stat;
    struct stat plain_stat;
    int stat_ok = stat(runnable, &runnable_stat) == 0 && stat(plain, &plain_stat) == 0;
    int runnable_executable = stat_ok && (runnable_stat.st_mode & S_IXUSR) != 0;
    /* The second assertion is the one that matters: "chmod 0755 everything"
       passes the first on its own. */
    int plain_not_executable = stat_ok && (plain_stat.st_mode & S_IXUSR) == 0;
    snprintf(message, sizeof message, "result %d, stat %d", result, stat_ok);
    fr_test_remove_tree(destination);

    ASSERT_EQm(message, FR_OK, result);
    ASSERTm(message, runnable_executable);
    ASSERTm(message, plain_not_executable);
    PASS();
#endif
}

TEST a_unix_produced_zip_carries_its_execute_bit_and_a_windows_one_does_not(void) {
    static char message[512];
    /* Ninja ships ninja-linux.zip and Gradle ships only a zip for every
       platform, so "ignore the mode in a zip" would leave those binaries
       non-executable exactly where the bit is load bearing. The pair is the
       test: one archive claiming Unix and one not, same entry, different
       answer. */
    const char *names[] = { "bin/tool" };
    const char *contents[] = { "x" };
    const int executable[] = { 1 };

    char unix_zip[4096];
    size_t unix_length = fr_test_zip_build(unix_zip, sizeof unix_zip, names, contents, executable,
                                           1, 1);
    char dos_zip[4096];
    size_t dos_length = fr_test_zip_build(dos_zip, sizeof dos_zip, names, contents, executable, 1,
                                          0);

    int unix_executable = first_member_is_executable(unix_zip, unix_length);
    int dos_executable = first_member_is_executable(dos_zip, dos_length);

    snprintf(message, sizeof message, "unix %d, dos %d", unix_executable, dos_executable);
    ASSERT_EQm(message, 1, unix_executable);
    ASSERT_EQm(message, 0, dos_executable);
    PASS();
}

TEST a_setuid_member_is_refused_by_name(void) {
    static char message[512];
    char destination[1024];
    snprintf(destination, sizeof destination, "%s/unpack-setuid-%d", fr_test_temp_base(),
             fr_test_process_id());
    fr_test_make_directory(destination);

    char buffer[FR_TAR_BLOCK * 8];
    memset(buffer, 0, sizeof buffer);
    size_t used = fr_test_tar_append(buffer, 0, "bin/suid", '0', "x", 1);
    memcpy(buffer + 100, "0004755", 7);
    fr_test_tar_fix_checksum(buffer, 0);
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
    ASSERTm(message, strstr(message, "setuid") != NULL || strstr(message, "set-user") != NULL);
    PASS();
}

TEST a_symlink_target_holding_a_backslash_is_refused(void) {
    static char message[512];
    /* The component walker knows only "/", so "..\..\outside" would be a
       single ordinary component and climb out unnoticed the day the Windows
       skip is lifted. A member NAME already refuses a backslash outright. */
    fr_unpack_report report;
    int result = unpack_one_symlink("..\\..\\outside", &report, message, sizeof message);
    ASSERT_EQm(message, FR_ERR, result);
    ASSERTm(message, strstr(message, "backslash") != NULL);
    PASS();
}

TEST a_setgid_directory_is_unpacked_rather_than_refused(void) {
    static char message[512];
    /* drwxr-sr-x is routine in a tarball made on macOS or BSD, so refusing it
       would fail an ordinary JDK or Node archive. The bit reaches no file
       either way: apply_permissions only ever writes 0755 or 0644. */
    char destination[1024];
    snprintf(destination, sizeof destination, "%s/unpack-setgid-%d", fr_test_temp_base(),
             fr_test_process_id());
    fr_test_make_directory(destination);

    char buffer[FR_TAR_BLOCK * 8];
    memset(buffer, 0, sizeof buffer);
    size_t used = fr_test_tar_append(buffer, 0, "share/", '5', "", 0);
    memcpy(buffer + 100, "0002755", 7);
    fr_test_tar_fix_checksum(buffer, 0);
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

    size_t directories = report.directories_created;
    snprintf(message, sizeof message, "result %d, directories %zu, err \"%s\"", result,
             directories, err.message);
    fr_test_remove_tree(destination);

    ASSERT_EQm(message, FR_OK, result);
    ASSERT_EQm(message, 1u, directories);
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
    RUN_TEST(a_symlink_with_an_absolute_target_is_refused);
    RUN_TEST(a_symlink_whose_relative_target_climbs_out_is_refused);
    RUN_TEST(a_contained_symlink_is_created_on_posix_and_named_on_windows);
    RUN_TEST(a_chain_of_symlinks_cannot_walk_a_later_member_out);
    RUN_TEST(a_non_file_member_counts_against_the_expansion_ceiling);
    RUN_TEST(a_tar_members_execute_bit_reaches_the_file);
    RUN_TEST(a_unix_produced_zip_carries_its_execute_bit_and_a_windows_one_does_not);
    RUN_TEST(a_setuid_member_is_refused_by_name);
    RUN_TEST(a_symlink_target_holding_a_backslash_is_refused);
    RUN_TEST(a_setgid_directory_is_unpacked_rather_than_refused);
    remove(input_path);
    GREATEST_MAIN_END();
}
