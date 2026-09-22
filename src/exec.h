#ifndef DAUKLE_EXEC_H
#define DAUKLE_EXEC_H

#include "types.h"

#include <stddef.h>

#define FR_EXEC_CAPTURE_LIMIT (1024u * 1024u)

typedef struct {
    const char *program;
    const char *const *argv;
    size_t argv_count;
    const char *cwd;
    int capture;
} fr_exec_request;

typedef struct {
    int code;
    char *stdout_text;
    char *stderr_text;
    int truncated;
} fr_exec_result;

/* FR_ERR means the child never ran. A child that ran and failed is FR_OK with
   a nonzero code, because "the tool said no" and "the tool could not be
   started" are different answers and a caller must be able to tell them apart.

   Exit code 127 is the one code both platforms report as FR_ERR "could not be
   started" rather than as a code. POSIX has no choice: execv fails inside the
   forked child, which cannot report errno back across the fork, so 127 is the
   only channel it has. Windows could tell the two apart, and deliberately does
   not, because a contract that differs per platform is one a plugin author
   discovers from a bug report.

   Death by signal is the one thing the two cannot agree on: POSIX reports
   128 + the signal number, Windows reports the exception code bit-identically
   as a negative int (0xC0000005 arrives as -1073741819). Both are nonzero, so
   `check` behaves the same; only the number differs. */
int fr_exec_run(const fr_exec_request *request, fr_exec_result *out, fr_error *err);
void fr_exec_result_free(fr_exec_result *result);

/* Joins a program and its argument vector into the single command line
   CreateProcess requires, using the quoting rules the MSVC runtime parses
   back. Declared on every platform, not only Windows, because a function
   compiled only on Windows is a function tested only on Windows, and this is
   the one place in exec where a mistake is an argument-injection bug rather
   than a defect. The buffer it allocates is sized on the invariant that the
   escaping emits at most two output bytes per input byte, so an escaping rule
   that ever exceeds that fails here with FR_ERR instead of overflowing. */
int fr_exec_command_line(const char *program, const char *const *argv, size_t argv_count,
                         char **out_line, fr_error *err);

#endif
