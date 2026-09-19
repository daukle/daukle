#ifndef DAUKLE_PLUGINS_H
#define DAUKLE_PLUGINS_H

#include "types.h"

#include <stddef.h>

struct cJSON;

typedef enum { FR_PLUGIN_LOCAL, FR_PLUGIN_REMOTE } fr_plugin_kind;

typedef struct {
    char *label;
    fr_plugin_kind kind;
    char *path;     /* FR_PLUGIN_LOCAL */
    char *repo;     /* FR_PLUGIN_REMOTE, "owner/name" */
    char *version;  /* FR_PLUGIN_REMOTE, a range */
    char *sha256;   /* optional, NULL when unpinned */
} fr_plugin_entry;

/* Reads the manifest's `[plugins]` table into a freshly allocated array.
   An absent table yields *out_count == 0 and FR_OK, not an error. */
int fr_plugins_parse(const struct cJSON *document, fr_plugin_entry **out, size_t *out_count,
                     fr_error *err);

/* Frees every string an entry owns, then the array itself.
   Tolerates a NULL array paired with a zero count. */
void fr_plugins_free(fr_plugin_entry *entries, size_t count);

#endif
