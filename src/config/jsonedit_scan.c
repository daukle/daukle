#include "config/jsonedit_internal.h"

#include "util/strbuf.h"

#include <string.h>

size_t fr_json_edit_skip_whitespace(const char *text, size_t index) {
    while (text[index] == ' ' || text[index] == '\t' || text[index] == '\n' || text[index] == '\r') index++;
    return index;
}

static size_t skip_string(const char *text, size_t index) {
    index++;
    while (text[index] != '\0') {
        if (text[index] == '\\' && text[index + 1] != '\0') {
            index += 2;
            continue;
        }
        if (text[index] == '"') return index + 1;
        index++;
    }
    return index;
}

size_t fr_json_edit_skip_value(const char *text, size_t index) {
    if (text[index] == '"') return skip_string(text, index);
    if (text[index] == '{' || text[index] == '[') {
        int depth = 0;
        while (text[index] != '\0') {
            if (text[index] == '"') {
                index = skip_string(text, index);
                continue;
            }
            if (text[index] == '{' || text[index] == '[') {
                depth++;
            } else if (text[index] == '}' || text[index] == ']') {
                depth--;
                if (depth == 0) return index + 1;
            }
            index++;
        }
        return index;
    }
    while (text[index] != '\0' && strchr(",}] \t\n\r", text[index]) == NULL) index++;
    return index;
}

static int read_member(const char *text, size_t index, member *out) {
    if (text[index] != '"') return 0;
    out->key_start = index;
    out->key_end = skip_string(text, index);
    size_t cursor = fr_json_edit_skip_whitespace(text, out->key_end);
    if (text[cursor] != ':') return 0;
    cursor = fr_json_edit_skip_whitespace(text, cursor + 1);
    out->value_start = cursor;
    out->value_end = fr_json_edit_skip_value(text, cursor);
    return 1;
}

int fr_json_edit_first_member(const char *text, size_t brace, member *out) {
    return read_member(text, fr_json_edit_skip_whitespace(text, brace + 1), out);
}

int fr_json_edit_next_member(const char *text, const member *current, member *out) {
    size_t index = fr_json_edit_skip_whitespace(text, current->value_end);
    if (text[index] != ',') return 0;
    return read_member(text, fr_json_edit_skip_whitespace(text, index + 1), out);
}

static int member_has_key(const char *text, const member *entry, const char *key) {
    size_t length = entry->key_end - entry->key_start - 2;
    return strlen(key) == length && strncmp(text + entry->key_start + 1, key, length) == 0;
}

int fr_json_edit_find_member(const char *text, size_t brace, const char *key, member *out) {
    member entry;
    if (!fr_json_edit_first_member(text, brace, &entry)) return 0;
    for (;;) {
        if (member_has_key(text, &entry, key)) {
            *out = entry;
            return 1;
        }
        member following;
        if (!fr_json_edit_next_member(text, &entry, &following)) return 0;
        entry = following;
    }
}

int fr_json_edit_last_member(const char *text, size_t brace, member *out) {
    member entry;
    if (!fr_json_edit_first_member(text, brace, &entry)) return 0;
    for (;;) {
        member following;
        if (!fr_json_edit_next_member(text, &entry, &following)) {
            *out = entry;
            return 1;
        }
        entry = following;
    }
}

static size_t line_start(const char *text, size_t index) {
    while (index > 0 && text[index - 1] != '\n') index--;
    return index;
}

static void append_leading_whitespace(fr_strbuf *buffer, const char *text, size_t index) {
    size_t start = line_start(text, index);
    size_t cursor = start;
    while (cursor < index && (text[cursor] == ' ' || text[cursor] == '\t')) cursor++;
    fr_strbuf_append_bytes(buffer, text + start, cursor - start);
}

int fr_json_edit_starts_its_line(const char *text, size_t index) {
    for (size_t cursor = line_start(text, index); cursor < index; cursor++) {
        if (text[cursor] != ' ' && text[cursor] != '\t') return 0;
    }
    return 1;
}

char *fr_json_edit_leading_whitespace(const char *text, size_t index) {
    fr_strbuf buffer;
    fr_strbuf_init(&buffer);
    append_leading_whitespace(&buffer, text, index);
    return fr_strbuf_release(&buffer);
}

char *fr_json_edit_indent_unit(const char *text) {
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
