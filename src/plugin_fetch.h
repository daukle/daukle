#ifndef DAUKLE_PLUGIN_FETCH_H
#define DAUKLE_PLUGIN_FETCH_H

#include "http.h"
#include "types.h"

/* Core's entire acquisition floor: the bytes at url, cached under
   <cache-root>/plugins/<sha256 of url>/plugin.lua and served from there on a
   later run, so a repeated build is offline. The caller owns *out_text.
   Core never parses what comes back: what the bytes must be in order to load
   is the plugin loader's question, not this module's.
   headers are sent with the request verbatim and never interpreted: a private
   artifact needs whatever credential its resolver already used, and core names
   no authentication scheme of its own. They are deliberately absent from the
   cache key, which is the url alone: two runs differing only in a credential
   are asking for the same bytes.
   The disk cache here honours fr_cache_enabled but deliberately not
   fr_cache_refreshing. Refreshing exists so a resolver's own producer reruns,
   while what is stored here is invalidated by name instead: "daukle plugin
   update" calls fr_plugin_fetch_discard for exactly the entries it scopes to,
   and bypassing reads as well would re-download every chunk in the manifest,
   including the ones that command was not asked about. */
int fr_plugin_fetch(const char *url, const fr_http_header *headers, size_t header_count,
                    char **out_text, fr_error *err);

/* Removes the entry fr_plugin_fetch would serve for url, so bytes that failed
   a digest pin are not handed to the next run. Best effort: an absent entry is
   not an error. */
void fr_plugin_fetch_discard(const char *url);

#endif
