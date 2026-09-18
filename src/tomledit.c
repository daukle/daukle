#include "tomledit.h"

#include "error.h"
#include "strbuf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *line_after(const char *cursor) {
    const char *newline = strchr(cursor, '\n');
    return newline == NULL ? cursor + strlen(cursor) : newline + 1;
}

static int line_starts_with(const char *line, const char *prefix) {
    while (*line == ' ' || *line == '\t') line++;
    return strncmp(line, prefix, strlen(prefix)) == 0;
}

/* A key belongs to "[[consumers]]" itself only up to the first nested "[...]"
   header; past that point a line beginning "id = " belongs to whichever
   nested table it is in, not to the consumer, so a dependency table's own
   fields can never be mistaken for a consumer's id. */
static const char *find_consumer(const char *text, const char *consumer_id) {
    char needle[512];
    snprintf(needle, sizeof needle, "id = \"%s\"", consumer_id);
    for (const char *header = text; *header != '\0'; header = line_after(header)) {
        if (!line_starts_with(header, "[[consumers]]")) continue;
        for (const char *cursor = line_after(header); *cursor != '\0'; cursor = line_after(cursor)) {
            if (line_starts_with(cursor, "[")) break;
            if (line_starts_with(cursor, needle)) return header;
        }
    }
    return NULL;
}

static const char *end_of_consumer(const char *from) {
    for (const char *cursor = line_after(from); *cursor != '\0'; cursor = line_after(cursor)) {
        if (line_starts_with(cursor, "[[")) return cursor;
    }
    return from + strlen(from);
}

static const char *find_dependency(const char *from, const char *until, const char *project) {
    char needle[512];
    snprintf(needle, sizeof needle, "[consumers.dependencies.\"%s\"]", project);
    for (const char *cursor = from; cursor < until && *cursor != '\0'; cursor = line_after(cursor)) {
        if (line_starts_with(cursor, needle)) return cursor;
    }
    return NULL;
}

static const char *find_version_line(const char *from, const char *until) {
    for (const char *cursor = line_after(from); cursor < until && *cursor != '\0';
         cursor = line_after(cursor)) {
        if (line_starts_with(cursor, "version = ")) return cursor;
        if (line_starts_with(cursor, "[")) return NULL;
    }
    return NULL;
}

static int text_uses_crlf(const char *text) {
    const char *newline = strchr(text, '\n');
    return newline != NULL && newline != text && newline[-1] == '\r';
}

static void append_modules(fr_strbuf *buffer, const char *const *modules, size_t module_count,
                           const char *eol) {
    fr_strbuf_append(buffer, "  modules = [");
    for (size_t index = 0; index < module_count; index++) {
        if (index > 0) fr_strbuf_append(buffer, ", ");
        fr_strbuf_append_format(buffer, "\"%s\"", modules[index]);
    }
    fr_strbuf_append(buffer, "]");
    fr_strbuf_append(buffer, eol);
}

int fr_toml_edit_set_dependency(const char *text, const char *consumer_id, const char *project,
                                const char *range, const char *const *modules, size_t module_count,
                                char **out, fr_error *err) {
    *out = NULL;
    const char *consumer = find_consumer(text, consumer_id);
    if (consumer == NULL) {
        fr_error_set(err, "no consumer with id \"%s\"", consumer_id);
        return FR_ERR;
    }
    const char *consumer_end = end_of_consumer(consumer);
    const char *dependency = find_dependency(consumer, consumer_end, project);
    const char *eol = text_uses_crlf(text) ? "\r\n" : "\n";

    fr_strbuf buffer;
    fr_strbuf_init(&buffer);

    if (dependency != NULL) {
        const char *version_line = find_version_line(dependency, consumer_end);
        if (version_line == NULL) {
            fr_error_set(err, "\"%s\" under \"%s\" has no version line", project, consumer_id);
            fr_strbuf_free(&buffer);
            return FR_ERR;
        }
        fr_strbuf_append_bytes(&buffer, text, (size_t) (version_line - text));
        fr_strbuf_append_format(&buffer, "  version = \"%s\"", range);
        fr_strbuf_append(&buffer, eol);
        fr_strbuf_append(&buffer, line_after(version_line));
    } else {
        fr_strbuf_append_bytes(&buffer, text, (size_t) (consumer_end - text));
        fr_strbuf_append(&buffer, eol);
        fr_strbuf_append_format(&buffer, "  [consumers.dependencies.\"%s\"]", project);
        fr_strbuf_append(&buffer, eol);
        fr_strbuf_append_format(&buffer, "  version = \"%s\"", range);
        fr_strbuf_append(&buffer, eol);
        append_modules(&buffer, modules, module_count, eol);
        fr_strbuf_append(&buffer, consumer_end);
    }

    *out = fr_strbuf_release(&buffer);
    if (*out == NULL) {
        fr_error_set(err, "out of memory editing the manifest");
        return FR_ERR;
    }
    return FR_OK;
}
