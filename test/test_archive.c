#include "greatest.h"
#include "archive.h"

#include "support.h"
#include "tar.h"

#include <stdio.h>
#include <string.h>

GREATEST_MAIN_DEFS();

/* Produced by GNU tar 1.35 and gzip 1.14: a gzip whose header carries an FNAME
   field, which miniz's own writer never emits. */
static const unsigned char real_gzip_bytes[] = {
    0x1f, 0x8b, 0x08, 0x08, 0xaf, 0x77, 0xbb, 0x6a, 0x02, 0x03, 0x72, 0x65,
    0x61, 0x6c, 0x2e, 0x74, 0x61, 0x72, 0x00, 0xed, 0xd2, 0x31, 0x0a, 0x80,
    0x30, 0x0c, 0x85, 0xe1, 0x1c, 0x25, 0x07, 0x10, 0x49, 0x25, 0xad, 0xe7,
    0xe9, 0x50, 0x50, 0x10, 0x0b, 0x6d, 0x44, 0x8f, 0x6f, 0xe8, 0xe8, 0xe6,
    0xa0, 0x20, 0xe6, 0x5b, 0x7e, 0x78, 0x4b, 0x96, 0xac, 0x59, 0x52, 0x2f,
    0x87, 0xc0, 0x83, 0x48, 0x05, 0xe6, 0x56, 0x75, 0x2d, 0x39, 0x0a, 0xe0,
    0xd8, 0x33, 0x33, 0xd1, 0xd0, 0xf6, 0xd1, 0x6b, 0x90, 0xe0, 0x05, 0x5b,
    0x95, 0x58, 0xf4, 0x24, 0xfc, 0x53, 0xc4, 0x92, 0xe2, 0x82, 0x92, 0xf3,
    0x82, 0x7b, 0xd1, 0x67, 0x40, 0x99, 0xe6, 0xda, 0xe1, 0xbd, 0x1d, 0x8c,
    0x31, 0xc6, 0x7c, 0xcc, 0x09, 0x40, 0x5f, 0x5e, 0x75, 0x00, 0x08, 0x00,
    0x00
};

/* Produced by CPython's zipfile: deflated, and made on a Unix system, neither
   of which miniz's writer does here. */
static const unsigned char real_zip_bytes[] = {
    0x50, 0x4b, 0x03, 0x04, 0x14, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00,
    0x21, 0x58, 0x9b, 0x26, 0x81, 0x09, 0x1d, 0x00, 0x00, 0x00, 0x46, 0x00,
    0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x6e, 0x6f, 0x74, 0x65, 0x2e, 0x74,
    0x78, 0x74, 0x4b, 0x54, 0x28, 0x4a, 0x4d, 0xcc, 0x51, 0x28, 0xc9, 0xcf,
    0xcf, 0x51, 0x28, 0x2f, 0xca, 0x2f, 0x49, 0x55, 0x28, 0xc9, 0xc8, 0x2c,
    0xd6, 0x51, 0x48, 0x24, 0x49, 0x1c, 0x00, 0x50, 0x4b, 0x01, 0x02, 0x14,
    0x03, 0x14, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x21, 0x58, 0x9b,
    0x26, 0x81, 0x09, 0x1d, 0x00, 0x00, 0x00, 0x46, 0x00, 0x00, 0x00, 0x08,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xa4,
    0x81, 0x00, 0x00, 0x00, 0x00, 0x6e, 0x6f, 0x74, 0x65, 0x2e, 0x74, 0x78,
    0x74, 0x50, 0x4b, 0x05, 0x06, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01,
    0x00, 0x36, 0x00, 0x00, 0x00, 0x43, 0x00, 0x00, 0x00, 0x00, 0x00
};

static const char real_tool_line[] =
    "a real tool wrote this, a real tool wrote this, a real tool wrote this";

