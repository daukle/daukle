#ifndef DAUKLE_CACHE_H
#define DAUKLE_CACHE_H

#include "types.h"

/* The directory every cache entry lives under, for a caller that lays out its
   own cache shape beneath it rather than the project/version/artifact one
   fr_cache_path builds, so there remains exactly one place that computes it. */
int fr_cache_root(char *out, size_t out_size, fr_error *err);

/* Answers whether text may be spliced into a cache path as one component:
   no "..", no leading separator, no backslash, no ':', no trailing-dot
   component (Windows strips it, aliasing two names to one directory), and
   with allow_one_slash exactly one interior slash with a non-empty tail,
   which is the "owner/name" shape. A caller laying out its own cache shape
   beneath fr_cache_root (see it) builds paths this module never sees, so it
   needs this same rule rather than a second copy of it: two containment
   checks are two checks that will disagree, and the one that disagrees is
   the one that lets a path out of the cache root. */
int fr_cache_component_is_safe(const char *text, int allow_one_slash);

/* artifact identifies WHICH artifact the entry holds, beyond the project and
   version naming it: two manifests can name one project id and version and
   resolve them from different repositories, and serving one for the other
   renders silently wrong coordinates. Any string that distinguishes them will
   do, so a source plugin passes whatever it resolved (for github-releases,
   the release asset url). */
int fr_cache_path(const char *project, const char *version, const char *artifact,
                  char **out_path, fr_error *err);
int fr_cache_read(const char *project, const char *version, const char *artifact,
                  char **out_text, fr_error *err);
/* Writes length bytes, so what is cached is byte-identical to what was
   fetched and a later hit parses exactly as the fetch did. Bounding the write
   by strlen instead would silently truncate a body containing a NUL. */
void fr_cache_write(const char *project, const char *version, const char *artifact,
                    const char *text, size_t length);

/* The atomic tmp-then-rename write fr_cache_write uses internally, exposed for
   any other cache shape needing the same guarantee (see fr_cache_root), so
   there is exactly one atomic-replace implementation rather than two that
   could drift. path is mutated and restored while creating ancestor
   directories, so it must be a writable buffer, not a string literal. */
int fr_cache_write_atomic(char *path, const char *text, size_t length);

void fr_cache_set_enabled(int enabled);

/* Lets a caller with its own cache shape (see fr_cache_root) obey --no-cache
   the same way fr_cache_read and fr_cache_write already do, rather than
   duplicating the flag. */
int fr_cache_enabled(void);

#endif
