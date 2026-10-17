#ifndef DAUKLE_MIGRATE_INTERNAL_H
#define DAUKLE_MIGRATE_INTERNAL_H

#include "migrate/catalog.h"
#include "migrate/detect.h"
#include "migrate/migrate.h"
#include "migrate/result.h"
#include "util/types.h"

#include <stddef.h>

struct cJSON;

/* One migrator's answer, kept past session A.
   @implNote every string is an owned copy: the migrator's own strings live in
   session A's Lua slots, which die when that runtime shuts down. */
typedef struct {
    char *label;
    char *tool;
    char *toolchain;
    char *instance;
    char *dir;
    char *root;
    char *derived_dir_relative;
    struct cJSON *result;
} fr_migrate_kept;

/* A foreign plugin a migrator reported, paired with the one catalog plugin
   claiming it. foreign is borrowed from the kept result that named it. */
typedef struct {
    size_t kept_index;
    const struct cJSON *foreign;
    const fr_catalog_plugin *replacer;
    struct cJSON *translation;
} fr_migrate_foreign;

/* considered holds every catalog plugin migrate decided to pin, detected ones
   first, each once; one with no asset is already a blocker and is never loaded. */
typedef struct {
    const fr_migrate_options *options;
    fr_catalog catalog;
    fr_detections detections;
    const fr_catalog_plugin **considered;
    size_t considered_count;
    size_t detected_count;
    fr_migrate_kept *kept;
    size_t kept_count;
    fr_migrate_foreign *foreign;
    size_t foreign_count;
    fr_blockers blockers;
    struct cJSON *manifest;
} fr_migrate_state;

int fr_migrate_core_blocker(fr_migrate_state *state, const char *construct, const char *where,
                            const char *why, fr_error *err);

/* Records plugin among those to pin, once per id. A plugin with no asset has
   nothing to pin and becomes a blocker the first time it is seen. */
int fr_migrate_consider(fr_migrate_state *state, const fr_catalog_plugin *plugin, fr_error *err);

/* Session A: loads the detected plugins, names each detection's instance and
   keeps what its migrator's run returned. */
int fr_migrate_session_a(fr_migrate_state *state, fr_error *err);

/* Pairs every foreign plugin a kept result names with the catalog plugin whose
   replaces claims it, and considers that plugin for pinning. */
int fr_migrate_collect_foreign(fr_migrate_state *state, fr_error *err);

/* Session B: loads every considered plugin, runs each translator, assembles the
   manifest, and when nothing blocks runs each migrator's build. */
int fr_migrate_session_b(fr_migrate_state *state, fr_error *err);

/* Merges every migrator manifest and then every translation into
   state->manifest, records each instance and fills what core owns. */
int fr_migrate_assemble(fr_migrate_state *state, fr_error *err);

/* Writes daukle.toml, proves it in session C, compares, moves the old files. */
int fr_migrate_finish(fr_migrate_state *state, fr_migrate_outcome *out_outcome, fr_error *err);

#endif
