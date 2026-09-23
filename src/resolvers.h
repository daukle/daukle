#ifndef DAUKLE_RESOLVERS_H
#define DAUKLE_RESOLVERS_H

#include "types.h"

#include <stddef.h>

struct cJSON;

/* One [resolvers] entry. Exactly one of url and path is set. block borrows
   from the caller's document, which outlives every use of it, and is what the
   resolver's own resolve function receives. */
typedef struct {
    char *label;
    char *url;
    char *path;
    char *sha256;
    const struct cJSON *block;
} fr_resolver_entry;

/* An absent [resolvers] table yields *out_count == 0 and FR_OK, not an error. */
int fr_resolvers_parse(const struct cJSON *document, fr_resolver_entry **out, size_t *out_count,
                       fr_error *err);

void fr_resolvers_free(fr_resolver_entry *entries, size_t count);

const fr_resolver_entry *fr_resolvers_find(const fr_resolver_entry *entries, size_t count,
                                           const char *label);

#endif
