#ifndef TERKO_HTTP_H
#define TERKO_HTTP_H

#include "types.h"

#include <stddef.h>

#define FR_HTTP_MAX_BODY (8u * 1024u * 1024u)
#define FR_HTTP_MAX_REDIRECTS 5

typedef struct { const char *name; const char *value; } fr_http_header;

typedef int (*fr_http_fn)(const char *url, const fr_http_header *headers, size_t header_count,
                          char **out_body, size_t *out_length, fr_error *err);

int fr_http_get(const char *url, const fr_http_header *headers, size_t header_count,
                char **out_body, size_t *out_length, fr_error *err);

fr_http_fn fr_http_set_backend(fr_http_fn backend);

int fr_http_backend_get(const char *url, const fr_http_header *headers, size_t header_count,
                        char **out_body, size_t *out_length, fr_error *err);

#endif
