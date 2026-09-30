#ifndef DAUKLE_PROVISION_H
#define DAUKLE_PROVISION_H

#include "archive/unpack.h"
#include "util/types.h"

#include <stddef.h>

typedef struct {
    char root[1024];
    int was_cached;
    fr_unpack_report unpack;
} fr_provision_result;

/* The directory a digest's tree occupies, whether or not it exists. Keyed on
   the content digest alone, so every url serving the same bytes shares one
   tree and a url that starts serving different bytes cannot land in it. */
int fr_provision_root_path(const char *sha256_hex, char *out, size_t out_size, fr_error *err);

/* Probes the cache, and only on a miss fetches, verifies and unpacks. The pin
   is what makes the cache path known before any request is made, which is why
   it is mandatory: a provisioned toolchain costs no network call at all.
   @implNote fr_cache_enabled is deliberately not consulted: the key IS the bytes, so no
   entry can go stale and a refetch either costs time or is refused by the digest. Repairing
   a tree corrupted on disk is a cache command's concern, not a fetch-freshness flag's. */
int fr_provision(const char *url, const char *sha256_hex, fr_provision_result *out, fr_error *err);

#endif