static int write_bytes_to_file(const char *file_name, const char *bytes, size_t length) {
    FILE *file = fopen(file_name, "wb");
    if (file == NULL) return 0;
    size_t written = length == 0 ? 0 : fwrite(bytes, 1, length, file);
    fclose(file);
    return written == length;
}

static const char *kind_name(fr_member_kind kind) {
    if (kind == FR_MEMBER_DIRECTORY) return "dir";
    if (kind == FR_MEMBER_SYMLINK) return "link";
    return "file";
}

/* Writes archive to a temp file, iterates it, and renders what came back as
   "name:kind:contents|", a symlink's target standing in for its contents, so
   three formats can be compared against one string. */
static int render_members(const char *bytes, size_t length, const char *suffix, char *out,
                          size_t out_size) {
    char archive_file[1024];
    snprintf(archive_file, sizeof archive_file, "%s/archive-render-%d%s", fr_test_temp_base(),
             fr_test_process_id(), suffix);
    out[0] = '\0';
    if (!write_bytes_to_file(archive_file, bytes, length)) return FR_ERR;

    fr_archive *archive = NULL;
    fr_error err;
    err.message[0] = '\0';
    int result = fr_archive_open(archive_file, 64 * 1024, &archive, &err);

    size_t used = 0;
    while (result == FR_OK) {
        const fr_archive_member *member = NULL;
        result = fr_archive_next(archive, &member, &err);
        if (result != FR_OK || member == NULL) break;
        const char *shown = member->kind == FR_MEMBER_SYMLINK ? member->link_target
                                                             : member->bytes;
        size_t shown_length = member->kind == FR_MEMBER_SYMLINK
                                  ? strlen(member->link_target == NULL ? "" : member->link_target)
                                  : member->length;
        int written = snprintf(out + used, out_size - used, "%s:%s:%.*s|", member->name,
                               kind_name(member->kind), (int) shown_length,
                               shown == NULL ? "" : shown);
        if (written < 0 || (size_t) written >= out_size - used) {
            result = FR_ERR;
            break;
        }
        used += (size_t) written;
    }

    if (archive != NULL) fr_archive_close(archive);
    remove(archive_file);
    return result;
}

TEST the_kind_is_decided_by_content_not_by_the_url(void) {
    static char message[256];
    char tar[FR_TAR_BLOCK * 4];
    memset(tar, 0, sizeof tar);
    size_t used = fr_test_tar_append(tar, 0, "a.txt", '0', "hello", 5);
    fr_test_tar_end(tar, used);

    char gz[4096];
    size_t gz_length = fr_test_gzip(gz, sizeof gz, tar, sizeof tar);

    const char *names[] = { "a.txt" };
    const char *contents[] = { "hello" };
    const int executable[] = { 0 };
    char zip[4096];
    size_t zip_length = fr_test_zip_build(zip, sizeof zip, names, contents, executable, 1, 1);

    fr_archive_kind kind;
    fr_error err;
    int tar_ok = fr_archive_kind_of(tar, sizeof tar, &kind, &err) == FR_OK
              && kind == FR_ARCHIVE_TAR;
    int gz_ok = fr_archive_kind_of(gz, gz_length, &kind, &err) == FR_OK
             && kind == FR_ARCHIVE_TAR_GZ;
    int zip_ok = fr_archive_kind_of(zip, zip_length, &kind, &err) == FR_OK
              && kind == FR_ARCHIVE_ZIP;

    snprintf(message, sizeof message, "tar %d gz %d zip %d", tar_ok, gz_ok, zip_ok);
    ASSERTm(message, tar_ok);
    ASSERTm(message, gz_ok);
    ASSERTm(message, zip_ok);
    PASS();
}

TEST bytes_that_are_none_of_the_three_are_refused(void) {
    static char message[512];
    const char *chunk = "return { name = \"lua\" }";

    fr_archive_kind kind;
    fr_error err;
    err.message[0] = '\0';
    int result = fr_archive_kind_of(chunk, strlen(chunk), &kind, &err);

    snprintf(message, sizeof message, "result %d, err \"%s\"", result, err.message);
    ASSERT_EQm(message, FR_ERR, result);
    ASSERTm(message, strstr(err.message, "zip") != NULL);
    PASS();
}

