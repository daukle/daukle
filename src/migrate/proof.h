#ifndef DAUKLE_MIGRATE_PROOF_H
#define DAUKLE_MIGRATE_PROOF_H

#include "util/types.h"

#include <stddef.h>

typedef enum { FR_PROOF_BYTES, FR_PROOF_ARCHIVE } fr_proof_kind;

/* FR_OK with *out_same set, or FR_ERR when a side cannot be read. difference is
   filled with one sentence when the two differ: which member is missing on which
   side, or which member's bytes differ, or the two digests. Paths are relative
   to root.

   An archive is compared by its file members' names and bytes. Order, directory
   members and timestamps are ignored, because a jar tool may write any of them
   differently for the same content. A symlink member is always a difference. */
int fr_proof_compare(const char *root, const char *old_path, const char *new_path,
                     fr_proof_kind kind, int *out_same, char *difference,
                     size_t difference_size, fr_error *err);

/* Accepts "bytes" or "archive". The one place that vocabulary is spelled. */
int fr_proof_kind_parse(const char *text, fr_proof_kind *out);

#endif
