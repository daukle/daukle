#ifndef DAUKLE_EXEC_H
#define DAUKLE_EXEC_H

#include "types.h"

#include <stddef.h>

/* Joins a program and its argument vector into the single command line
   CreateProcess requires, using the quoting rules the MSVC runtime parses
   back. Declared on every platform, not only Windows, because a function
   compiled only on Windows is a function tested only on Windows, and this is
   the one place in exec where a mistake is an argument-injection bug rather
   than a defect. */
int fr_exec_command_line(const char *program, const char *const *argv, size_t argv_count,
                         char **out_line, fr_error *err);

#endif
