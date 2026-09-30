#include "greatest.h"
#include "archive/tar.h"

#include "project/region.h"
#include "util/error.h"
#include "support.h"

#include <stdlib.h>
#include <string.h>

GREATEST_MAIN_DEFS();

static char buffer[8192];

static void reset(void) {
    memset(buffer, 0, sizeof buffer);
}

TEST an_archive_lists_its_members_in_order(void) {
    reset();
    size_t offset = fr_test_tar_append(buffer, 0, "plugin.lua", '0', "return 1", 8);
    offset = fr_test_tar_append(buffer, offset, "lib/semver.lua", '0', "return 22", 9);
    offset = fr_test_tar_end(buffer, offset);

    fr_tar archive;
    fr_error err;
    ASSERT_EQ(FR_OK, fr_tar_read(buffer, offset, &archive, &err));
    ASSERT_EQ(2u, archive.count);
    ASSERT_STR_EQ("plugin.lua", archive.members[0].name);
    ASSERT_EQ(8u, archive.members[0].length);
    ASSERT(memcmp(archive.members[0].bytes, "return 1", 8) == 0);
    ASSERT_STR_EQ("lib/semver.lua", archive.members[1].name);
    ASSERT_EQ(9u, archive.members[1].length);
    ASSERT(memcmp(archive.members[1].bytes, "return 22", 9) == 0);
    PASS();
}

TEST a_member_is_found_by_name(void) {
    reset();
    size_t offset = fr_test_tar_append(buffer, 0, "plugin.lua", '0', "return 1", 8);
    offset = fr_test_tar_end(buffer, offset);

    fr_tar archive;
    fr_error err;
    ASSERT_EQ(FR_OK, fr_tar_read(buffer, offset, &archive, &err));
    ASSERT(fr_tar_find(&archive, "plugin.lua") != NULL);
    ASSERT(fr_tar_find(&archive, "absent.lua") == NULL);
    PASS();
}

/* Asserts the CONTENT of the member after the directory entry, not merely its
   name: a skip that forgot to advance past the entry's own block would still
   produce the right name list while reading everything after it from the wrong
   offset. */
TEST a_directory_entry_is_skipped_without_shifting_the_rest(void) {
    reset();
    size_t offset = fr_test_tar_append(buffer, 0, "plugin.lua", '0', "return 1", 8);
    offset = fr_test_tar_append(buffer, offset, "lib/", '5', "", 0);
    offset = fr_test_tar_append(buffer, offset, "lib/semver.lua", '0', "return 22", 9);
    offset = fr_test_tar_end(buffer, offset);

    fr_tar archive;
    fr_error err;
    ASSERT_EQ(FR_OK, fr_tar_read(buffer, offset, &archive, &err));
    ASSERT_EQ(2u, archive.count);
    ASSERT_STR_EQ("lib/semver.lua", archive.members[1].name);
    ASSERT_EQ(9u, archive.members[1].length);
    ASSERT(memcmp(archive.members[1].bytes, "return 22", 9) == 0);
    PASS();
}

static int refuses_typeflag(char typeflag) {
    reset();
    size_t offset = fr_test_tar_append(buffer, 0, "plugin.lua", typeflag, "", 0);
    offset = fr_test_tar_end(buffer, offset);

    fr_tar archive;
    fr_error err;
    if (fr_tar_read(buffer, offset, &archive, &err) == FR_OK) return 0;
    return strstr(err.message, "is not a regular file") != NULL;
}

TEST a_symlink_member_is_refused(void) {
    ASSERT(refuses_typeflag('2'));
    PASS();
}

TEST a_hardlink_member_is_refused(void) {
    ASSERT(refuses_typeflag('1'));
    PASS();
}

TEST a_character_device_member_is_refused(void) {
    ASSERT(refuses_typeflag('3'));
    PASS();
}

TEST a_fifo_member_is_refused(void) {
    ASSERT(refuses_typeflag('6'));
    PASS();
}

/* An extension record is refused rather than interpreted, which is what keeps
   the reader's surface the size it looks. */
TEST a_pax_extended_header_is_refused(void) {
    ASSERT(refuses_typeflag('x'));
    PASS();
}

