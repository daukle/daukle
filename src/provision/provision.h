#ifndef DAUKLE_PROVISION_H
#define DAUKLE_PROVISION_H

#include "archive/unpack.h"
#include "net/http.h"
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
/* headers are sent verbatim with the fetch and may be NULL. They exist so a
   toolchain archive behind a credential can be provisioned at all; the value
   reaching here is already resolved, so this layer never learns the name of the
   variable it came from. The cache key is the content digest alone, so a tree
   fetched once with a credential is readable afterwards without one: the
   credential protects the download, not the cache. D-32. */
int fr_provision(const char *url, const char *sha256_hex, const fr_http_header *headers,
                 size_t header_count, fr_provision_result *out, fr_error *err);

typedef struct {
    char path[1024];
    int was_cached;
} fr_artifact_result;

/* Fetches a single pinned file, verifies it and KEEPS it, without unpacking and
   without asking what it holds. It exists because unpacking can change what an
   archive means: a multi-release jar resolves its versioned classes as a file
   on a classpath and silently serves its base classes as the same content in a
   directory, so a dependency has to stay the file it was published as. D-52.
   @implNote the cache hit is decided by the FILE and not by its directory,
   because two urls serving one digest differ only in the name they suggest and
   the second must still resolve rather than find the first's name sitting
   alone. The pin is mandatory on the same grounds as fr_provision's. */
int fr_artifact(const char *url, const char *sha256_hex, const fr_http_header *headers,
                size_t header_count, fr_artifact_result *out, fr_error *err);

typedef struct {
    char path[1024];
    char sha256[65];
} fr_pin_result;

/* Fetches an UNPINNED file and reports the digest it computed on the way, so a
   resolver can emit a pin for bytes nothing had a pin for. It is the one
   acquisition in daukle that does not verify, which is why its caller is gated
   to an explicit resolve run: see fr_lua_verbs_set_resolving. D-77.
   @implNote it lands the file under the same <artifacts>/<digest>/<name> key
   fr_artifact reads, so the pinned fetch that follows a resolve is a cache hit
   rather than a second download of the same bytes. There is no was_cached: the
   key is not known until the bytes are, so the probe fr_artifact makes before
   fetching has nothing to probe with. */
int fr_artifact_pin(const char *url, const fr_http_header *headers, size_t header_count,
                    fr_pin_result *out, fr_error *err);

#endif
