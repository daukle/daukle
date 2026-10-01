#ifndef DAUKLE_JSONX_H
#define DAUKLE_JSONX_H

#include "util/types.h"

#include <stddef.h>

#include "cJSON.h"

/* Reads json text into daukle's internal document model for a plugin holding
   its own data file, such as npm recovering its ledger from a package.json.
   json is deliberately NOT a manifest format: toml and lua are the whole config
   table, so this is reachable only through daukle.json_parse and never by
   finding a daukle.json on disk. It lived in a file called config_json.c until
   2026-10-01, whose name asserted the opposite.

   Sets *out only on success, and the caller then owns it and must
   cJSON_Delete it.

   The message names a byte offset rather than a line because a byte offset is
   the only position cJSON reports, and it leaves the origin to the caller,
   which is the plugin's own chunk name and line. */
int fr_json_parse(const char *text, cJSON **out, fr_error *err);

int fr_json_string(const cJSON *object, const char *key, const char *path, const char **out, fr_error *err);
int fr_json_object(const cJSON *object, const char *key, const char *path, const cJSON **out, fr_error *err);
/* True for a real array and for the empty object a lua table with no entries
   converts to. A source plugin returns a table, not a document, so unlike a
   daukle.toml there is nothing to repair an empty sequence against; every site
   requiring an array already treats no entries exactly as it treats an absent
   key, so admitting the empty object permits nothing those sites would have
   acted on. See D-5. */
int fr_json_is_array_or_empty_table(const cJSON *value);
int fr_json_array_of_strings(const cJSON *object, const char *key, const char *path,
                             char ***out, size_t *count, fr_error *err);
void fr_string_array_free(char **items, size_t count);

#endif