TEST a_gnu_long_name_header_is_refused(void) {
    ASSERT(refuses_typeflag('L'));
    PASS();
}

TEST a_bad_header_checksum_is_refused(void) {
    reset();
    size_t offset = fr_test_tar_append(buffer, 0, "plugin.lua", '0', "return 1", 8);
    offset = fr_test_tar_end(buffer, offset);
    buffer[10] = 'x';

    fr_tar archive;
    fr_error err;
    ASSERT_EQ(FR_ERR, fr_tar_read(buffer, offset, &archive, &err));
    ASSERT(strstr(err.message, "bad header checksum") != NULL);
    PASS();
}

TEST a_base_256_size_is_refused(void) {
    reset();
    size_t offset = fr_test_tar_append(buffer, 0, "plugin.lua", '0', "return 1", 8);
    offset = fr_test_tar_end(buffer, offset);
    buffer[124] = (char) (unsigned char) 0x80u;
    fr_test_tar_fix_checksum(buffer, 0);

    fr_tar archive;
    fr_error err;
    ASSERT_EQ(FR_ERR, fr_tar_read(buffer, offset, &archive, &err));
    ASSERT(strstr(err.message, "not octal") != NULL);
    PASS();
}

TEST a_non_empty_prefix_field_is_refused(void) {
    reset();
    size_t offset = fr_test_tar_append(buffer, 0, "plugin.lua", '0', "return 1", 8);
    offset = fr_test_tar_end(buffer, offset);
    buffer[345] = 'a';
    fr_test_tar_fix_checksum(buffer, 0);

    fr_tar archive;
    fr_error err;
    ASSERT_EQ(FR_ERR, fr_tar_read(buffer, offset, &archive, &err));
    ASSERT(strstr(err.message, "too long to store in one field") != NULL);
    PASS();
}

TEST an_archive_that_ends_inside_a_member_is_refused(void) {
    char content[600];
    memset(content, 'x', sizeof content);

    reset();
    size_t offset = fr_test_tar_append(buffer, 0, "plugin.lua", '0', content, sizeof content);
    offset = fr_test_tar_end(buffer, offset);

    fr_tar archive;
    fr_error err;
    /* 900 stops inside the member, whose header declares 600 bytes after the
       first 512. */
    ASSERT_EQ(FR_ERR, fr_tar_read(buffer, 900, &archive, &err));
    ASSERT(strstr(err.message, "ends inside") != NULL);
    PASS();
}

TEST a_duplicate_member_name_is_refused(void) {
    reset();
    size_t offset = fr_test_tar_append(buffer, 0, "plugin.lua", '0', "return 1", 8);
    offset = fr_test_tar_append(buffer, offset, "plugin.lua", '0', "return 2", 8);
    offset = fr_test_tar_end(buffer, offset);

    fr_tar archive;
    fr_error err;
    ASSERT_EQ(FR_ERR, fr_tar_read(buffer, offset, &archive, &err));
    ASSERT(strstr(err.message, "twice") != NULL);
    PASS();
}

TEST a_name_past_the_length_limit_is_refused(void) {
    char name[FR_TAR_MAX_NAME + 2];
    memset(name, 'a', sizeof name - 1);
    name[sizeof name - 1] = '\0';

    reset();
    size_t offset = fr_test_tar_append(buffer, 0, name, '0', "return 1", 8);
    offset = fr_test_tar_end(buffer, offset);

    fr_tar archive;
    fr_error err;
    ASSERT_EQ(FR_ERR, fr_tar_read(buffer, offset, &archive, &err));
    ASSERT(strstr(err.message, "longer than") != NULL);
    PASS();
}

