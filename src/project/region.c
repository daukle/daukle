#include "project/region.h"

#include "util/error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *line_start(const char *base, const char *pos) {
    while (pos > base && pos[-1] != '\n') pos--;
    return pos;
}

/* Called once with dest == NULL to size the output, then once more to fill it: the same
   loop drives both passes, so they cannot disagree about how many bytes a line contributes. */
static size_t emit_region_body(char *dest, const char *replacement, size_t replacement_len,
                               const char *indent_start, size_t indent_len,
                               const char *terminator, size_t terminator_len) {
    if (replacement_len == 0) return 0;

    size_t written = 0;
    const char *cursor = replacement;
    const char *replacement_end = replacement + replacement_len;
    for (;;) {
        const char *newline = memchr(cursor, '\n', (size_t) (replacement_end - cursor));
        size_t line_len = newline != NULL ? (size_t) (newline - cursor) : (size_t) (replacement_end - cursor);

        if (dest != NULL) memcpy(dest + written, indent_start, indent_len);
        written += indent_len;
        if (dest != NULL) memcpy(dest + written, cursor, line_len);
        written += line_len;
        if (dest != NULL) memcpy(dest + written, terminator, terminator_len);
        written += terminator_len;

        if (newline == NULL) break;
        cursor = newline + 1;
    }
    return written;
}

static size_t trim_one_trailing_terminator(const char *text, size_t length) {
    if (length >= 2 && text[length - 2] == '\r' && text[length - 1] == '\n') return length - 2;
    if (length >= 1 && text[length - 1] == '\n') return length - 1;
    return length;
}

int fr_region_replace(const char *original, const char *begin_marker, const char *end_marker,
                      const char *replacement, char **out, fr_error *err) {
    *out = NULL;

    if (strstr(replacement, begin_marker) != NULL) {
        fr_error_set(err, "replacement contains the region marker \"%s\"", begin_marker);
        return FR_ERR;
    }
    if (strstr(replacement, end_marker) != NULL) {
        fr_error_set(err, "replacement contains the region marker \"%s\"", end_marker);
        return FR_ERR;
    }

    const char *begin_pos = strstr(original, begin_marker);
    if (begin_pos == NULL) {
        fr_error_set(err, "region marker \"%s\" not found", begin_marker);
        return FR_ERR;
    }

    const char *begin_marker_end = begin_pos + strlen(begin_marker);
    const char *end_pos = strstr(begin_marker_end, end_marker);
    if (end_pos == NULL) {
        fr_error_set(err, "region marker \"%s\" not found after \"%s\"", end_marker, begin_marker);
        return FR_ERR;
    }

    const char *indent_start = line_start(original, begin_pos);
    size_t indent_len = (size_t) (begin_pos - indent_start);

    const char *begin_newline = strchr(begin_marker_end, '\n');
    const char *prefix_end = begin_newline != NULL ? begin_newline + 1 : begin_marker_end;
    size_t prefix_len = (size_t) (prefix_end - original);

    int uses_crlf = begin_newline != NULL && begin_newline > original && begin_newline[-1] == '\r';
    const char *terminator = uses_crlf ? "\r\n" : "\n";
    size_t terminator_len = uses_crlf ? 2 : 1;

    const char *suffix_start = line_start(original, end_pos);
    if (suffix_start < prefix_end) {
        fr_error_set(err, "region markers \"%s\" and \"%s\" share one line", begin_marker, end_marker);
        return FR_ERR;
    }
    size_t suffix_len = strlen(suffix_start);

    size_t replacement_len = trim_one_trailing_terminator(replacement, strlen(replacement));
    size_t body_len = emit_region_body(NULL, replacement, replacement_len, indent_start, indent_len,
                                       terminator, terminator_len);

    char *buffer = malloc(prefix_len + body_len + suffix_len + 1);
    if (buffer == NULL) {
        fr_error_set(err, "out of memory replacing region");
        return FR_ERR;
    }

    memcpy(buffer, original, prefix_len);
    emit_region_body(buffer + prefix_len, replacement, replacement_len, indent_start, indent_len,
                     terminator, terminator_len);
    memcpy(buffer + prefix_len + body_len, suffix_start, suffix_len);
    buffer[prefix_len + body_len + suffix_len] = '\0';

    *out = buffer;
    return FR_OK;
}

int fr_file_read_text(const char *path, char **out, fr_error *err) {
    return fr_file_read_bytes(path, out, NULL, err);
}

int fr_file_read_bytes(const char *path, char **out, size_t *out_length, fr_error *err) {
    *out = NULL;
    if (out_length != NULL) *out_length = 0;
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        fr_error_set(err, "cannot open \"%s\"", path);
        return FR_ERR;
    }
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (size < 0) {
        fclose(file);
        fr_error_set(err, "cannot size \"%s\"", path);
        return FR_ERR;
    }

    char *buffer = malloc((size_t) size + 1);
    if (buffer == NULL) {
        fclose(file);
        fr_error_set(err, "out of memory reading \"%s\"", path);
        return FR_ERR;
    }
    size_t read = fread(buffer, 1, (size_t) size, file);
    fclose(file);
    if (read != (size_t) size) {
        free(buffer);
        fr_error_set(err, "cannot read \"%s\"", path);
        return FR_ERR;
    }
    buffer[read] = '\0';

    *out = buffer;
    if (out_length != NULL) *out_length = read;
    return FR_OK;
}

int fr_file_write_text(const char *path, const char *text, fr_error *err) {
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        fr_error_set(err, "cannot open \"%s\" for writing", path);
        return FR_ERR;
    }
    size_t length = strlen(text);
    size_t written = fwrite(text, 1, length, file);
    fclose(file);
    if (written != length) {
        fr_error_set(err, "cannot write \"%s\"", path);
        return FR_ERR;
    }
    return FR_OK;
}
