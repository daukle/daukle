#ifndef DAUKLE_PLUGIN_FETCH_H
#define DAUKLE_PLUGIN_FETCH_H

#include "types.h"

/* Core's entire acquisition floor: the bytes at url, cached under
   <cache-root>/plugins/<sha256 of url>/plugin.lua and served from there on a
   later run, so a repeated build is offline. The caller owns *out_text.
   Core never parses what comes back: what the bytes must be in order to load
   is the plugin loader's question, not this module's. */
int fr_plugin_fetch(const char *url, char **out_text, fr_error *err);

/* Removes the entry fr_plugin_fetch would serve for url, so bytes that failed
   a digest pin are not handed to the next run. Best effort: an absent entry is
   not an error. */
void fr_plugin_fetch_discard(const char *url);

#endif
