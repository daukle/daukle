#ifndef DAUKLE_CACHE_H
#define DAUKLE_CACHE_H

#include "types.h"

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
void fr_cache_set_enabled(int enabled);

#endif
