#include "config/jsonedit.h"
#include "config/jsonedit_internal.h"

#include "util/error.h"
#include "util/strbuf.h"

#include <stdlib.h>
#include <string.h>

#define MAX_SEGMENTS 8
#define MAX_SEGMENT_LENGTH 128

typedef enum { VALUE_STRING, VALUE_STRING_ARRAY, VALUE_EMPTY_OBJECT } value_kind;

typedef struct {
    value_kind kind;
    const char *scalar;
    const char *const *values;
    size_t count;
} edit_value;

static void append_json_string(fr_strbuf *buffer, const char *value) {
    fr_strbuf_append(buffer, "\"");
    for (const unsigned char *cursor = (const unsigned char *) value; *cursor != '\0'; cursor++) {
        switch (*cursor) {
            case '"': fr_strbuf_append(buffer, "\\\""); break;
            case '\\': fr_strbuf_append(buffer, "\\\\"); break;
            case '\n': fr_strbuf_append(buffer, "\\n"); break;
            case '\r': fr_strbuf_append(buffer, "\\r"); break;
            case '\t': fr_strbuf_append(buffer, "\\t"); break;
            default:
                if (*cursor < 0x20) fr_strbuf_append_format(buffer, "\\u%04x", (unsigned) *cursor);
                else fr_strbuf_append_bytes(buffer, (const char *) cursor, 1);
        }
    }
    fr_strbuf_append(buffer, "\"");
}

static void append_value(fr_strbuf *buffer, const edit_value *value,
                         const char *member_indent, const char *unit) {
    if (value->kind == VALUE_EMPTY_OBJECT) {
        fr_strbuf_append(buffer, "{}");
        return;
    }
    if (value->kind == VALUE_STRING) {
        append_json_string(buffer, value->scalar);
        return;
    }
    if (value->count == 0) {
        fr_strbuf_append(buffer, "[]");
        return;
    }
    fr_strbuf_append(buffer, "[\n");
    for (size_t index = 0; index < value->count; index++) {
        fr_strbuf_append(buffer, member_indent);
        fr_strbuf_append(buffer, unit);
        append_json_string(buffer, value->values[index]);
        fr_strbuf_append(buffer, index + 1 < value->count ? ",\n" : "\n");
    }
    fr_strbuf_append(buffer, member_indent);
    fr_strbuf_append(buffer, "]");
}

static char *splice(const char *text, size_t start, size_t end, const char *insert) {
    fr_strbuf buffer;
    fr_strbuf_init(&buffer);
    fr_strbuf_append_bytes(&buffer, text, start);
    fr_strbuf_append(&buffer, insert);
    fr_strbuf_append(&buffer, text + end);
    return fr_strbuf_release(&buffer);
}

static char *duplicate(const char *text) {
    return splice(text, 0, 0, "");
}

static int finish(char *result, char **out, fr_error *err) {
    if (result == NULL) {
        fr_error_set(err, "out of memory editing the json document");
        return FR_ERR;
    }
    *out = result;
    return FR_OK;
}

static int replace_existing(const char *text, const member *existing, const edit_value *value,
                            const char *unit, char **out, fr_error *err) {
    char *indent = fr_json_edit_leading_whitespace(text, existing->key_start);
    if (indent == NULL) return finish(NULL, out, err);

    fr_strbuf rendered;
    fr_strbuf_init(&rendered);
    append_value(&rendered, value, indent, unit);
    char *rendered_text = fr_strbuf_release(&rendered);
    free(indent);
    if (rendered_text == NULL) return finish(NULL, out, err);

    char *result = splice(text, existing->value_start, existing->value_end, rendered_text);
    free(rendered_text);
    return finish(result, out, err);
}

static int append_after_last(const char *text, const member *last, const char *key,
                             const edit_value *value, const char *unit, char **out, fr_error *err) {
    int own_line = fr_json_edit_starts_its_line(text, last->key_start);
    char *indent = own_line ? fr_json_edit_leading_whitespace(text, last->key_start) : NULL;
    if (own_line && indent == NULL) return finish(NULL, out, err);

    fr_strbuf insertion;
    fr_strbuf_init(&insertion);
    fr_strbuf_append(&insertion, own_line ? ",\n" : ", ");
    if (own_line) fr_strbuf_append(&insertion, indent);
    append_json_string(&insertion, key);
    fr_strbuf_append(&insertion, ": ");
    append_value(&insertion, value, own_line ? indent : "", unit);
    char *insertion_text = fr_strbuf_release(&insertion);
    free(indent);
    if (insertion_text == NULL) return finish(NULL, out, err);

    char *result = splice(text, last->value_end, last->value_end, insertion_text);
    free(insertion_text);
    return finish(result, out, err);
}

