#include "exec.h"

#include "error.h"

#include <stdlib.h>
#include <string.h>

static int needs_quoting(const char *argument) {
    return argument[0] == '\0' || strpbrk(argument, " \t\n\v\"") != NULL;
}

static void append_quoted(const char *argument, char *line, size_t *length) {
    line[(*length)++] = '"';
    for (const char *cursor = argument;; cursor++) {
        size_t slashes = 0;
        while (*cursor == '\\') { slashes++; cursor++; }
        if (*cursor == '\0') {
            for (size_t index = 0; index < slashes * 2; index++) line[(*length)++] = '\\';
            break;
        }
        if (*cursor == '"') {
            for (size_t index = 0; index < slashes * 2 + 1; index++) line[(*length)++] = '\\';
            line[(*length)++] = '"';
            continue;
        }
        for (size_t index = 0; index < slashes; index++) line[(*length)++] = '\\';
        line[(*length)++] = *cursor;
    }
    line[(*length)++] = '"';
}

int fr_exec_command_line(const char *program, const char *const *argv, size_t argv_count,
                         char **out_line, fr_error *err) {
    size_t bound = strlen(program) * 2 + 3;
    for (size_t index = 0; index < argv_count; index++) bound += strlen(argv[index]) * 2 + 3;

    char *line = malloc(bound + 1);
    if (line == NULL) {
        fr_error_set(err, "out of memory building a command line");
        return FR_ERR;
    }

    size_t length = 0;
    for (size_t index = 0; index <= argv_count; index++) {
        const char *argument = index == 0 ? program : argv[index - 1];
        if (index > 0) line[length++] = ' ';
        if (!needs_quoting(argument)) {
            for (const char *cursor = argument; *cursor != '\0'; cursor++) line[length++] = *cursor;
            continue;
        }
        append_quoted(argument, line, &length);
    }
    line[length] = '\0';

    *out_line = line;
    return FR_OK;
}