TEST an_archive_past_the_member_limit_is_refused(void) {
    size_t needed = (FR_TAR_MAX_MEMBERS + 1) * 2 * FR_TAR_BLOCK + 2 * FR_TAR_BLOCK;
    char *big = calloc(1, needed);
    ASSERT(big != NULL);

    size_t offset = 0;
    for (int index = 0; index <= FR_TAR_MAX_MEMBERS; index++) {
        char name[32];
        snprintf(name, sizeof name, "member%d.lua", index);
        offset = fr_test_tar_append(big, offset, name, '0', "x", 1);
    }
    offset = fr_test_tar_end(big, offset);

    fr_tar archive;
    fr_error err;
    int status = fr_tar_read(big, offset, &archive, &err);
    char message[512];
    snprintf(message, sizeof message, "%s", err.message);
    free(big);

    ASSERT_EQ(FR_ERR, status);
    ASSERT(strstr(message, "more than") != NULL);
    PASS();
}

/* The declared size is what is refused, so the archive never has to carry the
   megabyte the limit exists to keep out. */
TEST a_member_past_the_size_limit_is_refused(void) {
    reset();
    size_t offset = fr_test_tar_append(buffer, 0, "plugin.lua", '0', "x", 1);
    offset = fr_test_tar_end(buffer, offset);

    unsigned long long over = FR_TAR_MAX_MEMBER_BYTES + 1u;
    memset(buffer + 124, '0', 11);
    for (size_t index = 0; index < 11 && over > 0; index++, over >>= 3) {
        buffer[124 + 10 - index] = (char) ('0' + (over & 7u));
    }
    buffer[135] = '\0';
    fr_test_tar_fix_checksum(buffer, 0);

    fr_tar archive;
    fr_error err;
    ASSERT_EQ(FR_ERR, fr_tar_read(buffer, offset, &archive, &err));
    ASSERT(strstr(err.message, "larger than") != NULL);
    PASS();
}

TEST a_leading_dot_slash_is_stripped_once(void) {
    reset();
    size_t offset = fr_test_tar_append(buffer, 0, "./plugin.lua", '0', "return 1", 8);
    offset = fr_test_tar_append(buffer, offset, "././second.lua", '0', "return 2", 8);
    offset = fr_test_tar_append(buffer, offset, ".././escape.lua", '0', "return 3", 8);
    offset = fr_test_tar_end(buffer, offset);

    fr_tar archive;
    fr_error err;
    ASSERT_EQ(FR_OK, fr_tar_read(buffer, offset, &archive, &err));
    ASSERT_EQ(3u, archive.count);
    ASSERT_STR_EQ("plugin.lua", archive.members[0].name);
    /* One prefix, not as many as there are: stripping in a loop is a different
       reader and this is the name that tells them apart. */
    ASSERT_STR_EQ("./second.lua", archive.members[1].name);
    /* And nothing else is normalised, so what climbs out still looks like it
       does to the rule that refuses it one layer up. */
    ASSERT_STR_EQ(".././escape.lua", archive.members[2].name);
    PASS();
}

TEST an_empty_first_block_ends_the_archive(void) {
    reset();
    fr_tar archive;
    fr_error err;
    ASSERT_EQ(FR_OK, fr_tar_read(buffer, 2 * FR_TAR_BLOCK, &archive, &err));
    ASSERT_EQ(0u, archive.count);
    PASS();
}

TEST a_lua_chunk_is_not_taken_for_an_archive(void) {
    static const char chunk[] = "daukle.plugin{ api = 1 }\n";
    ASSERT_EQ(0, fr_tar_looks_like_archive(chunk, sizeof chunk - 1));

    char comment[900];
    memset(comment, 'x', sizeof comment);
    memcpy(comment, "--[[", 4);
    ASSERT_EQ(0, fr_tar_looks_like_archive(comment, sizeof comment));

    /* The magic alone is not the test: a chunk may hold the word and still be
       a chunk. */
    memcpy(comment + 257, "ustar", 5);
    ASSERT_EQ(0, fr_tar_looks_like_archive(comment, sizeof comment));
    PASS();
}

TEST an_archive_is_recognised(void) {
    reset();
    size_t offset = fr_test_tar_append(buffer, 0, "plugin.lua", '0', "return 1", 8);
    offset = fr_test_tar_end(buffer, offset);
    ASSERT_EQ(1, fr_tar_looks_like_archive(buffer, offset));
    PASS();
}

