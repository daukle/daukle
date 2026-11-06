#ifndef DAUKLE_MIGRATE_PROOF_H
#define DAUKLE_MIGRATE_PROOF_H

#include "util/types.h"

#include <stddef.h>

struct cJSON;

typedef enum { FR_PROOF_BYTES, FR_PROOF_ARCHIVE, FR_PROOF_ARCHIVE_JSON } fr_proof_kind;

/* FR_OK with *out_same set, or FR_ERR when a side cannot be read. difference is
   filled with one sentence when the two differ: which member is missing on which
   side, or which member's bytes differ, or the two digests. Paths are relative
   to root.

   An archive is compared by its file members' names and bytes. Order, directory
   members and timestamps are ignored, because a jar tool may write any of them
   differently for the same content. A symlink member is always a difference.
   FR_PROOF_ARCHIVE_JSON also compares a member named "*.json" by its parsed value, whitespace and
   key order aside, for a tool that renders a manifest into the archive itself. */
int fr_proof_compare(const char *root, const char *old_path, const char *new_path,
                     fr_proof_kind kind, int *out_same, char *difference,
                     size_t difference_size, fr_error *err);

/* Accepts "bytes", "archive" or "archive-json". The one place that vocabulary is spelled. */
int fr_proof_kind_parse(const char *text, fr_proof_kind *out);

typedef enum {
    FR_PROOF_ONLY_OLD,
    FR_PROOF_ONLY_NEW,
    FR_PROOF_DIFFERENT_BYTES,
    FR_PROOF_SYMLINK_OLD,
    FR_PROOF_SYMLINK_NEW
} fr_proof_difference_kind;

/* member is NULL when two files are compared as bytes. */
typedef struct {
    char *member;
    fr_proof_difference_kind kind;
} fr_proof_difference;

/* The digests are filled for a bytes comparison only. truncated is set once
   FR_PROOF_MAX_DIFFERENCES entries are listed and more differ. */
typedef struct {
    fr_proof_difference *items;
    size_t count;
    size_t capacity;
    int truncated;
    char old_digest[65];
    char new_digest[65];
} fr_proof_differences;

#define FR_PROOF_MAX_DIFFERENCES 10000u

/* Every difference fr_proof_compare would stop at the first of, in member
   order, with root NULL taking both paths as given. Free out with
   fr_proof_differences_free. */
int fr_proof_list_differences(const char *root, const char *old_path, const char *new_path,
                              fr_proof_kind kind, fr_proof_differences *out, fr_error *err);

void fr_proof_differences_free(fr_proof_differences *list);

/* "only-old", "only-new", "bytes", "symlink-old" or "symlink-new". */
const char *fr_proof_difference_kind_name(fr_proof_difference_kind kind);

/* A list of { member?, kind, oldSha256?, newSha256? }, the digests only for a
   bytes comparison. NULL when out of memory. */
struct cJSON *fr_proof_differences_json(const fr_proof_differences *list);

#endif
