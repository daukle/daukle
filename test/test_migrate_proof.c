#include "greatest.h"

#include "migrate/proof.h"
#include "support.h"
#include "util/sha256.h"

#include <stdio.h>
#include <string.h>

GREATEST_MAIN_DEFS();

#define PATH_SIZE 512
#define ZIP_SIZE 4096
#define DIFFERENCE_SIZE 512

static char root[PATH_SIZE];

static int write_file(const char *name, const char *bytes, size_t length) {
    char path[PATH_SIZE];
    snprintf(path, sizeof path, "%s/%s", root, name);
    FILE *file = fopen(path, "wb");
    if (file == NULL) return 0;
    size_t written = fwrite(bytes, 1, length, file);
    fclose(file);
    return written == length;
}

static const int not_executable[] = { 0, 0 };

static int write_zip(const char *name, const char *const *names, const char *const *contents,
                     size_t count) {
    char zip[ZIP_SIZE];
    size_t length = fr_test_zip_build(zip, sizeof zip, names, contents, not_executable, count, 0);
    return length > 0 && write_file(name, zip, length);
}

static int compare(fr_proof_kind kind, const char *old_name, const char *new_name, int *same,
                   char *difference, fr_error *err) {
    return fr_proof_compare(root, old_name, new_name, kind, same, difference, DIFFERENCE_SIZE, err);
}

TEST identical_bytes_are_the_same(void) {
    ASSERT(write_file("old.bin", "x", 1));
    ASSERT(write_file("new.bin", "x", 1));
    int same = 0;
    char difference[DIFFERENCE_SIZE] = "";
    fr_error err;
    ASSERT_EQ(FR_OK, compare(FR_PROOF_BYTES, "old.bin", "new.bin", &same, difference, &err));
    ASSERT(same);
    PASS();
}

TEST different_bytes_name_both_digests(void) {
    ASSERT(write_file("old.bin", "x", 1));
    ASSERT(write_file("new.bin", "y", 1));
    char old_digest[65];
    char new_digest[65];
    fr_sha256_hex("x", 1, old_digest);
    fr_sha256_hex("y", 1, new_digest);
    int same = 1;
    char difference[DIFFERENCE_SIZE] = "";
    fr_error err;
    ASSERT_EQ(FR_OK, compare(FR_PROOF_BYTES, "old.bin", "new.bin", &same, difference, &err));
    ASSERT(!same);
    ASSERT(strstr(difference, old_digest) != NULL);
    ASSERT(strstr(difference, new_digest) != NULL);
    PASS();
}

static int compare_texts(const char *old_text, size_t old_length, const char *new_text,
                         size_t new_length, char *difference) {
    if (!write_file("old.txt", old_text, old_length) || !write_file("new.txt", new_text, new_length)) {
        return 0;
    }
    int same = 1;
    fr_error err;
    if (compare(FR_PROOF_BYTES, "old.txt", "new.txt", &same, difference, &err) != FR_OK) return 0;
    return !same;
}

TEST bytes_proof_names_the_lines_only_on_each_side(void) {
    char difference[DIFFERENCE_SIZE] = "";
    ASSERT(compare_texts("a\nb\nc\n", 6, "a\nc\nd\n", 6, difference));
    ASSERT(strstr(difference, "only in the old build: \"b\"") != NULL);
    ASSERT(strstr(difference, "only in the new build: \"d\"") != NULL);
    ASSERT(strstr(difference, "\"a\"") == NULL);
    PASS();
}

TEST bytes_proof_counts_lines_past_the_first_five(void) {
    char difference[DIFFERENCE_SIZE] = "";
    const char *old_text = "l1\nl2\nl3\nl4\nl5\nl6\nl7\n";
    ASSERT(compare_texts(old_text, strlen(old_text), "", 0, difference));
    ASSERT(strstr(difference, "\"l5\"") != NULL);
    ASSERT(strstr(difference, "\"l6\"") == NULL);
    ASSERT(strstr(difference, "and 2 more") != NULL);
    PASS();
}