/* The builder in support.c is daukle's own idea of a tar, so a reader tested
   only against it would share any mistake the builder makes. These bytes came
   out of GNU tar 1.35:
     tar --format=ustar -cf plugin.tar plugin.lua lib/semver.lua
   with the trailing padding past the two terminator blocks removed. */
TEST a_real_tar_archive_reads(void) {
    char *bytes = NULL;
    size_t length = 0;
    fr_error err;
    ASSERT_EQ(FR_OK, fr_file_read_bytes("test/fixtures/tar/real-ustar.tar", &bytes, &length, &err));

    fr_tar archive;
    int status = fr_tar_read(bytes, length, &archive, &err);
    char message[512];
    snprintf(message, sizeof message, "%s", err.message);

    int recognised = fr_tar_looks_like_archive(bytes, length);
    size_t count = archive.count;
    char first[FR_TAR_MAX_NAME + 1];
    char second[FR_TAR_MAX_NAME + 1];
    size_t first_length = 0;
    int first_matches = 0;
    if (status == FR_OK && count == 2) {
        snprintf(first, sizeof first, "%s", archive.members[0].name);
        snprintf(second, sizeof second, "%s", archive.members[1].name);
        first_length = archive.members[0].length;
        first_matches = memcmp(archive.members[0].bytes, "return 1\n", 9) == 0;
    } else {
        first[0] = '\0';
        second[0] = '\0';
    }
    free(bytes);

    ASSERT_EQm(message, FR_OK, status);
    ASSERT_EQ(1, recognised);
    ASSERT_EQ(2u, count);
    ASSERT_STR_EQ("plugin.lua", first);
    ASSERT_STR_EQ("lib/semver.lua", second);
    ASSERT_EQ(9u, first_length);
    ASSERT(first_matches);
    PASS();
}

TEST the_header_parser_joins_a_prefix_to_a_name(void) {
    static char message[512];
    unsigned char block[FR_TAR_BLOCK];
    char local_bytes[FR_TAR_BLOCK * 4];
    memset(local_bytes, 0, sizeof local_bytes);
    fr_test_tar_append(local_bytes, 0, "deep/inner.txt", '0', "x", 1);
    /* The prefix field is 155 bytes at offset 345, and a reader that ignores it
       silently produces a DIFFERENT file's name, which is the defect. */
    memcpy(local_bytes + 345, "outer", 5);
    fr_test_tar_fix_checksum(local_bytes, 0);
    memcpy(block, local_bytes, FR_TAR_BLOCK);

    fr_tar_header header;
    int end = 0;
    fr_error err;
    err.message[0] = '\0';
    int result = fr_tar_read_header(block, 0, &header, &end, &err);

    snprintf(message, sizeof message, "result %d, name \"%s\", err \"%s\"", result, header.name,
             err.message);
    ASSERT_EQm(message, FR_OK, result);
    ASSERT_EQm(message, 0, end);
    ASSERT_STR_EQm(message, "outer/deep/inner.txt", header.name);
    ASSERT_EQm(message, 1, header.used_prefix);
    PASS();
}

TEST the_plugin_reader_still_refuses_a_prefix(void) {
    static char message[512];
    char local_bytes[FR_TAR_BLOCK * 4];
    memset(local_bytes, 0, sizeof local_bytes);
    size_t offset = fr_test_tar_append(local_bytes, 0, "inner.txt", '0', "x", 1);
    memcpy(local_bytes + 345, "outer", 5);
    fr_test_tar_fix_checksum(local_bytes, 0);
    fr_test_tar_end(local_bytes, offset);

    fr_tar archive;
    fr_error err;
    err.message[0] = '\0';
    int result = fr_tar_read(local_bytes, sizeof local_bytes, &archive, &err);

    snprintf(message, sizeof message, "%s", err.message);
    ASSERT_EQm(message, FR_ERR, result);
    ASSERTm(message, strstr(message, "too long to store in one") != NULL);
    PASS();
}

TEST the_header_parser_reports_the_zero_block_as_the_end(void) {
    unsigned char block[FR_TAR_BLOCK];
    memset(block, 0, sizeof block);

    fr_tar_header header;
    int end = 0;
    fr_error err;
    int result = fr_tar_read_header(block, 0, &header, &end, &err);

    ASSERT_EQ(FR_OK, result);
    ASSERT_EQ(1, end);
    PASS();
}

