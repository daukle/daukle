#ifndef DAUKLE_PLUGINS_H
#define DAUKLE_PLUGINS_H

#include "registry.h"
#include "types.h"

#include <stddef.h>

struct cJSON;

typedef enum { FR_PLUGIN_LOCAL, FR_PLUGIN_REMOTE } fr_plugin_kind;

typedef struct {
    char *label;
    fr_plugin_kind kind;
    char *path;     /* FR_PLUGIN_LOCAL */
    char *repo;     /* FR_PLUGIN_REMOTE, "owner/name" */
    char *version;  /* FR_PLUGIN_REMOTE, a range */
    char *sha256;   /* optional, NULL when unpinned */
} fr_plugin_entry;

/* One reported plugin, holding copies of everything an fr_plugin_entry held:
   fr_plugins_load frees the entries it parsed before returning, so a report
   that borrowed their pointers would dangle the moment anything read it. */
typedef struct {
    char *label;
    fr_plugin_kind kind;
    char *resolved;    /* FR_PLUGIN_REMOTE: the version resolved; FR_PLUGIN_LOCAL: the path used */
    char **uses;
    size_t uses_count;
    char sha256[65];   /* always computed, pinned or not: this is what makes adopting a
                          pin a copy and a paste rather than a separate command */
} fr_plugin_report_entry;

typedef struct {
    fr_plugin_report_entry *entries;
    size_t count;
} fr_plugin_report;

/* strdup is not C11 and strndup is absent on MSVC, so plugins.c and
   plugins_remote.c share these rather than each keeping a copy. Defined in
   plugins.c, the original owner of both. */
char *dup_string(const char *text);
char *dup_prefix(const char *text, size_t length);

/* Reads the manifest's `[plugins]` table into a freshly allocated array.
   An absent table yields *out_count == 0 and FR_OK, not an error. */
int fr_plugins_parse(const struct cJSON *document, fr_plugin_entry **out, size_t *out_count,
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

/* Deletes every cached version of repo ("owner/name") under the plugin cache
   root, so the next resolve re-fetches. Best effort: a repo with nothing
   cached is not an error. */
int fr_plugins_remove_cache(const char *repo, fr_error *err);

/* "daukle plugin update [label]"'s scoping rule, over entries already parsed
   from ONE manifest: label NULL removes the cache of every FR_PLUGIN_REMOTE
   entry in entries, and nothing outside it, so a label-less update in one
   project can never reach another project's cache. A label removes only the
   entry it names (a local match has nothing cached, so this is a no-op, not
   an error); a label entries does not declare is an error naming it. */
int fr_plugins_update_cache(const fr_plugin_entry *entries, size_t count,
                            const char *label, fr_error *err);

#endif
