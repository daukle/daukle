#include "util/strbuf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void fr_strbuf_init(fr_strbuf *buffer) {
    buffer->data = NULL;
    buffer->length = 0;
    buffer->capacity = 0;
    buffer->failed = 0;
}

static int reserve(fr_strbuf *buffer, size_t extra) {
    if (buffer->failed) return 0;
    size_t needed = buffer->length + extra + 1;
    if (needed <= buffer->capacity) return 1;
    size_t capacity = buffer->capacity == 0 ? 64 : buffer->capacity;
    while (capacity < needed) capacity *= 2;
    char *data = realloc(buffer->data, capacity);
    if (data == NULL) {
        buffer->failed = 1;
        return 0;
    }
    buffer->data = data;
    buffer->capacity = capacity;
    return 1;
}

void fr_strbuf_append_bytes(fr_strbuf *buffer, const char *bytes, size_t length) {
    if (!reserve(buffer, length)) return;
    memcpy(buffer->data + buffer->length, bytes, length);
    buffer->length += length;
    buffer->data[buffer->length] = '\0';
}

void fr_strbuf_append(fr_strbuf *buffer, const char *text) {
    fr_strbuf_append_bytes(buffer, text, strlen(text));
}

void fr_strbuf_append_format(fr_strbuf *buffer, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int length = vsnprintf(NULL, 0, format, args);
    va_end(args);
    if (length < 0) {
        buffer->failed = 1;
        return;
    }
    if (!reserve(buffer, (size_t) length)) return;
    va_start(args, format);
    vsnprintf(buffer->data + buffer->length, buffer->capacity - buffer->length, format, args);
    va_end(args);
    buffer->length += (size_t) length;
}

char *fr_strbuf_release(fr_strbuf *buffer) {
    if (buffer->failed) {
        fr_strbuf_free(buffer);
        return NULL;
    }
    char *data = buffer->data;
    if (data == NULL) {
        data = malloc(1);
        if (data != NULL) data[0] = '\0';
    }
    fr_strbuf_init(buffer);
    return data;
}

void fr_strbuf_free(fr_strbuf *buffer) {
    free(buffer->data);
    fr_strbuf_init(buffer);
}
