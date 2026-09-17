#include "greatest.h"
#include "region.h"

#include <stdlib.h>
#include <string.h>

TEST replaces_between_the_markers(void) {
    const char *original =
        "dependencies {\n"
        "    // terko:begin\n"
        "    old line\n"
        "    // terko:end\n"
        "}\n";
    char *out = NULL; fr_error err;
    ASSERT_EQ(FR_OK, fr_region_replace(original, "// terko:begin", "// terko:end",
                                       "new line\nsecond line", &out, &err));
    ASSERT_STR_EQ(
        "dependencies {\n"
        "    // terko:begin\n"
        "    new line\n"
        "    second line\n"
        "    // terko:end\n"
        "}\n", out);
    free(out);
    PASS();
}

TEST empties_the_region_when_the_replacement_is_empty(void) {
    const char *original =
        "// terko:begin\n"
        "old\n"
        "// terko:end\n";
    char *out = NULL; fr_error err;
    ASSERT_EQ(FR_OK, fr_region_replace(original, "// terko:begin", "// terko:end", "", &out, &err));
    ASSERT_STR_EQ("// terko:begin\n// terko:end\n", out);
    free(out);
    PASS();
}

TEST leaves_text_outside_the_region_untouched(void) {
    const char *original =
        "before\n// terko:begin\nx\n// terko:end\nafter\n";
    char *out = NULL; fr_error err;
    fr_region_replace(original, "// terko:begin", "// terko:end", "y", &out, &err);
    ASSERT(strstr(out, "before\n") == out);
    ASSERT(strstr(out, "\nafter\n") != NULL);
    free(out);
    PASS();
}

TEST fails_when_the_begin_marker_is_absent(void) {
    char *out = NULL; fr_error err;
    ASSERT_EQ(FR_ERR, fr_region_replace("nothing here\n", "// terko:begin", "// terko:end", "y", &out, &err));
    ASSERT(strstr(err.message, "terko:begin") != NULL);
    PASS();
}

TEST fails_when_the_end_marker_is_absent(void) {
    char *out = NULL; fr_error err;
    ASSERT_EQ(FR_ERR, fr_region_replace("// terko:begin\nx\n", "// terko:begin", "// terko:end", "y", &out, &err));
    ASSERT(strstr(err.message, "terko:end") != NULL);
    PASS();
}

TEST fails_when_the_end_marker_precedes_the_begin_marker(void) {
    const char *original = "// terko:end\nx\n// terko:begin\n";
    char *out = NULL; fr_error err;
    ASSERT_EQ(FR_ERR, fr_region_replace(original, "// terko:begin", "// terko:end", "y", &out, &err));
    PASS();
}

TEST fails_when_both_markers_share_one_line(void) {
    const char *original = "dependencies {\n    // terko:begin // terko:end\n}\n";
    char *out = NULL; fr_error err;
    ASSERT_EQ(FR_ERR, fr_region_replace(original, "// terko:begin", "// terko:end", "X", &out, &err));
    ASSERT(out == NULL);
    ASSERT(strstr(err.message, "terko:begin") != NULL);
    ASSERT(strstr(err.message, "terko:end") != NULL);
    PASS();
}

TEST fails_when_the_replacement_carries_a_marker(void) {
    const char *original = "// terko:begin\nold\n// terko:end\n";
    char *out = NULL; fr_error err;
    ASSERT_EQ(FR_ERR, fr_region_replace(original, "// terko:begin", "// terko:end",
                                        "line\n// terko:end\nsmuggled", &out, &err));
    ASSERT(out == NULL);
    ASSERT(strstr(err.message, "terko:end") != NULL);
    PASS();
}

TEST joins_generated_lines_with_the_files_own_crlf_terminator(void) {
    const char *original =
        "dependencies {\r\n"
        "    // terko:begin\r\n"
        "    old line\r\n"
        "    // terko:end\r\n"
        "}\r\n";
    char *out = NULL; fr_error err;
    ASSERT_EQ(FR_OK, fr_region_replace(original, "// terko:begin", "// terko:end",
                                       "new line\nsecond line", &out, &err));
    ASSERT_STR_EQ(
        "dependencies {\r\n"
        "    // terko:begin\r\n"
        "    new line\r\n"
        "    second line\r\n"
        "    // terko:end\r\n"
        "}\r\n", out);
    free(out);
    PASS();
}

TEST trims_one_trailing_newline_from_the_replacement(void) {
    const char *original =
        "// terko:begin\n"
        "old\n"
        "// terko:end\n";
    char *out_with_newline = NULL;
    char *out_without_newline = NULL;
    fr_error err;
    ASSERT_EQ(FR_OK, fr_region_replace(original, "// terko:begin", "// terko:end",
                                       "line\n", &out_with_newline, &err));
    ASSERT_EQ(FR_OK, fr_region_replace(original, "// terko:begin", "// terko:end",
                                       "line", &out_without_newline, &err));
    ASSERT_STR_EQ(out_without_newline, out_with_newline);
    ASSERT_STR_EQ("// terko:begin\nline\n// terko:end\n", out_with_newline);
    free(out_with_newline);
    free(out_without_newline);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(replaces_between_the_markers);
    RUN_TEST(empties_the_region_when_the_replacement_is_empty);
    RUN_TEST(leaves_text_outside_the_region_untouched);
    RUN_TEST(fails_when_the_begin_marker_is_absent);
    RUN_TEST(fails_when_the_end_marker_is_absent);
    RUN_TEST(fails_when_the_end_marker_precedes_the_begin_marker);
    RUN_TEST(fails_when_both_markers_share_one_line);
    RUN_TEST(fails_when_the_replacement_carries_a_marker);
    RUN_TEST(joins_generated_lines_with_the_files_own_crlf_terminator);
    RUN_TEST(trims_one_trailing_newline_from_the_replacement);
    GREATEST_MAIN_END();
}
