#ifndef DAUKLE_PLUGIN_DEPS_H
#define DAUKLE_PLUGIN_DEPS_H

#include "plugins.h"
#include "resolvers.h"
#include "types.h"

#include <stddef.h>

struct cJSON;

typedef struct fr_plugin_deps fr_plugin_deps;

#define FR_PLUGIN_DEPS_MAX_DEPTH 8
#define FR_PLUGIN_DEPS_MAX_NODES 64

/* Acquires every artifact declaration requires, transitively: fetch, verify the
   digest, open the source, read that artifact's own declaration, repeat. No
   entry chunk is run. overrides is the dependent's own [plugins] entry
   "requires" table or NULL: it replaces what the author named for one of
   declaration's own aliases (depth 0 only, never an alias belonging to
   something declaration itself requires), and may take any form a [plugins]
   value takes (the string sugar, or a table with path, url, or resolver plus
   coordinate). resolvers is carried for the same reason load_one carries it:
   an override may name a resolver coordinate, which the author's own url form
   never can. Every source stays open until close. */
int fr_plugin_deps_acquire(const fr_plugin_declaration *declaration,
                           const struct cJSON *overrides, const fr_resolver_entry *resolvers,
                           size_t resolver_count, fr_plugin_deps **out, fr_error *err);

/* The bytes of member, if alias names a required artifact and that artifact
   exports member. out_owner is the artifact's own dependency set, which is what
   a require inside that module resolves against, and out_owner_label names it
   in a message. All three are borrowed from the acquisition and are invalid
   after fr_plugin_deps_close: the caller copies whatever it needs to outlive
   the load, and never closes what out_owner points at. */
int fr_plugin_deps_member(fr_plugin_deps *deps, const char *alias, const char *member,
                          const char **out_text, size_t *out_length,
                          fr_plugin_deps **out_owner, const char **out_owner_label,
                          fr_error *err);

void fr_plugin_deps_close(fr_plugin_deps *deps);

#endif