TEST bytes_proof_keeps_only_the_digests_for_binary_files(void) {
    char difference[DIFFERENCE_SIZE] = "";
    char old_digest[65];
    fr_sha256_hex("a\0b", 3, old_digest);
    ASSERT(compare_texts("a\0b", 3, "a\0c", 3, difference));
    ASSERT(strstr(difference, "only in") == NULL);
    ASSERT(strstr(difference, old_digest) != NULL);
    PASS();
}

TEST bytes_proof_reports_a_reordering(void) {
    char difference[DIFFERENCE_SIZE] = "";
    ASSERT(compare_texts("a\nb\n", 4, "b\na\n", 4, difference));
    ASSERT(strstr(difference, "the same lines in a different order") != NULL);
    PASS();
}

TEST bytes_proof_treats_crlf_as_different(void) {
    char difference[DIFFERENCE_SIZE] = "";
    ASSERT(compare_texts("a\r\n", 3, "a\n", 2, difference));
    ASSERT(strstr(difference, "only in the old build: \"a\\r\"") != NULL);
    ASSERT(strstr(difference, "only in the new build: \"a\"") != NULL);
    PASS();
}

TEST archives_with_the_same_members_in_another_order_are_the_same(void) {
    const char *first_names[] = { "a.class", "b.class" };
    const char *first_contents[] = { "1", "2" };
    const char *second_names[] = { "b.class", "a.class" };
    const char *second_contents[] = { "2", "1" };
    ASSERT(write_zip("old.zip", first_names, first_contents, 2));
    ASSERT(write_zip("new.zip", second_names, second_contents, 2));
    int same = 0;
    char difference[DIFFERENCE_SIZE] = "";
    fr_error err;
    ASSERT_EQ(FR_OK, compare(FR_PROOF_ARCHIVE, "old.zip", "new.zip", &same, difference, &err));
    ASSERT(same);
    PASS();
}

TEST an_archive_member_missing_on_one_side_is_named(void) {
    const char *both_names[] = { "a", "b" };
    const char *both_contents[] = { "1", "2" };
    const char *one_names[] = { "a" };
    const char *one_contents[] = { "1" };
    ASSERT(write_zip("old.zip", both_names, both_contents, 2));
    ASSERT(write_zip("new.zip", one_names, one_contents, 1));
    int same = 1;
    char difference[DIFFERENCE_SIZE] = "";
    fr_error err;
    ASSERT_EQ(FR_OK, compare(FR_PROOF_ARCHIVE, "old.zip", "new.zip", &same, difference, &err));
    ASSERT(!same);
    ASSERT(strstr(difference, "\"b\"") != NULL);
    ASSERT(strstr(difference, "new") != NULL);
    PASS();
}

TEST an_archive_member_with_other_bytes_is_named(void) {
    const char *names[] = { "a" };
    const char *old_contents[] = { "1" };
    const char *new_contents[] = { "2" };
    ASSERT(write_zip("old.zip", names, old_contents, 1));
    ASSERT(write_zip("new.zip", names, new_contents, 1));
    int same = 1;
    char difference[DIFFERENCE_SIZE] = "";
    fr_error err;
    ASSERT_EQ(FR_OK, compare(FR_PROOF_ARCHIVE, "old.zip", "new.zip", &same, difference, &err));
    ASSERT(!same);
    ASSERT(strstr(difference, "\"a\"") != NULL);
    PASS();
}

TEST a_missing_side_is_an_error(void) {
    ASSERT(write_file("new.bin", "x", 1));
    int same = 1;
    char difference[DIFFERENCE_SIZE] = "";
    fr_error err;
    ASSERT_EQ(FR_ERR, compare(FR_PROOF_BYTES, "absent.bin", "new.bin", &same, difference, &err));
    ASSERT(strstr(err.message, "absent.bin") != NULL);
    const char *names[] = { "a" };
    const char *contents[] = { "1" };
    ASSERT(write_zip("present.zip", names, contents, 1));
    ASSERT_EQ(FR_ERR, compare(FR_PROOF_ARCHIVE, "present.zip", "absent.zip", &same, difference, &err));
    ASSERT(strstr(err.message, "absent.zip") != NULL);
    PASS();
}

