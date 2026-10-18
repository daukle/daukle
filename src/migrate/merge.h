#ifndef DAUKLE_MIGRATE_MERGE_H
#define DAUKLE_MIGRATE_MERGE_H

#include "migrate/result.h"
#include "util/types.h"

struct cJSON;

/* Deep-merges fragment into into. Two objects merge key by key; equal values
   are fine; any other collision is a blocker naming the dotted key and both
   sources (owner_of records who set each key path first). */
int fr_migrate_merge(struct cJSON *into, const struct cJSON *fragment, const char *source,
                     struct cJSON *owner_of, fr_blockers *blockers, fr_error *err);

/* Writes "toolchain" and "dir" into toolchains.<instance>, creating the table
   when the migrator wrote none. toolchain is written only when it differs from
   instance, and dir only when it is not the root (NULL or empty). */
int fr_migrate_set_instance(struct cJSON *manifest, const char *instance, const char *toolchain,
                            const char *dir, fr_error *err);

/* schema, project and version, each only when absent: project defaults to
   project_name, version to "0.1.0", and modules to an empty table. */
int fr_migrate_fill_defaults(struct cJSON *manifest, const char *project_name, fr_error *err);

/* [plugins].<id> = { url, sha256 } for each plugin migrate loaded. */
int fr_migrate_add_plugin(struct cJSON *manifest, const char *id, const char *url,
                          const char *sha256, fr_error *err);

#endif
