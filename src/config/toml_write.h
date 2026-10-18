#ifndef DAUKLE_TOML_WRITE_H
#define DAUKLE_TOML_WRITE_H

#include "util/types.h"

#include <stddef.h>

struct cJSON;

/* Renders a JSON object as a TOML document and reads it back, refusing to hand
   over text that does not parse to the same document. Keys listed in first are
   written first, in that order, when present; the rest keep their order. null
   is refused, since TOML has no null. *out is the caller's to free.

   @implNote The read-back is the writer's specification: the renderer may be
   wrong about a shape it never met, and a document written to disk that reads
   back differently is worse than an error. */
int fr_toml_write(const struct cJSON *document, const char *const *first, size_t first_count,
                  char **out, fr_error *err);

#endif
