#include "http.h"

#include "error.h"

#include <stdlib.h>
#include <string.h>

static fr_http_fn ACTIVE_BACKEND = fr_http_backend_get;

fr_http_fn fr_http_set_backend(fr_http_fn backend) {
    fr_http_fn previous = ACTIVE_BACKEND;
    ACTIVE_BACKEND = backend == NULL ? fr_http_backend_get : backend;
    return previous;
}

int fr_http_get(const char *url, const fr_http_header *headers, size_t header_count,
                char **out_body, size_t *out_length, fr_error *err) {
    *out_body = NULL;
    *out_length = 0;
    return ACTIVE_BACKEND(url, headers, header_count, out_body, out_length, err);
}

static int holds_a_line_break_or_nul(const char *text, size_t length) {
    for (size_t index = 0; index < length; index++) {
        if (text[index] == '\r' || text[index] == '\n' || text[index] == '\0') return 1;
    }
    return 0;
}

static char *copy_bounded(const char *text, size_t length) {
    char *copy = malloc(length + 1);
    if (copy == NULL) return NULL;
    memcpy(copy, text, length);
    copy[length] = '\0';
    return copy;
}

int fr_http_headers_add(fr_http_headers *headers, const char *name, size_t name_length,
                        const char *value, size_t value_length, fr_error *err) {
    if (name_length == 0) {
        fr_error_set(err, "a header name is empty");
        return FR_ERR;
    }
    if (holds_a_line_break_or_nul(name, name_length)
        || holds_a_line_break_or_nul(value, value_length)) {
        fr_error_set(err, "the header \"%.*s\" holds a line break or a NUL, which would let it"
                          " smuggle a further header into the request",
                     (int) name_length, name);
        return FR_ERR;
    }
    if (headers->count == FR_HTTP_MAX_HEADERS) {
        fr_error_set(err, "more than %d headers were asked for", FR_HTTP_MAX_HEADERS);
        return FR_ERR;
    }

    char *name_copy = copy_bounded(name, name_length);
    char *value_copy = copy_bounded(value, value_length);
    if (name_copy == NULL || value_copy == NULL) {
        free(name_copy);
        free(value_copy);
        fr_error_set(err, "out of memory recording a header");
        return FR_ERR;
    }

    headers->items[headers->count].name = name_copy;
    headers->items[headers->count].value = value_copy;
    headers->count++;
    return FR_OK;
}

void fr_http_headers_free(fr_http_headers *headers) {
    for (size_t index = 0; index < headers->count; index++) {
        free(headers->items[index].name);
        free(headers->items[index].value);
    }
    headers->count = 0;
}

size_t fr_http_headers_borrow(const fr_http_headers *headers, fr_http_header *out) {
    for (size_t index = 0; index < headers->count; index++) {
        out[index].name = headers->items[index].name;
        out[index].value = headers->items[index].value;
    }
    return headers->count;
}