TEST a_tar_a_targz_and_a_zip_iterate_identically(void) {
    static char message[1024];
    /* One expectation, three formats. A format-specific mistake shows as a
       difference between the three rather than as a disagreement with a
       string somebody typed. */
    char tar[FR_TAR_BLOCK * 16];
    memset(tar, 0, sizeof tar);
    size_t used = fr_test_tar_append(tar, 0, "bin/", '5', "", 0);
    used = fr_test_tar_append(tar, used, "bin/tool", '0', "payload", 7);
    used = fr_test_tar_append(tar, used, "lib/data.txt", '0', "hello", 5);
    fr_test_tar_end(tar, used);

    char gz[8192];
    size_t gz_length = fr_test_gzip(gz, sizeof gz, tar, sizeof tar);

    const char *names[] = { "bin/", "bin/tool", "lib/data.txt" };
    const char *contents[] = { "", "payload", "hello" };
    const int executable[] = { 0, 0, 0 };
    char zip[8192];
    size_t zip_length = fr_test_zip_build(zip, sizeof zip, names, contents, executable, 3, 1);

    char from_tar[512];
    char from_gz[512];
    char from_zip[512];
    int tar_ok = render_members(tar, sizeof tar, ".tar", from_tar, sizeof from_tar) == FR_OK;
    int gz_ok = render_members(gz, gz_length, ".tar.gz", from_gz, sizeof from_gz) == FR_OK;
    int zip_ok = render_members(zip, zip_length, ".zip", from_zip, sizeof from_zip) == FR_OK;

    snprintf(message, sizeof message, "tar \"%s\" gz \"%s\" zip \"%s\"", from_tar, from_gz,
             from_zip);
    ASSERTm(message, tar_ok && gz_ok && zip_ok);
    ASSERT_STR_EQm(message, "bin/:dir:|bin/tool:file:payload|lib/data.txt:file:hello|", from_tar);
    ASSERT_STR_EQm(message, from_tar, from_gz);
    ASSERT_STR_EQm(message, from_tar, from_zip);
    PASS();
}

TEST a_zip_whose_local_header_contradicts_its_directory_is_refused(void) {
    static char message[512];
    const char *names[] = { "a.txt" };
    const char *contents[] = { "hello" };
    const int executable[] = { 0 };
    char zip[4096];
    size_t zip_length = fr_test_zip_build(zip, sizeof zip, names, contents, executable, 1, 1);

    /* The first local header starts at offset 0 and carries the uncompressed
       size at 22. Disagreeing with the central directory has no innocent
       cause: it is the parsing differential, and only the two readers'
       disagreement makes it exploitable. */
    zip[22] = (char) 0x40;

    char rendered[512];
    int result = render_members(zip, zip_length, ".zip", rendered, sizeof rendered);

    snprintf(message, sizeof message, "result %d, rendered \"%s\"", result, rendered);
    ASSERT_EQm(message, FR_ERR, result);
    PASS();
}

TEST a_real_gzip_and_a_real_zip_are_read(void) {
    static char message[1024];
    static char expected[256];
    snprintf(expected, sizeof expected, "note.txt:file:%s|", real_tool_line);

    char from_gz[512];
    char from_zip[512];
    int gz_ok = render_members((const char *) real_gzip_bytes, sizeof real_gzip_bytes, ".tar.gz",
                               from_gz, sizeof from_gz) == FR_OK;
    int zip_ok = render_members((const char *) real_zip_bytes, sizeof real_zip_bytes, ".zip",
                                from_zip, sizeof from_zip) == FR_OK;

    snprintf(message, sizeof message, "gz %d \"%s\" zip %d \"%s\"", gz_ok, from_gz, zip_ok,
             from_zip);
    ASSERTm(message, gz_ok && zip_ok);
    ASSERT_STR_EQm(message, expected, from_gz);
    ASSERT_STR_EQm(message, expected, from_zip);
    PASS();
}

