#ifndef DAUKLE_PLUGINS_REMOTE_H
#define DAUKLE_PLUGINS_REMOTE_H

#include "plugins.h"
#include "types.h"

/* The Authorization value daukle presents to GitHub, "Bearer <token>" from
   DAUKLE_TOKEN or, failing that, GITHUB_TOKEN, and NULL when neither is set.
   The caller owns the string. One implementation rather than one per caller,
   so authenticating daukle with the daukle-specific variable cannot leave one
   of them fetching anonymously. It lives here because this is the module that
   outlives source_github.c, which the plugin catalogue replaces. */
char *fr_github_auth_header(void);

/* Resolves a FR_PLUGIN_REMOTE entry's plugin.lua text: the cache under
   fr_cache_root is scanned first for a version satisfying entry->version, and
   only a miss reaches GitHub. Called from plugins.c's load_one, its only
   caller. */
int fr_plugins_resolve_remote(const fr_plugin_entry *entry, char **out_text, char **out_origin,
                              fr_error *err);

/* Deletes the single cached version directory holding the plugin.lua that
   fr_plugins_resolve_remote reported as origin, not the whole owner/name
   tree. Called from plugins.c's load_one when a remote plugin fails its
   sha256 pin: the cache is consulted before the network, so bytes that failed
   their integrity check must not stay where the next resolve would serve them
   instead of re-fetching. Best effort, like the rest of the plugin cache. */
void fr_plugins_discard_cached_version(const char *origin);

/* Deletes every cached version of repo ("owner/name") under the plugin cache
   root, so the next resolve re-fetches. Best effort: a repo with nothing
   cached is not an error. Called from plugins.c's fr_plugins_update_cache,
   once per FR_PLUGIN_REMOTE entry it removes. */
int fr_plugins_remove_cache(const char *repo, fr_error *err);

#endif