static int fill_empty_object(const char *text, size_t brace, const char *key,
                             const edit_value *value, const char *unit, char **out, fr_error *err) {
    char *closing_indent = fr_json_edit_leading_whitespace(text, brace);
    if (closing_indent == NULL) return finish(NULL, out, err);

    fr_strbuf member_indent;
    fr_strbuf_init(&member_indent);
    fr_strbuf_append(&member_indent, closing_indent);
    fr_strbuf_append(&member_indent, unit);
    char *member_indent_text = fr_strbuf_release(&member_indent);
    if (member_indent_text == NULL) {
        free(closing_indent);
        return finish(NULL, out, err);
    }

    fr_strbuf insertion;
    fr_strbuf_init(&insertion);
    fr_strbuf_append(&insertion, "\n");
    fr_strbuf_append(&insertion, member_indent_text);
    append_json_string(&insertion, key);
    fr_strbuf_append(&insertion, ": ");
    append_value(&insertion, value, member_indent_text, unit);
    fr_strbuf_append(&insertion, "\n");
    fr_strbuf_append(&insertion, closing_indent);
    char *insertion_text = fr_strbuf_release(&insertion);
    free(member_indent_text);
    free(closing_indent);
    if (insertion_text == NULL) return finish(NULL, out, err);

    char *result = splice(text, brace + 1, fr_json_edit_skip_value(text, brace) - 1, insertion_text);
    free(insertion_text);
    return finish(result, out, err);
}

static int set_member(const char *text, size_t brace, const char *key, const edit_value *value,
                      char **out, fr_error *err) {
    char *unit = fr_json_edit_indent_unit(text);
    if (unit == NULL) return finish(NULL, out, err);

    int status;
    member existing;
    member last;
    if (fr_json_edit_find_member(text, brace, key, &existing)) {
        status = replace_existing(text, &existing, value, unit, out, err);
    } else if (fr_json_edit_last_member(text, brace, &last)) {
        status = append_after_last(text, &last, key, value, unit, out, err);
    } else {
        status = fill_empty_object(text, brace, key, value, unit, out, err);
    }
    free(unit);
    return status;
}

static int remove_member(const char *text, size_t brace, const char *key, char **out, fr_error *err) {
    member target;
    if (!fr_json_edit_find_member(text, brace, key, &target)) {
        return finish(duplicate(text), out, err);
    }

    size_t previous_end = brace + 1;
    int has_previous = 0;
    member entry;
    fr_json_edit_first_member(text, brace, &entry);
    while (entry.key_start != target.key_start) {
        previous_end = entry.value_end;
        has_previous = 1;
        member following;
        fr_json_edit_next_member(text, &entry, &following);
        entry = following;
    }

    member following;
    size_t cut_start;
    size_t cut_end;
    if (fr_json_edit_next_member(text, &target, &following)) {
        cut_start = target.key_start;
        cut_end = following.key_start;
    } else if (has_previous) {
        cut_start = previous_end;
        cut_end = target.value_end;
    } else {
        cut_start = brace + 1;
        cut_end = fr_json_edit_skip_value(text, brace) - 1;
    }
    return finish(splice(text, cut_start, cut_end, ""), out, err);
}

static int split_path(const char *path, char storage[][MAX_SEGMENT_LENGTH], size_t *count,
                      fr_error *err) {
    *count = 0;
    if (path == NULL || path[0] == '\0') return FR_OK;

    const char *cursor = path;
    for (;;) {
        const char *dot = strchr(cursor, '.');
        size_t length = dot != NULL ? (size_t) (dot - cursor) : strlen(cursor);
        if (length == 0 || length >= MAX_SEGMENT_LENGTH) {
            fr_error_set(err, "path \"%s\" has an empty or oversized segment", path);
            return FR_ERR;
        }
        if (*count == MAX_SEGMENTS) {
            fr_error_set(err, "path \"%s\" is nested deeper than %d", path, MAX_SEGMENTS);
            return FR_ERR;
        }
        memcpy(storage[*count], cursor, length);
        storage[(*count)++][length] = '\0';
        if (dot == NULL) return FR_OK;
        cursor = dot + 1;
    }
}

