#ifndef DAUKLE_PLUGINS_REMOTE_H
#define DAUKLE_PLUGINS_REMOTE_H

#include "plugins.h"
#include "types.h"

/* Resolves a FR_PLUGIN_REMOTE entry's plugin.lua text: the cache under
   fr_cache_root is scanned first for a version satisfying entry->version, and
   only a miss reaches GitHub. Called from plugins.c's load_one, its only
   caller. */
int fr_plugins_resolve_remote(const fr_plugin_entry *entry, char **out_text, char **out_origin,
                              fr_error *err);

#endif
