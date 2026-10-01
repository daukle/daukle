#ifndef DAUKLE_PLUGINS_INTERNAL_H
#define DAUKLE_PLUGINS_INTERNAL_H

#include "plugin/plugin_deps.h"
#include "plugin/plugins.h"
#include "util/error.h"

#include "lua.h"

#include <stddef.h>

/* Runs one chunk far enough to read its daukle.plugin declaration into
   declaration, which the caller has already filled the kind and label of.
   fr_plugins_read_declaration in plugins.h is the same pass for a caller that
   has neither the runtime state nor a part-filled declaration to hand. */
int fr_plugins_declaration_read(lua_State *state, const char *text, size_t length,
                                const char *origin, fr_plugin_declaration *declaration,
                                fr_error *err);

/* What the load pass in plugins.c needs from the report in plugin_report.c, and
   nothing else: the report's storage stays private to that file, so a caller
   cannot append a row by writing one. Everything the rest of the program reads
   the report through is in plugins.h, not here. */

/* Appends one entry, owning copies of everything it stores: entry and declaration are both
   about to be freed by their callers (fr_plugins_free and fr_plugins_free_declaration), so
   nothing here may keep a pointer into either. origin is the url a URL or resolved entry fetched, or the
   resolved path a local entry read; resolved is the resolver's own answer, NULL for the two
   kinds core names itself. */
int fr_plugins_report_append(const fr_plugin_entry *entry, const char *origin,
                             const char *resolved, const char *digest,
                             const fr_plugin_declaration *declaration, fr_error *err);

/* Appends a row per binding in deps, sorted, so that one unchanged manifest prints
   the same report on every machine. */
int fr_plugins_report_append_dependencies(fr_plugin_deps *deps, fr_error *err);

/* Declared-versus-referenced is a static comparison over the parsed entries, not
   something discovered by acquiring a resolver: section 4.1 deliberately never fetches
   or runs a resolver no plugin names, so this must not either. */
int fr_plugins_report_record_unused_resolvers(const fr_plugin_entry *entries, size_t entry_count,
                                              const fr_resolver_entry *resolvers,
                                              size_t resolver_count, fr_error *err);

#endif
