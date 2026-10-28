#include "greatest.h"

#include "cli/wrapper_scripts.h"
#include "project/region.h"
#include "util/types.h"

#include <stdlib.h>
#include <string.h>

static enum greatest_test_res expect_embedded(const char *path, const unsigned char *embedded,
                                              size_t embedded_length) {
    fr_error err;
    char *bytes = NULL;
    size_t length = 0;
    ASSERT_EQ(FR_OK, fr_file_read_bytes(path, &bytes, &length, &err));
    ASSERT_EQ(length, embedded_length);
    ASSERT_MEM_EQ(bytes, embedded, length);
    free(bytes);
    PASS();
}

TEST the_posix_script_is_embedded_byte_for_byte(void) {
    CHECK_CALL(expect_embedded("wrapper/daukle", fr_wrapper_posix_script,
                               fr_wrapper_posix_script_length));
    PASS();
}

TEST the_powershell_script_is_embedded_byte_for_byte(void) {
    CHECK_CALL(expect_embedded("wrapper/daukle.ps1", fr_wrapper_powershell_script,
                               fr_wrapper_powershell_script_length));
    PASS();
}

TEST the_posix_script_keeps_lf(void) {
    ASSERT(memchr(fr_wrapper_posix_script, '\r', fr_wrapper_posix_script_length) == NULL);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(the_posix_script_is_embedded_byte_for_byte);
    RUN_TEST(the_powershell_script_is_embedded_byte_for_byte);
    RUN_TEST(the_posix_script_keeps_lf);
    GREATEST_MAIN_END();
}
