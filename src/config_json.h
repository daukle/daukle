#ifndef DAUKLE_CONFIG_JSON_H
#define DAUKLE_CONFIG_JSON_H

#include "jsonx.h"

/* Reads json text into daukle's internal document model for a plugin holding
   its own data file, such as npm recovering its ledger from a package.json.
   json is deliberately not a manifest format: toml and lua are the whole config
   table, so this is reachable only through daukle.json_parse and never by
   finding a daukle.json on disk.

   Sets *out only on success, and the caller then owns it and must
   cJSON_Delete it.

   The message names a byte offset rather than a line because a byte offset is
   the only position cJSON reports, and it leaves the origin to the caller,
   which is the plugin's own chunk name and line. */
int fr_config_json_parse(const char *text, cJSON **out, fr_error *err);

#endif
