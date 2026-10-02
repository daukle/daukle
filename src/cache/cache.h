#ifndef DAUKLE_CACHE_H
#define DAUKLE_CACHE_H

#include "util/types.h"

/* The directory every cache entry lives under, for a caller that lays out its
   own cache shape beneath it rather than the project/version/artifact one
   fr_cache_path builds, so there remains exactly one place that computes it. */
int fr_cache_root(char *out, size_t out_size, fr_error *err);

/* The directory a provisioned toolchain tree lives under, built on
   fr_cache_root (see it) rather than recomputing the root. */
int fr_cache_toolchains_root(char *out, size_t out_size, fr_error *err);

/* The directory a pinned artifact lives under, built on fr_cache_root (see it)
   rather than recomputing the root. Separate from the toolchains root because
   an artifact is a FILE a plugin names and a toolchain is a TREE it runs from,
   so "a present entry is a complete one" is a statement about a file in one and
   about a directory in the other. */
int fr_cache_artifacts_root(char *out, size_t out_size, fr_error *err);

/* Whether path names an existing directory. The provisioning probe and the
   rename below both ask whether a tree is already there, and two answers to
   one question are two answers that will disagree. */
int fr_cache_directory_exists(const char *path);

/* Whether path names an existing regular file, and the file counterpart of
   fr_cache_directory_exists for the same reason: an artifact's presence is
   what decides its cache hit, and a directory of that name is not a hit. */
int fr_cache_file_exists(const char *path);

/* Moves a finished file onto its final path, replacing whatever is there, for
   a cache shape whose entries are files it did not write through
   fr_cache_write_atomic (see fr_cache_root). Replacing is correct where the
   path is content addressed, since anything already at it holds the same
   bytes by construction. */
int fr_cache_rename_file(const char *from, const char *to);

/* Creates path and every ancestor of it, for a cache shape (see fr_cache_root)
   whose entries are directories rather than the files fr_cache_write_atomic
   makes. Silent like the rest: a directory that could not be created simply
   makes the later write fail. path is mutated and restored, so it must be a
   writable buffer, not a string literal. */
void fr_cache_make_directories(char *path);

/* Moves a fully built directory into its final place without replacing what is
   already there, and reports success when the target exists afterwards either
   way: a target that already exists means another daukle finished the same
   work first, and its tree is the one to use. */
int fr_cache_rename_directory(const char *from, const char *to);

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

/* A second, independent flag from fr_cache_set_enabled: refreshing makes
   fr_cache_read report a miss unconditionally (so a producer always reruns)
   while leaving fr_cache_write untouched, so the fresh answer IS persisted.
   --no-cache wants both reads and writes off, which fr_cache_set_enabled(0)
   already gives it; "daukle plugin update" wants only reads bypassed, so the
   resolver's own coordinate-to-url cache is overwritten rather than merely
   ignored for one call. Has no effect while fr_cache_enabled() is false,
   since a disabled cache already treats every read as a miss and every write
   as a no-op. */
void fr_cache_set_refreshing(int refreshing);
int fr_cache_refreshing(void);

#endif
