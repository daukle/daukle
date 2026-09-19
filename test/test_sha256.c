#include "greatest.h"
#include "sha256.h"

#include <string.h>

TEST hashes_the_empty_input(void) {
    char hex[65];
    fr_sha256_hex("", 0, hex);
    ASSERT_STR_EQ("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", hex);
    PASS();
}

TEST hashes_abc(void) {
    char hex[65];
    fr_sha256_hex("abc", 3, hex);
    ASSERT_STR_EQ("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", hex);
    PASS();
}

TEST hashes_a_multi_block_input(void) {
    char data[1000];
    memset(data, 'a', sizeof data);
    char hex[65];
    fr_sha256_hex(data, sizeof data, hex);
    ASSERT_EQ(64, (int) strlen(hex));
    ASSERT_STR_EQ("41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3",
                  hex);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(hashes_the_empty_input);
    RUN_TEST(hashes_abc);
    RUN_TEST(hashes_a_multi_block_input);
    GREATEST_MAIN_END();
}
