#ifndef TIESTONE_STRBUF_H
#define TIESTONE_STRBUF_H

#include <stddef.h>

/* A failed growth is sticky, so a caller appends freely and checks once, at
   release, which returns NULL if any append along the way could not allocate. */
typedef struct {
    char *data;
    size_t length;
    size_t capacity;
    int failed;
} fr_strbuf;

void fr_strbuf_init(fr_strbuf *buffer);
void fr_strbuf_append(fr_strbuf *buffer, const char *text);
void fr_strbuf_append_bytes(fr_strbuf *buffer, const char *bytes, size_t length);
void fr_strbuf_append_format(fr_strbuf *buffer, const char *format, ...);
char *fr_strbuf_release(fr_strbuf *buffer);
void fr_strbuf_free(fr_strbuf *buffer);

#endif