TEST the_header_parser_reads_the_mode_and_the_link_target(void) {
    static char message[512];
    char local_bytes[FR_TAR_BLOCK * 4];
    memset(local_bytes, 0, sizeof local_bytes);
    fr_test_tar_append(local_bytes, 0, "bin/tool", '2', "", 0);
    memcpy(local_bytes + 100, "0000755", 7);           /* mode field, 8 bytes at 100 */
    memcpy(local_bytes + 157, "../real/tool", 12);     /* linkname field, 100 bytes at 157 */
    fr_test_tar_fix_checksum(local_bytes, 0);

    fr_tar_header header;
    int end = 0;
    fr_error err;
    int result = fr_tar_read_header((const unsigned char *) local_bytes, 0, &header, &end, &err);

    snprintf(message, sizeof message, "result %d, mode %lo, link \"%s\"", result, header.mode,
             header.link_target);
    ASSERT_EQm(message, FR_OK, result);
    ASSERT_EQm(message, '2', header.typeflag);
    ASSERTm(message, (header.mode & 0111u) != 0);
    ASSERT_STR_EQm(message, "../real/tool", header.link_target);
    PASS();
}

TEST the_header_parser_names_the_offset_of_a_bad_checksum(void) {
    static char message[512];
    char local_bytes[FR_TAR_BLOCK * 3];
    memset(local_bytes, 0, sizeof local_bytes);
    size_t offset = fr_test_tar_append(local_bytes, 0, "first.txt", '0', "", 0);
    fr_test_tar_append(local_bytes, offset, "second.txt", '0', "y", 1);
    local_bytes[offset + 10] = 'z';

    fr_tar_header header;
    int end = 0;
    fr_error err;
    err.message[0] = '\0';
    int result = fr_tar_read_header((const unsigned char *) local_bytes + offset, offset, &header,
                                    &end, &err);

    snprintf(message, sizeof message, "result %d, offset %zu, err \"%s\"", result, offset,
             err.message);
    ASSERT_EQm(message, FR_ERR, result);
    ASSERTm(message, strstr(err.message, "at offset 512") != NULL);
    PASS();
}

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(an_archive_lists_its_members_in_order);
    RUN_TEST(a_member_is_found_by_name);
    RUN_TEST(a_directory_entry_is_skipped_without_shifting_the_rest);
    RUN_TEST(a_symlink_member_is_refused);
    RUN_TEST(a_hardlink_member_is_refused);
    RUN_TEST(a_character_device_member_is_refused);
    RUN_TEST(a_fifo_member_is_refused);
    RUN_TEST(a_pax_extended_header_is_refused);
    RUN_TEST(a_gnu_long_name_header_is_refused);
    RUN_TEST(a_bad_header_checksum_is_refused);
    RUN_TEST(a_base_256_size_is_refused);
    RUN_TEST(a_non_empty_prefix_field_is_refused);
    RUN_TEST(an_archive_that_ends_inside_a_member_is_refused);
    RUN_TEST(a_duplicate_member_name_is_refused);
    RUN_TEST(a_name_past_the_length_limit_is_refused);
    RUN_TEST(an_archive_past_the_member_limit_is_refused);
    RUN_TEST(a_member_past_the_size_limit_is_refused);
    RUN_TEST(a_leading_dot_slash_is_stripped_once);
    RUN_TEST(an_empty_first_block_ends_the_archive);
    RUN_TEST(a_lua_chunk_is_not_taken_for_an_archive);
    RUN_TEST(an_archive_is_recognised);
    RUN_TEST(a_real_tar_archive_reads);
    RUN_TEST(the_header_parser_joins_a_prefix_to_a_name);
    RUN_TEST(the_plugin_reader_still_refuses_a_prefix);
    RUN_TEST(the_header_parser_reports_the_zero_block_as_the_end);
    RUN_TEST(the_header_parser_reads_the_mode_and_the_link_target);
    RUN_TEST(the_header_parser_names_the_offset_of_a_bad_checksum);
    GREATEST_MAIN_END();
}
