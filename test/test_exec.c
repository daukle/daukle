#include "greatest.h"

#include "exec.h"
#include "support.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

/* The child half of this file. Spawned by the tests below as themselves, so a
   child process exists on every platform without depending on a shell, and so
   the argv the child received can be compared byte for byte with the argv the
   test passed. */
static int run_as_child(int argc, char **argv) {
#ifdef _WIN32
    /* Without this the CRT's default text mode turns every '\n' this child
       writes into "\r\n", and the byte-identical test above is comparing
       against a bare '\n'. */
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif
    int code = atoi(argv[2]);
    for (int index = 3; index < argc; index++) printf("[%s]\n", argv[index]);
    fflush(stdout);
    fprintf(stderr, "child stderr\n");
    fflush(stderr);
    return code;
}

/* argv[0] as ctest invokes it, kept so a test can spawn this binary again. */
static const char *self_path;

static int run(const char *const *argv, size_t count, int capture, fr_exec_result *out,
               fr_error *err) {
    fr_exec_request request;
    request.program = self_path;
    request.argv = argv;
    request.argv_count = count;
    request.cwd = NULL;
    request.capture = capture;
    return fr_exec_run(&request, out, err);
}

TEST a_child_exit_code_is_reported(void) {
    const char *argv[] = { "--exec-child", "3" };
    fr_exec_result result; fr_error err;
    ASSERT_EQ(FR_OK, run(argv, 2, 0, &result, &err));
    ASSERT_EQ(3, result.code);
    fr_exec_result_free(&result);
    PASS();
}

TEST capture_collects_both_streams(void) {
    const char *argv[] = { "--exec-child", "0", "hello" };
    fr_exec_result result; fr_error err;
    ASSERT_EQ(FR_OK, run(argv, 3, 1, &result, &err));
    ASSERT_EQ(0, result.code);
    ASSERT(result.stdout_text != NULL);
    ASSERT(strstr(result.stdout_text, "[hello]") != NULL);
    ASSERT(result.stderr_text != NULL);
    ASSERT(strstr(result.stderr_text, "child stderr") != NULL);
    fr_exec_result_free(&result);
    PASS();
}

TEST without_capture_the_streams_are_absent_rather_than_empty(void) {
    const char *argv[] = { "--exec-child", "0", "hello" };
    fr_exec_result result; fr_error err;
    ASSERT_EQ(FR_OK, run(argv, 3, 0, &result, &err));
    ASSERT(result.stdout_text == NULL);
    ASSERT(result.stderr_text == NULL);
    fr_exec_result_free(&result);
    PASS();
}

/* The point of spawning this binary as its own child: the child reports the
   argv it actually received, so a quoting bug is a diff and not a guess. */
TEST every_argument_arrives_byte_identical(void) {
    const char *argv[] = { "--exec-child", "0",
                           "a b", "a\"b", "C:\\path with space\\", "", "plain" };
    fr_exec_result result; fr_error err;
    ASSERT_EQ(FR_OK, run(argv, 7, 1, &result, &err));
    ASSERT(strstr(result.stdout_text, "[a b]\n") != NULL);
    ASSERT(strstr(result.stdout_text, "[a\"b]\n") != NULL);
    ASSERT(strstr(result.stdout_text, "[C:\\path with space\\]\n") != NULL);
    ASSERT(strstr(result.stdout_text, "[]\n") != NULL);
    ASSERT(strstr(result.stdout_text, "[plain]\n") != NULL);
    fr_exec_result_free(&result);
    PASS();
}

TEST a_program_that_does_not_exist_fails_naming_it(void) {
    fr_exec_request request;
    request.program = "daukle-no-such-program";
    request.argv = NULL;
    request.argv_count = 0;
    request.cwd = NULL;
    request.capture = 0;
    fr_exec_result result; fr_error err;
    ASSERT_EQ(FR_ERR, fr_exec_run(&request, &result, &err));
    ASSERT(strstr(err.message, "daukle-no-such-program") != NULL);
    ASSERT(strstr(err.message, "could not be started") != NULL);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    self_path = argv[0];
    if (argc >= 3 && strcmp(argv[1], "--exec-child") == 0) return run_as_child(argc, argv);
    GREATEST_MAIN_BEGIN();
    RUN_TEST(a_child_exit_code_is_reported);
    RUN_TEST(capture_collects_both_streams);
    RUN_TEST(without_capture_the_streams_are_absent_rather_than_empty);
    RUN_TEST(every_argument_arrives_byte_identical);
    RUN_TEST(a_program_that_does_not_exist_fails_naming_it);
    GREATEST_MAIN_END();
}