TEST a_member_larger_than_the_cap_is_refused_by_name(void) {
    static char message[512];
    char tar[FR_TAR_BLOCK * 16];
    memset(tar, 0, sizeof tar);
    static char payload[2000];
    memset(payload, 'a', sizeof payload);
    size_t used = fr_test_tar_append(tar, 0, "big.bin", '0', payload, sizeof payload);
    fr_test_tar_end(tar, used);

    char archive_file[1024];
    snprintf(archive_file, sizeof archive_file, "%s/archive-cap-%d.tar", fr_test_temp_base(),
             fr_test_process_id());
    write_bytes_to_file(archive_file, tar, sizeof tar);

    fr_archive *archive = NULL;
    fr_error err;
    err.message[0] = '\0';
    /* The cap is passed to open, so it bounds the buffer before a member is
       ever extracted into it. */
    int opened = fr_archive_open(archive_file, 1024, &archive, &err);
    const fr_archive_member *member = NULL;
    int result = opened == FR_OK ? fr_archive_next(archive, &member, &err) : FR_ERR;
    if (archive != NULL) fr_archive_close(archive);
    remove(archive_file);
    snprintf(message, sizeof message, "opened %d, result %d, err \"%s\"", opened, result,
             err.message);

    ASSERT_EQm(message, FR_OK, opened);
    ASSERT_EQm(message, FR_ERR, result);
    ASSERTm(message, strstr(message, "big.bin") != NULL);
    ASSERTm(message, strstr(message, "1024") != NULL);
    PASS();
}

TEST a_tar_member_that_is_not_a_file_a_directory_or_a_symlink_is_refused(void) {
    static char message[512];
    char tar[FR_TAR_BLOCK * 8];
    memset(tar, 0, sizeof tar);
    size_t used = fr_test_tar_append(tar, 0, "dev/null", '3', "", 0);
    fr_test_tar_end(tar, used);

    char rendered[512];
    int result = render_members(tar, sizeof tar, ".tar", rendered, sizeof rendered);

    snprintf(message, sizeof message, "result %d, rendered \"%s\"", result, rendered);
    ASSERT_EQm(message, FR_ERR, result);
    PASS();
}

TEST a_tar_symlink_member_carries_its_target(void) {
    static char message[512];
    char tar[FR_TAR_BLOCK * 8];
    memset(tar, 0, sizeof tar);
    size_t used = fr_test_tar_append(tar, 0, "bin/tool", '2', "", 0);
    memcpy(tar + 157, "real/tool", 9);
    fr_test_tar_fix_checksum(tar, 0);
    fr_test_tar_end(tar, used);

    char rendered[512];
    int result = render_members(tar, sizeof tar, ".tar", rendered, sizeof rendered);

    snprintf(message, sizeof message, "result %d, rendered \"%s\"", result, rendered);
    ASSERT_EQm(message, FR_OK, result);
    ASSERT_STR_EQm(message, "bin/tool:link:real/tool|", rendered);
    PASS();
}

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(the_kind_is_decided_by_content_not_by_the_url);
    RUN_TEST(bytes_that_are_none_of_the_three_are_refused);
    RUN_TEST(a_tar_a_targz_and_a_zip_iterate_identically);
    RUN_TEST(a_zip_whose_local_header_contradicts_its_directory_is_refused);
    RUN_TEST(a_real_gzip_and_a_real_zip_are_read);
    RUN_TEST(a_member_larger_than_the_cap_is_refused_by_name);
    RUN_TEST(a_tar_member_that_is_not_a_file_a_directory_or_a_symlink_is_refused);
    RUN_TEST(a_tar_symlink_member_carries_its_target);
    GREATEST_MAIN_END();
}
