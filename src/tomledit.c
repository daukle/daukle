#include "tomledit.h"

#include "config_toml.h"
#include "error.h"
#include "strbuf.h"

#include "cJSON.h"

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

static int build_needle(char *buffer, size_t size, const char *format, const char *value,
                        const char *what, fr_error *err) {
    int written = snprintf(buffer, size, format, value);
    if (written < 0 || (size_t) written >= size) {
        fr_error_set(err, "the %s is too long (%d characters)", what, (int) strlen(value));
        return FR_ERR;
    }
    return FR_OK;
}

/* A key belongs to "[[consumers]]" itself only up to the first nested "[...]"
   header; past that point a line beginning "id = " belongs to whichever
   nested table it is in, not to the consumer, so a dependency table's own
   fields can never be mistaken for a consumer's id. */
static int find_consumer(const char *text, const char *consumer_id, const char **out,
                         fr_error *err) {
    char needle[512];
    if (build_needle(needle, sizeof needle, "id = \"%s\"", consumer_id, "consumer id", err) != FR_OK) {
        return FR_ERR;
    }
    for (const char *header = text; *header != '\0'; header = line_after(header)) {
        if (!line_starts_with(header, "[[consumers]]")) continue;
        for (const char *cursor = line_after(header); *cursor != '\0'; cursor = line_after(cursor)) {
            if (line_starts_with(cursor, "[")) break;
            if (line_starts_with(cursor, needle)) {
                *out = header;
                return FR_OK;
            }
        }
    }
    *out = NULL;
    return FR_OK;
}

static const char *end_of_consumer(const char *from) {
    for (const char *cursor = line_after(from); *cursor != '\0'; cursor = line_after(cursor)) {
        if (line_starts_with(cursor, "[[")) return cursor;
    }
    return from + strlen(from);
}

static int find_dependency(const char *from, const char *until, const char *project,
                           const char **out, fr_error *err) {
    char needle[512];
    if (build_needle(needle, sizeof needle, "[consumers.dependencies.\"%s\"]", project,
                     "project name", err) != FR_OK) {
        return FR_ERR;
    }
    for (const char *cursor = from; cursor < until && *cursor != '\0'; cursor = line_after(cursor)) {
        if (line_starts_with(cursor, needle)) {
            *out = cursor;
            return FR_OK;
        }
    }
    *out = NULL;
    return FR_OK;
}

static const char *find_version_line(const char *from, const char *until) {
    for (const char *cursor = line_after(from); cursor < until && *cursor != '\0';
         cursor = line_after(cursor)) {
        if (line_starts_with(cursor, "version = ")) return cursor;
        if (line_starts_with(cursor, "[")) return NULL;
    }
    return NULL;
}

static const char *find_quote(const char *from) {
    for (const char *cursor = from; *cursor != '\0' && *cursor != '\n'; cursor++) {
        if (*cursor == '"') return cursor;
    }
    return NULL;
}

static int text_uses_crlf(const char *text) {
    const char *newline = strchr(text, '\n');
    return newline != NULL && newline != text && newline[-1] == '\r';
}

static char *leading_whitespace(const char *line_start) {
    const char *cursor = line_start;
    while (*cursor == ' ' || *cursor == '\t') cursor++;
    fr_strbuf buffer;
    fr_strbuf_init(&buffer);
    fr_strbuf_append_bytes(&buffer, line_start, (size_t) (cursor - line_start));
    return fr_strbuf_release(&buffer);
}

/* jsonedit.c detects a document's indent the same way: the first line that has
   leading whitespace before a real character sets the unit. A consumer's own
   keys give the depth to indent from, since consumer_id lines are read at
   whatever depth the surrounding document already uses for them. */
static char *indent_unit(const char *text) {
    fr_strbuf buffer;
    fr_strbuf_init(&buffer);
    for (size_t index = 0; text[index] != '\0'; index++) {
        if (text[index] != '\n') continue;
        size_t start = index + 1;
        size_t cursor = start;
        while (text[cursor] == ' ' || text[cursor] == '\t') cursor++;
        if (cursor > start && text[cursor] != '\0' && text[cursor] != '\n' && text[cursor] != '\r') {
            fr_strbuf_append_bytes(&buffer, text + start, cursor - start);
            return fr_strbuf_release(&buffer);
        }
    }
    fr_strbuf_append(&buffer, "  ");
    return fr_strbuf_release(&buffer);
}

static int build_member_indent(const char *text, const char *consumer, char **out, fr_error *err) {
    char *unit = indent_unit(text);
    if (unit == NULL) {
        fr_error_set(err, "out of memory editing the manifest");
        return FR_ERR;
    }
    char *base = leading_whitespace(consumer);
    if (base == NULL) {
        free(unit);
        fr_error_set(err, "out of memory editing the manifest");
        return FR_ERR;
    }

    fr_strbuf buffer;
    fr_strbuf_init(&buffer);
    fr_strbuf_append(&buffer, base);
    fr_strbuf_append(&buffer, unit);
    free(base);
    free(unit);

    *out = fr_strbuf_release(&buffer);
    if (*out == NULL) {
        fr_error_set(err, "out of memory editing the manifest");
        return FR_ERR;
    }
    return FR_OK;
}

