#ifndef DAUKLE_JSONEDIT_INTERNAL_H
#define DAUKLE_JSONEDIT_INTERNAL_H

#include <stddef.h>

/* The seam between jsonedit.c, which writes an edit into a document, and
   jsonedit_scan.c, which reads one: where a value ends, which member holds a
   key, and how a line is indented. */

typedef struct {
    size_t key_start;
    size_t key_end;
    size_t value_start;
    size_t value_end;
} member;

size_t fr_json_edit_skip_whitespace(const char *text, size_t index);
size_t fr_json_edit_skip_value(const char *text, size_t index);
int fr_json_edit_first_member(const char *text, size_t brace, member *out);
int fr_json_edit_next_member(const char *text, const member *current, member *out);
int fr_json_edit_find_member(const char *text, size_t brace, const char *key, member *out);
int fr_json_edit_last_member(const char *text, size_t brace, member *out);
int fr_json_edit_starts_its_line(const char *text, size_t index);
char *fr_json_edit_leading_whitespace(const char *text, size_t index);
char *fr_json_edit_indent_unit(const char *text);

#endif