static size_t find_central_record(const unsigned char *bytes, size_t length) {
    static const unsigned char signature[4] = { 'P', 'K', 1, 2 };
    for (size_t offset = 0; offset + sizeof signature <= length; offset++) {
        if (memcmp(bytes + offset, signature, sizeof signature) == 0) return offset;
    }
    return length;
}

static void change_dos_field(unsigned char *field) {
    field[0] ^= 0xff;
    field[1] ^= 0x7f;
}

TEST archive_timestamps_are_ignored(void) {
    const char *names[] = { "a.class" };
    const char *contents[] = { "1" };
    char zip[ZIP_SIZE];
    size_t length = fr_test_zip_build(zip, sizeof zip, names, contents, not_executable, 1, 0);
    ASSERT(length > 0);
    ASSERT(write_file("old.zip", zip, length));

    unsigned char *bytes = (unsigned char *) zip;
    size_t central = find_central_record(bytes, length);
    ASSERT(central < length);
    change_dos_field(bytes + 10);
    change_dos_field(bytes + 12);
    change_dos_field(bytes + central + 12);
    change_dos_field(bytes + central + 14);
    ASSERT(write_file("new.zip", zip, length));

    int same = 0;
    char difference[DIFFERENCE_SIZE] = "";
    fr_error err;
    ASSERT_EQ(FR_OK, compare(FR_PROOF_BYTES, "old.zip", "new.zip", &same, difference, &err));
    ASSERT(!same);
    ASSERT_EQ(FR_OK, compare(FR_PROOF_ARCHIVE, "old.zip", "new.zip", &same, difference, &err));
    ASSERT(same);
    PASS();
}

TEST the_kind_vocabulary_is_parsed_in_one_place(void) {
    fr_proof_kind kind;
    ASSERT_EQ(FR_OK, fr_proof_kind_parse("bytes", &kind));
    ASSERT_EQ(FR_PROOF_BYTES, kind);
    ASSERT_EQ(FR_OK, fr_proof_kind_parse("archive", &kind));
    ASSERT_EQ(FR_PROOF_ARCHIVE, kind);
    ASSERT_EQ(FR_ERR, fr_proof_kind_parse("Bytes", &kind));
    ASSERT_EQ(FR_ERR, fr_proof_kind_parse("", &kind));
    PASS();
}

SUITE(migrate_proof) {
    RUN_TEST(identical_bytes_are_the_same);
    RUN_TEST(different_bytes_name_both_digests);
    RUN_TEST(bytes_proof_names_the_lines_only_on_each_side);
    RUN_TEST(bytes_proof_counts_lines_past_the_first_five);
    RUN_TEST(bytes_proof_keeps_only_the_digests_for_binary_files);
    RUN_TEST(bytes_proof_reports_a_reordering);
    RUN_TEST(bytes_proof_treats_crlf_as_different);
    RUN_TEST(archives_with_the_same_members_in_another_order_are_the_same);
    RUN_TEST(an_archive_member_missing_on_one_side_is_named);
    RUN_TEST(an_archive_member_with_other_bytes_is_named);
    RUN_TEST(a_missing_side_is_an_error);
    RUN_TEST(archive_timestamps_are_ignored);
    RUN_TEST(the_kind_vocabulary_is_parsed_in_one_place);
}

int main(int argc, char **argv) {
    snprintf(root, sizeof root, "%s/proof-%d", fr_test_temp_base(), fr_test_process_id());
    fr_test_remove_tree(root);
    fr_test_make_directory(root);
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(migrate_proof);
    fr_test_remove_tree(root);
    GREATEST_MAIN_END();
}
