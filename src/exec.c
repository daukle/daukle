#include "exec.h"

#include "error.h"

#include <stdlib.h>
#include <string.h>

static int needs_quoting(const char *argument) {
    return argument[0] == '\0' || strpbrk(argument, " \t\n\v\"") != NULL;
}

/* Every byte of the command line is written through here so that an escaping
   rule which ever outgrows the two-output-bytes-per-input-byte worst case the
   buffer is sized for stops at the bound instead of running past it. */
static int put(char *line, size_t *length, size_t limit, char character) {
    if (*length >= limit) return 0;
    line[(*length)++] = character;
    return 1;
}

static int put_repeated(char *line, size_t *length, size_t limit, char character, size_t count) {
    for (size_t index = 0; index < count; index++) {
        if (!put(line, length, limit, character)) return 0;
    }
    return 1;
}

static int append_quoted(const char *argument, char *line, size_t *length, size_t limit) {
    if (!put(line, length, limit, '"')) return 0;
    for (const char *cursor = argument;; cursor++) {
        size_t slashes = 0;
        while (*cursor == '\\') { slashes++; cursor++; }
        if (*cursor == '\0') {
            if (!put_repeated(line, length, limit, '\\', slashes * 2)) return 0;
            break;
        }
        if (*cursor == '"') {
            if (!put_repeated(line, length, limit, '\\', slashes * 2 + 1)) return 0;
            if (!put(line, length, limit, '"')) return 0;
            continue;
        }
        if (!put_repeated(line, length, limit, '\\', slashes)) return 0;
        if (!put(line, length, limit, *cursor)) return 0;
    }
    return put(line, length, limit, '"');
}

void fr_exec_result_free(fr_exec_result *result) {
    free(result->stdout_text);
    free(result->stderr_text);
    result->stdout_text = NULL;
    result->stderr_text = NULL;
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
    int within_bound = 1;
    for (size_t index = 0; within_bound && index <= argv_count; index++) {
        const char *argument = index == 0 ? program : argv[index - 1];
        if (index > 0 && !put(line, &length, bound, ' ')) { within_bound = 0; break; }
        if (!needs_quoting(argument)) {
            for (const char *cursor = argument; *cursor != '\0'; cursor++) {
                if (!put(line, &length, bound, *cursor)) { within_bound = 0; break; }
            }
            continue;
        }
        within_bound = append_quoted(argument, line, &length, bound);
    }
    if (!within_bound) {
        free(line);
        fr_error_set(err, "the command line for \"%s\" outgrew the bound reserved for it", program);
        return FR_ERR;
    }
    line[length] = '\0';

    *out_line = line;
    return FR_OK;
}
