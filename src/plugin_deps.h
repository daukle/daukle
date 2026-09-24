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

/* A member of the artifact deps itself belongs to, which is what an alias-less
   require inside a module this acquisition served must read: a module's
   siblings are its own plugin's, not the dependent's, and they are reachable
   whether or not their artifact exports them. Refused on the view
   fr_plugin_deps_acquire returned, which belongs to no artifact. *out_text is
   borrowed from the acquisition and invalid after fr_plugin_deps_close, as
   fr_plugin_deps_member's is. */
int fr_plugin_deps_own_member(fr_plugin_deps *deps, const char *member, const char **out_text,
                              size_t *out_length, fr_error *err);

/* What deps is known by in a message: a [plugins] label for the acquired view,
   the alias that first named the artifact for a view handed back by
   fr_plugin_deps_member. Borrowed from the acquisition and invalid after
   fr_plugin_deps_close. */
const char *fr_plugin_deps_label(const fr_plugin_deps *deps);

/* One binding's reportable fields, borrowed from the acquisition and invalid after
   fr_plugin_deps_close. url, digest, uses, kind and overridden describe the artifact the binding
   names; alias and required_by describe the binding itself, so a url two dependents both name
   prints one row per dependent, never one row shared between them. digest is the hex sha256
   computed while acquiring the artifact (verified against its pin when it had one), never
   recomputed by a caller building a report from it. kind is FR_PLUGIN_URL for an ordinary fetch and
   whatever the manifest override actually used otherwise, never assumed. */
typedef struct {
    const char *url;
    const char *alias;
    const char *required_by;
    const char *digest;
    const char *const *uses;
    size_t uses_count;
    fr_plugin_kind kind;
    int overridden;
} fr_plugin_deps_row;

/* How many artifacts fr_plugin_deps_acquire acquired transitively (every depth, not counting the
   root view fr_plugin_deps_acquire itself returned, which belongs to no artifact). 0 for
   deps == NULL or a plugin that requires nothing. */
size_t fr_plugin_deps_count(const fr_plugin_deps *deps);

/* How many (dependent, alias) bindings the acquisition holds: the root's own requires plus every
   acquired artifact's own requires. A url reached by two dependents is two bindings even though it
   is one artifact, because a binding is what a report row means. 0 for deps == NULL. */
size_t fr_plugin_deps_row_count(const fr_plugin_deps *deps);

/* Fills out with the binding at index's reportable fields. index must be < fr_plugin_deps_row_count
   (deps). Rows come out in an order fr_plugin_deps_acquire's Lua table walks leave unspecified: a
   caller building a report from them sorts before it settles on one. */
void fr_plugin_deps_row_at(const fr_plugin_deps *deps, size_t index, fr_plugin_deps_row *out);

void fr_plugin_deps_close(fr_plugin_deps *deps);

#endif
