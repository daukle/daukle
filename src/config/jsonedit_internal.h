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

#endif
