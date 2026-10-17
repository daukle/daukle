#ifndef DAUKLE_MIGRATE_RESULT_H
#define DAUKLE_MIGRATE_RESULT_H

#include "util/types.h"

#include <stddef.h>
#include <stdio.h>

struct cJSON;

typedef struct {
    char *construct;
    char *where;
    char *why;
    char *surface;   /* the surface-file row, or NULL for "no row yet" */
    char *source;    /* who reported it: "migrator <label>", "translator <tool>/<id>", "core" */
} fr_blocker;

typedef struct {
    fr_blocker *items;
    size_t count;
} fr_blockers;

int fr_blockers_add(fr_blockers *blockers, const char *source, const char *construct,
                    const char *where, const char *why, const char *surface, fr_error *err);
void fr_blockers_free(fr_blockers *blockers);

/* Checks the shape run returned and records each blocker it carries.
   Malformed output is an ERROR naming the plugin, because it is a plugin bug and
   not a fact about the user's project. Validates: manifest an object or absent;
   no "plugins" or "schema" key in it (core owns both); foreign a list of
   { kind, id, config? }; blockers a list of { construct, where, why, surface? };
   owned a list of safe relative paths (no climbing, no "daukle.toml",
   "daukle.lua", and nothing that is or contains build/daukle: "build", "." and
   anything below build/daukle); proof { task, compare } with compare a list of
   { old, new, as = "bytes"|"archive" } of safe paths. An empty compare list, or
   no proof at all, is a BLOCKER, not an error: the migration cannot be proved.

   @implNote Lua converts an empty table to a JSON object, so every list field
   accepts the empty object as an empty list. */
int fr_migrate_check_run_result(const struct cJSON *result, const char *label,
                                fr_blockers *blockers, fr_error *err);

/* The same for a translator: manifest, blockers. */
int fr_migrate_check_translation(const struct cJSON *result, const char *source,
                                 fr_blockers *blockers, fr_error *err);

/* Prints every blocker, grouped by source, to out. */
void fr_blockers_print(const fr_blockers *blockers, FILE *out);

#endif