static int locate_object(const char *text, const char *const *segments, size_t count,
                         size_t *brace_out, fr_error *err) {
    size_t brace = fr_json_edit_skip_whitespace(text, 0);
    if (text[brace] != '{') {
        fr_error_set(err, "the document is not a json object");
        return FR_ERR;
    }
    for (size_t index = 0; index < count; index++) {
        member entry;
        if (!fr_json_edit_find_member(text, brace, segments[index], &entry)) {
            fr_error_set(err, "\"%s\" is missing", segments[index]);
            return FR_ERR;
        }
        if (text[entry.value_start] != '{') {
            fr_error_set(err, "\"%s\" is not an object", segments[index]);
            return FR_ERR;
        }
        brace = entry.value_start;
    }
    *brace_out = brace;
    return FR_OK;
}

/* Answers only whether a segment is absent. A segment that is present but not
   an object reads as present, so locate_object reports it rather than a
   removal silently treating a malformed document as nothing to do. */
static int path_is_present(const char *text, const char *const *segments, size_t count) {
    size_t brace = fr_json_edit_skip_whitespace(text, 0);
    if (text[brace] != '{') return 1;
    for (size_t index = 0; index < count; index++) {
        member entry;
        if (!fr_json_edit_find_member(text, brace, segments[index], &entry)) return 0;
        if (text[entry.value_start] != '{') return 1;
        brace = entry.value_start;
    }
    return 1;
}

static int create_missing_objects(char **text, const char *const *segments, size_t count,
                                  fr_error *err) {
    for (size_t depth = 0; depth < count; depth++) {
        size_t brace;
        if (locate_object(*text, segments, depth, &brace, err) != FR_OK) return FR_ERR;

        member entry;
        if (fr_json_edit_find_member(*text, brace, segments[depth], &entry)) {
            if ((*text)[entry.value_start] != '{') {
                fr_error_set(err, "\"%s\" is not an object", segments[depth]);
                return FR_ERR;
            }
            continue;
        }

        edit_value empty = { VALUE_EMPTY_OBJECT, NULL, NULL, 0 };
        char *grown = NULL;
        if (set_member(*text, brace, segments[depth], &empty, &grown, err) != FR_OK) return FR_ERR;
        free(*text);
        *text = grown;
    }
    return FR_OK;
}

static int edit_document(const char *text, const char *path, const char *key,
                         const edit_value *value, char **out, fr_error *err) {
    *out = NULL;

    char storage[MAX_SEGMENTS][MAX_SEGMENT_LENGTH];
    size_t count = 0;
    if (split_path(path, storage, &count, err) != FR_OK) return FR_ERR;

    const char *segments[MAX_SEGMENTS];
    for (size_t index = 0; index < count; index++) segments[index] = storage[index];

    char *current = duplicate(text);
    if (current == NULL) return finish(NULL, out, err);

    if (value == NULL) {
        if (!path_is_present(current, segments, count)) {
            *out = current;
            return FR_OK;
        }
    } else if (create_missing_objects(&current, segments, count, err) != FR_OK) {
        free(current);
        return FR_ERR;
    }

    size_t brace;
    if (locate_object(current, segments, count, &brace, err) != FR_OK) {
        free(current);
        return FR_ERR;
    }

    char *result = NULL;
    int status = value != NULL ? set_member(current, brace, key, value, &result, err)
                               : remove_member(current, brace, key, &result, err);
    free(current);
    if (status != FR_OK) return FR_ERR;
    *out = result;
    return FR_OK;
}

int fr_json_edit_set_string(const char *text, const char *path, const char *key,
                            const char *value, char **out, fr_error *err) {
    edit_value entry = { VALUE_STRING, value, NULL, 0 };
    return edit_document(text, path, key, &entry, out, err);
}

int fr_json_edit_set_string_array(const char *text, const char *path, const char *key,
                                  const char *const *values, size_t count,
                                  char **out, fr_error *err) {
    edit_value entry = { VALUE_STRING_ARRAY, NULL, values, count };
    return edit_document(text, path, key, &entry, out, err);
}

int fr_json_edit_remove(const char *text, const char *path, const char *key,
                        char **out, fr_error *err) {
    return edit_document(text, path, key, NULL, out, err);
}
