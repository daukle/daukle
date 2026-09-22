#include "greatest.h"

#include "exec.h"
#include "support.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <signal.h>
#endif

/* The child half of this file. Spawned by the tests below as themselves, so a
   child process exists on every platform without depending on a shell, and so
   the argv the child received can be compared byte for byte with the argv the
   test passed. */
static int run_as_child(int argc, char **argv) {
#ifdef _WIN32
    /* Without this the CRT's default text mode turns every '\n' this child
       writes into "\r\n", and the byte-identical test below is comparing
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

#define BIG_STDERR_LENGTH 70000

/* Bigger than a typical pipe buffer (4 KiB on Windows, 64 KiB on Linux), so
   a reader that drains stdout to EOF before ever looking at stderr leaves
   this child blocked in write() on stderr and never reaches its own exit;
   the parent would then block forever reading stdout, which never reaches
   EOF because the child that owns the write end never closes it. */
static int run_as_big_stderr_child(void) {
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif
    printf("small stdout\n");
    fflush(stdout);
    static char big[BIG_STDERR_LENGTH];
    memset(big, 'e', sizeof big);
    fwrite(big, 1, sizeof big, stderr);
    fflush(stderr);
    return 0;
}

#define HUGE_STDOUT_LENGTH (FR_EXEC_CAPTURE_LIMIT + 64u * 1024u)

/* Past FR_EXEC_CAPTURE_LIMIT, so the bound and the truncated flag are load
   bearing here: delete either and this test stops passing. */
static int run_as_huge_stdout_child(void) {
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    static char block[64u * 1024u];
    memset(block, 'o', sizeof block);
    for (size_t written = 0; written < HUGE_STDOUT_LENGTH; written += sizeof block) {
        fwrite(block, 1, sizeof block, stdout);
    }
    fflush(stdout);
    return 0;
}

/* Death that is not an exit: the one place the two backends genuinely cannot
   report the same number, pinned per platform rather than left to be found. */
static int run_as_abnormal_child(void) {
#ifdef _WIN32
    TerminateProcess(GetCurrentProcess(), 0xC0000005u);
    return 0;
#else
    raise(SIGKILL);
    return 0;
#endif
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
    ASSERT(result.stdout_text != NULL);
    ASSERT(strstr(result.stdout_text, "[a b]\n") != NULL);
    ASSERT(strstr(result.stdout_text, "[a\"b]\n") != NULL);
    ASSERT(strstr(result.stdout_text, "[C:\\path with space\\]\n") != NULL);
    ASSERT(strstr(result.stdout_text, "[]\n") != NULL);
    ASSERT(strstr(result.stdout_text, "[plain]\n") != NULL);
    fr_exec_result_free(&result);
    PASS();
}

/* Regression for the deadlock where read_all() drained stdout to EOF before
   ever touching stderr: a child whose stderr pipe fills before it finishes
   never gets to close stdout, so the parent hangs. Both streams must be
   drained concurrently for this to complete at all. */
TEST captured_stderr_survives_a_full_stdout_pipe(void) {
    const char *argv[] = { "--exec-big-stderr" };
    fr_exec_result result; fr_error err;
    ASSERT_EQ(FR_OK, run(argv, 1, 1, &result, &err));
    ASSERT_EQ(0, result.code);
    ASSERT_EQ(0, result.truncated);
    ASSERT(result.stdout_text != NULL);
    ASSERT(strstr(result.stdout_text, "small stdout") != NULL);
    ASSERT(result.stderr_text != NULL);
    ASSERT_EQ((size_t) BIG_STDERR_LENGTH, strlen(result.stderr_text));
    fr_exec_result_free(&result);
    PASS();
}

TEST a_captured_stream_past_the_limit_is_truncated_and_says_so(void) {
    const char *argv[] = { "--exec-huge-stdout" };
    fr_exec_result result; fr_error err;
    ASSERT_EQ(FR_OK, run(argv, 1, 1, &result, &err));
    ASSERT_EQ(0, result.code);
    ASSERT_EQ(1, result.truncated);
    ASSERT(result.stdout_text != NULL);
    ASSERT_EQ((size_t) FR_EXEC_CAPTURE_LIMIT, strlen(result.stdout_text));
    fr_exec_result_free(&result);
    PASS();
}

/* Both backends turn a child exit of 127 into "could not be started". POSIX
   has no choice, execv failing in the forked child has no other channel;
   Windows follows it so the contract does not differ per platform. */
TEST a_child_exiting_127_is_reported_as_one_that_could_not_start(void) {
    const char *argv[] = { "--exec-child", "127" };
    fr_exec_result result; fr_error err;
    ASSERT_EQ(FR_ERR, run(argv, 2, 0, &result, &err));
    ASSERT(strstr(err.message, "could not be started") != NULL);
    PASS();
}

/* The documented divergence, pinned so a change to either backend is a failing
   test rather than a surprise in a plugin: POSIX answers 128 + the signal,
   Windows answers the exception code bit-identically as a negative int. */
TEST a_child_that_dies_abnormally_reports_its_platforms_code(void) {
    const char *argv[] = { "--exec-abnormal" };
    fr_exec_result result; fr_error err;
    ASSERT_EQ(FR_OK, run(argv, 1, 0, &result, &err));
#ifdef _WIN32
    ASSERT_EQ((int) 0xC0000005u, result.code);
    ASSERT(result.code < 0);
#else
    ASSERT_EQ(128 + SIGKILL, result.code);
#endif
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
    if (argc >= 2 && strcmp(argv[1], "--exec-big-stderr") == 0) return run_as_big_stderr_child();
    if (argc >= 2 && strcmp(argv[1], "--exec-huge-stdout") == 0) return run_as_huge_stdout_child();
    if (argc >= 2 && strcmp(argv[1], "--exec-abnormal") == 0) return run_as_abnormal_child();
    GREATEST_MAIN_BEGIN();
    RUN_TEST(a_child_exit_code_is_reported);
    RUN_TEST(capture_collects_both_streams);
    RUN_TEST(without_capture_the_streams_are_absent_rather_than_empty);
    RUN_TEST(every_argument_arrives_byte_identical);
    RUN_TEST(captured_stderr_survives_a_full_stdout_pipe);
    RUN_TEST(a_captured_stream_past_the_limit_is_truncated_and_says_so);
    RUN_TEST(a_child_exiting_127_is_reported_as_one_that_could_not_start);
    RUN_TEST(a_child_that_dies_abnormally_reports_its_platforms_code);
    RUN_TEST(a_program_that_does_not_exist_fails_naming_it);
    GREATEST_MAIN_END();
}
