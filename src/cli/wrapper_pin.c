#include "cli/wrapper_pin.h"

#include "util/strbuf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *skip_blanks(const char *cursor) {
    while (*cursor == ' ' || *cursor == '\t') cursor++;
    return cursor;
}

static int read_version_line(const char *line, char *out, size_t size) {
    const char *cursor = skip_blanks(line);
    if (strncmp(cursor, "version", 7) != 0) return 0;
    cursor = skip_blanks(cursor + 7);
    if (*cursor != '=') return 0;
    cursor = skip_blanks(cursor + 1);
    if (*cursor != '"') return 0;
    const char *value = cursor + 1;
    const char *close = value;
    while (*close != '\0' && *close != '"' && *close != '\n') close++;
    if (*close != '"') return 0;
    size_t length = (size_t) (close - value);
    if (length >= size) return 0;
    memcpy(out, value, length);
    out[length] = '\0';
    return 1;
}

int fr_wrapper_pin_version(const char *text, char *out, size_t size) {
    if (text == NULL || size == 0) return 0;
    for (const char *line = text; *line != '\0';) {
        if (*skip_blanks(line) == '[') return 0;
        if (read_version_line(line, out, size)) return 1;
        const char *next = strchr(line, '\n');
        if (next == NULL) break;
        line = next + 1;
    }
    return 0;
}

static int file_exists(const char *candidate) {
    FILE *file = fopen(candidate, "rb");
    if (file == NULL) return 0;
    fclose(file);
    return 1;
}

static int try_candidate(const char *candidate, fr_strbuf *searched, char **out_found) {
    if (!file_exists(candidate)) {
        fr_strbuf_append_format(searched, "%s\n", candidate);
        return 0;
    }
    *out_found = malloc(strlen(candidate) + 1);
    if (*out_found != NULL) strcpy(*out_found, candidate);
    return *out_found != NULL;
}

static int try_beside(const char *directory, const char *relative, fr_strbuf *searched,
                      char **out_found) {
    fr_strbuf joined;
    fr_strbuf_init(&joined);
    fr_strbuf_append_format(&joined, "%s/%s", directory, relative);
    char *candidate = fr_strbuf_release(&joined);
    if (candidate == NULL) return 0;
    int found = try_candidate(candidate, searched, out_found);
    free(candidate);
    return found;
}

int fr_wrapper_pin_find(const char *override, const char *executable_directory, char **out_found,
                        char **out_searched) {
    *out_found = NULL;
    *out_searched = NULL;
    fr_strbuf searched;
    fr_strbuf_init(&searched);

    int found = override != NULL && override[0] != '\0' &&
                try_candidate(override, &searched, out_found);
    if (!found && executable_directory != NULL) {
        found = try_beside(executable_directory, "wrapper.toml", &searched, out_found) ||
                try_beside(executable_directory, "../share/daukle/wrapper.toml", &searched,
                           out_found);
    }

    if (found) {
        fr_strbuf_free(&searched);
        return 1;
    }
    *out_searched = fr_strbuf_release(&searched);
    return 0;
}
