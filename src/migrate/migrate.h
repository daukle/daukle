#ifndef DAUKLE_MIGRATE_H
#define DAUKLE_MIGRATE_H

#include "util/types.h"

#include <stddef.h>

/* catalogs are the locations fr_catalog_locations already combined. with names
   catalog ids to add at the root; a coordinate outside every catalog is not
   accepted. */
typedef struct {
    const char *const *catalogs;
    size_t catalog_count;
    const char *const *with;
    size_t with_count;
    int use_cache;
} fr_migrate_options;

typedef enum {
    FR_MIGRATE_DONE,
    FR_MIGRATE_BLOCKED,
    FR_MIGRATE_UNPROVED
} fr_migrate_outcome;

/* Runs the whole pipeline in the current directory. FR_ERR is a failure of
   migrate itself (a fetch, a plugin error, a malformed catalog); the three
   outcomes are answers about the project. DONE: daukle.toml written, proved,
   the old files moved aside. BLOCKED: nothing written, the report printed.
   UNPROVED: daukle.toml written and kept, nothing moved, the differences
   printed. Every message goes to err or to stderr; stdout gets one line on
   FR_MIGRATE_DONE. */
int fr_migrate_run(const fr_migrate_options *options, fr_migrate_outcome *out_outcome,
                   fr_error *err);

/* Deletes build/daukle/migrate/old. FR_ERR naming it when there is nothing to finish. */
int fr_migrate_done(fr_error *err);

#endif
