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
   started" are different answers and a caller must be able to tell them apart. */
int fr_exec_run(const fr_exec_request *request, fr_exec_result *out, fr_error *err);
void fr_exec_result_free(fr_exec_result *result);

/* Joins a program and its argument vector into the single command line
   CreateProcess requires, using the quoting rules the MSVC runtime parses
   back. Declared on every platform, not only Windows, because a function
   compiled only on Windows is a function tested only on Windows, and this is
   the one place in exec where a mistake is an argument-injection bug rather
   than a defect. */
int fr_exec_command_line(const char *program, const char *const *argv, size_t argv_count,
                         char **out_line, fr_error *err);

#endif
