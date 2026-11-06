#ifndef DAUKLE_MIGRATE_PROOF_INTERNAL_H
#define DAUKLE_MIGRATE_PROOF_INTERNAL_H

#include "migrate/proof.h"
#include "util/types.h"

#include <stddef.h>

int fr_proof_compare_archives(const char *old_path, const char *new_path, int json_values, int *out_same,
                              char *difference, size_t difference_size, fr_error *err);

/* Both paths are already joined to the root. */
int fr_proof_list_archive_differences(const char *old_path, const char *new_path, int json_values,
                                      fr_proof_differences *out, fr_error *err);

/* Sets truncated rather than growing past FR_PROOF_MAX_DIFFERENCES. */
int fr_proof_differences_add(fr_proof_differences *list, const char *member,
                             fr_proof_difference_kind kind, fr_error *err);

void fr_proof_describe_archive_difference(const fr_proof_difference *difference, char *out,
                                          size_t size);

#endif
