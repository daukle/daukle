#include "greatest.h"
#include "sha256.h"

#include <string.h>

static void incremental(const char *data, size_t length, size_t chunk, char out_hex[65]) {
    fr_sha256 context;
    fr_sha256_init(&context);
    for (size_t offset = 0; offset < length; offset += chunk) {
        size_t remaining = length - offset;
        fr_sha256_update(&context, data + offset, remaining < chunk ? remaining : chunk);
    }
    fr_sha256_final(&context, out_hex);
}

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

TEST hashes_the_padding_boundary_lengths(void) {
    char data[64];
    char hex[65];

    memset(data, 'a', 55);
    fr_sha256_hex(data, 55, hex);
    ASSERT_STR_EQ("9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318", hex);

    memset(data, 'a', 56);
    fr_sha256_hex(data, 56, hex);
    ASSERT_STR_EQ("b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a", hex);

    memset(data, 'a', 63);
    fr_sha256_hex(data, 63, hex);
    ASSERT_STR_EQ("7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34", hex);

    memset(data, 'a', 64);
    fr_sha256_hex(data, sizeof data, hex);
    ASSERT_STR_EQ("ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb", hex);

    PASS();
}

TEST the_incremental_digest_matches_the_one_shot_digest(void) {
    static char message[256];
    char data[1000];
    for (size_t index = 0; index < sizeof data; index++) data[index] = (char) (index % 251);

    char expected[65];
    fr_sha256_hex(data, sizeof data, expected);

    /* Chunk sizes chosen to land on, just inside and just past the 64-byte
       block boundary, which is where a buffering mistake hides. */
    static const size_t CHUNKS[] = { 1, 63, 64, 65, 128, 999, 1000 };
    for (size_t index = 0; index < sizeof CHUNKS / sizeof CHUNKS[0]; index++) {
        char actual[65];
        incremental(data, sizeof data, CHUNKS[index], actual);
        snprintf(message, sizeof message, "chunk %zu gave %s, expected %s", CHUNKS[index], actual,
                 expected);
        ASSERT_STR_EQm(message, expected, actual);
    }
    PASS();
}

TEST the_incremental_digest_of_nothing_matches(void) {
    /* Asserted against the published constant rather than against
       fr_sha256_hex, which now runs through the same final() this is meant to
       catch a defect in and would agree with itself. */
    static const char EMPTY[] =
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    char actual[65];
    fr_sha256 context;
    fr_sha256_init(&context);
    fr_sha256_final(&context, actual);

    ASSERT_STR_EQ(EMPTY, actual);
    PASS();
}

TEST the_length_counter_survives_past_one_block(void) {
    /* 56 is the byte count at which the length field no longer fits in the
       final block, so the padding takes its second path. */
    char data[200];
    memset(data, 'z', sizeof data);
    static const size_t LENGTHS[] = { 55, 56, 57, 63, 64, 119, 120 };
    for (size_t index = 0; index < sizeof LENGTHS / sizeof LENGTHS[0]; index++) {
        char expected[65];
        char actual[65];
        fr_sha256_hex(data, LENGTHS[index], expected);
        incremental(data, LENGTHS[index], 7, actual);
        ASSERT_STR_EQ(expected, actual);
    }
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(hashes_the_empty_input);
    RUN_TEST(hashes_abc);
    RUN_TEST(hashes_a_multi_block_input);
    RUN_TEST(hashes_the_padding_boundary_lengths);
    RUN_TEST(the_incremental_digest_matches_the_one_shot_digest);
    RUN_TEST(the_incremental_digest_of_nothing_matches);
    RUN_TEST(the_length_counter_survives_past_one_block);
    GREATEST_MAIN_END();
}
