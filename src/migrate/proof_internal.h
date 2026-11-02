#ifndef DAUKLE_MIGRATE_PROOF_INTERNAL_H
#define DAUKLE_MIGRATE_PROOF_INTERNAL_H

#include "util/types.h"

#include <stddef.h>

int fr_proof_compare_archives(const char *old_path, const char *new_path, int *out_same,
                              char *difference, size_t difference_size, fr_error *err);

#endif
