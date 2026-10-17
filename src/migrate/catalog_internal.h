#ifndef DAUKLE_CATALOG_INTERNAL_H
#define DAUKLE_CATALOG_INTERNAL_H

#include "migrate/catalog.h"

#include "cJSON.h"

#define FR_ENTRY_OK 0
#define FR_ENTRY_REFUSED 1
#define FR_ENTRY_NO_MEMORY 2

/* Reads one element of a catalog's plugins array into *out, which the caller
   passes zeroed. FR_ENTRY_REFUSED leaves a reason phrase in reason (it
   completes the sentence "catalog entry <id> ...") and *out already released;
   FR_ENTRY_NO_MEMORY leaves *out released too. *id_seen is the entry's id text
   when it is a string, borrowed from entry, so a refusal can name it. */
int fr_catalog_read_entry(const cJSON *entry, fr_catalog_plugin *out, fr_error *reason,
                          const char **id_seen);

void fr_catalog_plugin_release(fr_catalog_plugin *plugin);

#endif
