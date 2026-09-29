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
    char expected[65];
    fr_sha256_hex("", 0, expected);

    char actual[65];
    fr_sha256 context;
    fr_sha256_init(&context);
    fr_sha256_final(&context, actual);

    ASSERT_STR_EQ(expected, actual);
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
    RUN_TEST(the_incremental_digest_matches_the_one_shot_digest);
    RUN_TEST(the_incremental_digest_of_nothing_matches);
    RUN_TEST(the_length_counter_survives_past_one_block);
    GREATEST_MAIN_END();
}
