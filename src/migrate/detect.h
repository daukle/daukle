#ifndef DAUKLE_DETECT_H
#define DAUKLE_DETECT_H

#include "migrate/catalog.h"
#include "util/types.h"

#include <stddef.h>

#define FR_DETECT_MAX_DEPTH 16
#define FR_DETECT_MAX_DIRECTORIES 20000

/* One plugin answering in one directory. dir is "" for the project root and a
   "/"-separated relative path otherwise. */
typedef struct {
    const fr_catalog_plugin *plugin;
    char *dir;
} fr_detection;

typedef struct {
    fr_detection *items;
    size_t count;
} fr_detections;

/* Walks root, evaluates every catalog plugin's rules, settles claims on the
   same file (a contains match beats a name-only match, then the earlier
   catalog, and a remaining tie is an error naming every claimant unless one of
   them is in with), adds every id in with at the root, and collapses each
   plugin's matches to the outermost directories. An id in with that no catalog
   lists is an error naming it. The result is sorted by catalog order, then by
   directory.

   @implNote with is applied before the collapse, so a plugin named in with and
   also found in a subdirectory is reported once, at the root. */
int fr_detect(const char *root, const fr_catalog *catalog, const char *const *with,
              size_t with_count, fr_detections *out, fr_error *err);

void fr_detections_free(fr_detections *detections);

/* Exposed for the tests: "*" and "?" within one name, nothing else special. */
int fr_detect_name_matches(const char *pattern, const char *name);

#endif
