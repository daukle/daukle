#ifndef DAUKLE_CATALOG_H
#define DAUKLE_CATALOG_H

#include "util/types.h"

#include <stddef.h>

#define FR_CATALOG_MAX_FILES 16
#define FR_CATALOG_MAX_PATTERNS 16

typedef enum { FR_DETECT_ROOT, FR_DETECT_TREE } fr_detect_scope;

typedef struct {
    char *files[FR_CATALOG_MAX_FILES];
    size_t file_count;
    char *contains[FR_CATALOG_MAX_PATTERNS];
    size_t contains_count;
    fr_detect_scope scope;
} fr_detect_rule;

typedef struct {
    char *id;
    char *asset_url;
    char *asset_sha256;
    fr_detect_rule *rules;
    size_t rule_count;
    struct cJSON *replaces;
    char **skip;
    size_t skip_count;
    size_t catalog_index;
    char *location;
} fr_catalog_plugin;

typedef struct {
    fr_catalog_plugin *plugins;
    size_t count;
    char **warnings;
    size_t warning_count;
} fr_catalog;

/* The locations migrate reads: every whitespace-separated item of the
   environment value (NULL or empty for none), then every --catalog in order.
   Environment strings point into a copy held in the same block as the array,
   flag strings are borrowed; free(*out) releases both. */
int fr_catalog_locations(const char *environment, const char *const *flags, size_t flag_count,
                         const char ***out, size_t *out_count, fr_error *err);

/* Fetches (http or https) or reads (anything else, as a path) each location in
   order and appends its plugins. A plugin id seen in an earlier catalog is kept
   from that one and the later one is a warning. A malformed document is an
   error naming its location; a malformed ENTRY is a warning and the entry is
   dropped, never evaluated. A catalog is never cached: its content is read at
   run time. On error *out is freed and zeroed. */
int fr_catalog_load(const char *const *locations, size_t count, fr_catalog *out, fr_error *err);

/* Parses one document's text; exposed for the tests. */
int fr_catalog_parse(const char *text, const char *location, size_t catalog_index,
                     fr_catalog *into, fr_error *err);

const fr_catalog_plugin *fr_catalog_find(const fr_catalog *catalog, const char *id);

/* Whether a Lua pattern holds a "-" Lua reads as the lazy quantifier. The same
   rule as daukle/manifest's hasBareHyphen, because a third-party catalog never
   passed through that generator. */
int fr_catalog_pattern_has_bare_hyphen(const char *pattern);

void fr_catalog_free(fr_catalog *catalog);

#endif
