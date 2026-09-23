#ifndef DAUKLE_PLUGINS_H
#define DAUKLE_PLUGINS_H

#include "registry.h"
#include "resolvers.h"
#include "types.h"

#include <stddef.h>

struct cJSON;

typedef enum { FR_PLUGIN_PATH, FR_PLUGIN_URL, FR_PLUGIN_RESOLVED } fr_plugin_kind;

typedef struct {
    char *label;
    fr_plugin_kind kind;
    char *path;        /* FR_PLUGIN_PATH */
    char *url;         /* FR_PLUGIN_URL */
    char *resolver;    /* FR_PLUGIN_RESOLVED, a [resolvers] label */
    char *coordinate;  /* FR_PLUGIN_RESOLVED, opaque to core */
    char *sha256;      /* optional on every kind */
} fr_plugin_entry;

/* One reported plugin, holding copies of everything an fr_plugin_entry held:
   fr_plugins_load frees the entries it parsed before returning, so a report
   that borrowed their pointers would dangle the moment anything read it. */
typedef struct {
    char *label;
    fr_plugin_kind kind;
    char *resolver;    /* FR_PLUGIN_RESOLVED only: the [resolvers] label used, else NULL */
    char *url;         /* NULL for FR_PLUGIN_PATH; the fetched url otherwise */
    char *resolved;    /* what the resolver answered, or the url or path used */
    char **uses;
    size_t uses_count;
    char sha256[65];   /* always computed, pinned or not: this is what makes adopting a
                          pin a copy and a paste rather than a separate command */
} fr_plugin_report_entry;

typedef struct {
    fr_plugin_report_entry *entries;
    size_t count;
    char **unused_resolvers;      /* [resolvers] labels no plugin entry names */
    size_t unused_resolver_count;
} fr_plugin_report;

/* strdup is not C11 and strndup is absent on MSVC, so plugins.c and
   resolvers.c share these rather than each keeping a copy. Defined in
   plugins.c, the original owner of both. */
char *fr_dup_string(const char *text);
char *fr_dup_prefix(const char *text, size_t length);

/* Runs only as much of text as its daukle.plugin{...} call, the same
   protected read fr_plugins_load uses before installing verbs, and reports
   what it declared "uses" as a freshly allocated array. kind ("plugin" or
   "resolver") and label name the chunk in any error text, so a resolver's
   malformed uses is never reported as a plugin's; origin is what a parse
   error is reported against. *out_uses is NULL when uses is empty or absent,
   not an error. A lua runtime must already be open. Shared with resolvers.c,
   whose chunks are acquired the same way a plugin's is. */
int fr_plugins_read_uses(const char *text, const char *origin, const char *kind,
                         const char *label, char ***out_uses, size_t *out_uses_count,
                         fr_error *err);

/* Frees an array fr_plugins_read_uses returned. Tolerates a NULL array paired
   with a zero count. */
void fr_plugins_free_uses(char **uses, size_t count);

/* Reads the manifest's `[plugins]` table into a freshly allocated array.
   An absent table yields *out_count == 0 and FR_OK, not an error.
   resolvers is the manifest's `[resolvers]` table, used only to list the
   declared labels when a value names no resolver; whether a named resolver
   exists is decided when the entry loads, not here. */
int fr_plugins_parse(const struct cJSON *document, const fr_resolver_entry *resolvers,
                     size_t resolver_count, fr_plugin_entry **out, size_t *out_count,
                     fr_error *err);

/* A fetched dependency's manifest may not declare plugins: otherwise adding a
   dependency would be enough to make daukle execute its author's code. */
int fr_plugins_reject_in_fetched(const struct cJSON *document, const char *project, fr_error *err);

/* Frees every string an entry owns, then the array itself.
   Tolerates a NULL array paired with a zero count. */
void fr_plugins_free(fr_plugin_entry *entries, size_t count);

/* Runs every plugin the manifest declares, so that what each one registers is in
   registry by the time the manifest is read. A local path resolves inside the lua
   runtime's sandbox, which this opens on base_dir when no runtime is open yet. A
   manifest declaring no plugins opens no lua state at all. */
int fr_plugins_load(fr_registry *registry, const struct cJSON *document, const char *base_dir,
                    fr_error *err);

/* What the last fr_plugins_load resolved, cleared and rebuilt at the start of
   every call so a manifest declaring no plugins reports none, not whatever the
   previous manifest loaded. Never NULL; count is 0 before any load. */
const fr_plugin_report *fr_plugins_report(void);

/* Frees every copy fr_plugins_report holds. Called alongside
   fr_lua_runtime_shutdown once a caller is done reading the report, and
   internally at the start of every fr_plugins_load. */
void fr_plugins_report_clear(void);

/* "daukle plugin update [label]"'s scoping rule, over entries already parsed
   from ONE manifest: label NULL discards the fetched artifact of every entry
   in entries that has one, and nothing outside it, so a label-less update in
   one project can never reach another project's cache. A label discards only
   the entry it names (a path match has nothing cached, so this is a no-op,
   not an error); a label that entries does not declare is an error naming it.
   *out_removed_count counts entries of a kind that has something to discard
   (FR_PLUGIN_URL and FR_PLUGIN_RESOLVED), not whether fr_plugin_fetch_discard
   actually found anything there (it does not report that), so a caller can
   still tell a path-only update apart from one that touched real cache state.
   An FR_PLUGIN_RESOLVED entry is re-resolved, not merely discarded: the
   coordinate-to-url answer that needs re-deciding lives in the resolver's own
   cache, which core cannot selectively clear by itself, so this runs the
   resolve step with reads bypassed but writes kept on (fr_cache_set_refreshing,
   not fr_cache_set_enabled(0): the whole point is that the resolver's
   producer reruns AND its fresh answer is persisted, so an ordinary run
   afterward reads that fresh mapping instead of the stale one). This means a
   lua runtime must already be open (fr_lua_runtime_begin), the same
   precondition fr_resolvers_use itself has, since acquiring a resolver's
   chunk goes through it. It also means the command now executes plugin code,
   which a plain cache clear never did. */
int fr_plugins_update_cache(const fr_plugin_entry *entries, size_t count,
                            const fr_resolver_entry *resolvers, size_t resolver_count,
                            const char *label, size_t *out_removed_count, fr_error *err);

#endif
