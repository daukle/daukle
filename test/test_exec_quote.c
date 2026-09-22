#include "greatest.h"

#include "exec.h"

#include <stdlib.h>
#include <string.h>

static char *join(const char *program, const char *const *argv, size_t count) {
    char *line = NULL;
    fr_error err;
    if (fr_exec_command_line(program, argv, count, &line, &err) != FR_OK) return NULL;
    return line;
}

/* The reserved bound is two output bytes per input byte plus three per token,
   and fr_exec_command_line refuses rather than overflowing if the escaping
   ever needs more. This is the worst case the current rules can produce. */
TEST the_worst_case_escaping_stays_inside_the_reserved_bound(void) {
    const char *argv[] = { "\\\\\\\\\\\\\\\\\"", "\"\"\"\"\"\"", "\\\\\\\\", "a b\\\\" };
    const char *program = "C:\\a\\\\b\"";
    char *line = join(program, argv, 4);
    ASSERT(line != NULL);

    size_t bound = strlen(program) * 2 + 3;
    for (size_t index = 0; index < 4; index++) bound += strlen(argv[index]) * 2 + 3;
    ASSERT(strlen(line) <= bound);

    free(line);
    PASS();
}

TEST a_plain_argument_is_not_quoted(void) {
    const char *argv[] = { "build" };
    char *line = join("gradle", argv, 1);
    ASSERT(line != NULL);
    ASSERT_STR_EQ("gradle build", line);
    free(line);
    PASS();
}

TEST an_argument_containing_a_space_is_quoted(void) {
    const char *argv[] = { "a b" };
    char *line = join("tool", argv, 1);
    ASSERT(line != NULL);
    ASSERT_STR_EQ("tool \"a b\"", line);
    free(line);
    PASS();
}

TEST a_double_quote_is_escaped_with_a_backslash(void) {
    const char *argv[] = { "a\"b" };
    char *line = join("tool", argv, 1);
    ASSERT(line != NULL);
    ASSERT_STR_EQ("tool \"a\\\"b\"", line);
    free(line);
    PASS();
}

TEST a_trailing_backslash_is_doubled_so_it_does_not_escape_the_closing_quote(void) {
    const char *argv[] = { "C:\\path with space\\" };
    char *line = join("tool", argv, 1);
    ASSERT(line != NULL);
    ASSERT_STR_EQ("tool \"C:\\path with space\\\\\"", line);
    free(line);
    PASS();
}

TEST backslashes_before_a_quote_are_doubled_and_the_quote_escaped(void) {
    const char *argv[] = { "a\\\\\"b" };
    char *line = join("tool", argv, 1);
    ASSERT(line != NULL);
    ASSERT_STR_EQ("tool \"a\\\\\\\\\\\"b\"", line);
    free(line);
    PASS();
}

TEST an_empty_argument_survives_as_an_empty_quoted_string(void) {
    const char *argv[] = { "", "after" };
    char *line = join("tool", argv, 2);
    ASSERT(line != NULL);
    ASSERT_STR_EQ("tool \"\" after", line);
    free(line);
    PASS();
}

TEST a_program_path_containing_a_space_is_quoted(void) {
    char *line = join("C:\\Program Files\\t.exe", NULL, 0);
    ASSERT(line != NULL);
    ASSERT_STR_EQ("\"C:\\Program Files\\t.exe\"", line);
    free(line);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(the_worst_case_escaping_stays_inside_the_reserved_bound);
    RUN_TEST(a_plain_argument_is_not_quoted);
    RUN_TEST(an_argument_containing_a_space_is_quoted);
    RUN_TEST(a_double_quote_is_escaped_with_a_backslash);
    RUN_TEST(a_trailing_backslash_is_doubled_so_it_does_not_escape_the_closing_quote);
    RUN_TEST(backslashes_before_a_quote_are_doubled_and_the_quote_escaped);
    RUN_TEST(an_empty_argument_survives_as_an_empty_quoted_string);
    RUN_TEST(a_program_path_containing_a_space_is_quoted);
    GREATEST_MAIN_END();
}
