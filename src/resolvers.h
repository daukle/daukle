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

/* Acquires entry's chunk through the floor and runs it, unless entry->label is
   already the loaded resolver, then calls its resolve function with
   coordinate. Acquiring covers both a local path and a pinned url, verifying
   sha256 before the chunk runs, and honours whatever verbs its own
   daukle.plugin{ uses = {...} } declares, the same as a plugin's chunk does. A
   NULL entry is a real daukle error, not a crash: entry->label is the first
   thing this function would otherwise read. */
int fr_resolvers_use(const fr_resolver_entry *entry, const char *coordinate, char **out_url,
                     char **out_resolved, fr_error *err);

/* Resets the loaded-resolver memo, so a later fr_resolvers_use acquires again
   rather than assuming a resolver from an earlier, now-shut-down runtime is
   still loaded. Call wherever fr_lua_runtime_shutdown is paired with
   fr_plugins_report_clear. */
void fr_resolvers_clear(void);

#endif
