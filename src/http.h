#ifndef DAUKLE_HTTP_H
#define DAUKLE_HTTP_H

#include "types.h"

#include <stddef.h>

#define FR_HTTP_MAX_BODY (8u * 1024u * 1024u)
#define FR_HTTP_MAX_REDIRECTS 5

typedef struct { const char *name; const char *value; } fr_http_header;

typedef int (*fr_http_fn)(const char *url, const fr_http_header *headers, size_t header_count,
                          char **out_body, size_t *out_length, fr_error *err);

int fr_http_get(const char *url, const fr_http_header *headers, size_t header_count,
                char **out_body, size_t *out_length, fr_error *err);

/* The most headers a resolver may return. Bounded rather than grown because
   the only producer is a plugin, and how much core allocates is not a
   plugin's decision to make. */
#define FR_HTTP_MAX_HEADERS 16

typedef struct { char *name; char *value; } fr_http_owned_header;

/* Headers built at runtime by a producer that cannot point at literals, so
   the list owns its strings. */
typedef struct {
    fr_http_owned_header items[FR_HTTP_MAX_HEADERS];
    size_t count;
} fr_http_headers;

/* Refuses a name or value holding CR, LF or NUL: these are forwarded to the
   request verbatim, so one carrying a line break would let its author append
   a header of its own or split the request. Lengths are explicit because the
   producer is Lua, whose strings may hold an embedded NUL that strlen would
   not see. */
int fr_http_headers_add(fr_http_headers *headers, const char *name, size_t name_length,
                        const char *value, size_t value_length, fr_error *err);

void fr_http_headers_free(fr_http_headers *headers);

/* Copies the list into the borrowed form fr_http_get takes, returning how many
   entries were written; out holds at least FR_HTTP_MAX_HEADERS of them. */
size_t fr_http_headers_borrow(const fr_http_headers *headers, fr_http_header *out);

fr_http_fn fr_http_set_backend(fr_http_fn backend);

int fr_http_backend_get(const char *url, const fr_http_header *headers, size_t header_count,
                        char **out_body, size_t *out_length, fr_error *err);

#endif