static void append_modules(fr_strbuf *buffer, const char *const *modules, size_t module_count,
                           const char *eol, const char *indent) {
    fr_strbuf_append(buffer, indent);
    fr_strbuf_append(buffer, "modules = [");
    for (size_t index = 0; index < module_count; index++) {
        if (index > 0) fr_strbuf_append(buffer, ", ");
        fr_strbuf_append_format(buffer, "\"%s\"", modules[index]);
    }
    fr_strbuf_append(buffer, "]");
    fr_strbuf_append(buffer, eol);
}

/* An existing dependency's version is spliced in place; its module list is not,
   because rewriting an array that may span lines is a second span finder for no
   demand yet. Refusing is the honest half of that: a module list handed in here
   would otherwise be accepted and dropped. */
static int update_version(const char *text, const char *dependency, const char *consumer_end,
                          const char *project, const char *consumer_id, const char *range,
                          size_t module_count, fr_strbuf *buffer, fr_error *err) {
    if (module_count > 0) {
        fr_error_set(err, "\"%s\" is already a dependency of \"%s\" and its modules are left as"
                          " they are; drop the module list to update its version alone",
                     project, consumer_id);
        return FR_ERR;
    }
    const char *version_line = find_version_line(dependency, consumer_end);
    if (version_line == NULL) {
        fr_error_set(err, "\"%s\" under \"%s\" has no version line", project, consumer_id);
        return FR_ERR;
    }
    const char *value_open = find_quote(version_line);
    const char *value_close = value_open != NULL ? find_quote(value_open + 1) : NULL;
    if (value_close == NULL) {
        fr_error_set(err, "\"%s\" under \"%s\" has a version line without a quoted value",
                    project, consumer_id);
        return FR_ERR;
    }
    fr_strbuf_append_bytes(buffer, text, (size_t) (value_open + 1 - text));
    fr_strbuf_append(buffer, range);
    fr_strbuf_append(buffer, value_close);
    return FR_OK;
}

static int append_dependency(const char *text, const char *consumer, const char *consumer_end,
                             const char *project, const char *range, const char *const *modules,
                             size_t module_count, const char *eol, fr_strbuf *buffer, fr_error *err) {
    char *member_indent = NULL;
    if (build_member_indent(text, consumer, &member_indent, err) != FR_OK) return FR_ERR;

    const char *separator = (consumer_end > text && consumer_end[-1] == '\n') ? "" : eol;
    fr_strbuf_append_bytes(buffer, text, (size_t) (consumer_end - text));
    fr_strbuf_append(buffer, separator);
    fr_strbuf_append(buffer, member_indent);
    fr_strbuf_append_format(buffer, "[consumers.dependencies.\"%s\"]", project);
    fr_strbuf_append(buffer, eol);
    fr_strbuf_append(buffer, member_indent);
    fr_strbuf_append_format(buffer, "version = \"%s\"", range);
    fr_strbuf_append(buffer, eol);
    append_modules(buffer, modules, module_count, eol, member_indent);
    fr_strbuf_append(buffer, consumer_end);

    free(member_indent);
    return FR_OK;
}

/* The span finders match one spelling of each line they look for, and a legal
   manifest may write another; a missed dependency then appends a second header
   for a table that already exists, which TOML 1.0 rejects. Reading the result
   back is the one check that holds whatever a finder misses, so no caller can
   be handed text it would be wrong to write. */
static int still_parses(const char *text, fr_error *err) {
    cJSON *document = NULL;
    if (FR_CONFIG_TOML.load(FR_CONFIG_TOML.state, text, "the edited manifest", NULL, NULL, NULL,
                            &document, err) != FR_OK) {
        return FR_ERR;
    }
    cJSON_Delete(document);
    return FR_OK;
}

int fr_toml_edit_set_dependency(const char *text, const char *consumer_id, const char *project,
                                const char *range, const char *const *modules, size_t module_count,
                                char **out, fr_error *err) {
    *out = NULL;
    const char *consumer = NULL;
    if (find_consumer(text, consumer_id, &consumer, err) != FR_OK) return FR_ERR;
    if (consumer == NULL) {
        fr_error_set(err, "no consumer with id \"%s\"", consumer_id);
        return FR_ERR;
    }
    const char *consumer_end = end_of_consumer(consumer);
    const char *dependency = NULL;
    if (find_dependency(consumer, consumer_end, project, &dependency, err) != FR_OK) return FR_ERR;
    const char *eol = text_uses_crlf(text) ? "\r\n" : "\n";

    fr_strbuf buffer;
    fr_strbuf_init(&buffer);

    int status = dependency != NULL
        ? update_version(text, dependency, consumer_end, project, consumer_id, range,
                         module_count, &buffer, err)
        : append_dependency(text, consumer, consumer_end, project, range, modules, module_count,
                            eol, &buffer, err);
    if (status != FR_OK) {
        fr_strbuf_free(&buffer);
        return FR_ERR;
    }

    *out = fr_strbuf_release(&buffer);
    if (*out == NULL) {
        fr_error_set(err, "out of memory editing the manifest");
        return FR_ERR;
    }
    if (still_parses(*out, err) != FR_OK) {
        free(*out);
        *out = NULL;
        return FR_ERR;
    }
    return FR_OK;
}
