#ifndef DAUKLE_REGION_H
#define DAUKLE_REGION_H

#include "types.h"

#include <stddef.h>

int fr_region_replace(const char *original, const char *begin_marker, const char *end_marker,
                      const char *replacement, char **out, fr_error *err);
int fr_file_read_text(const char *path, char **out, fr_error *err);

/* fr_file_read_text plus the byte count, for a caller whose content may hold an
   embedded NUL that strlen would not see. The buffer is NUL-terminated one past
   *out_length either way. */
int fr_file_read_bytes(const char *path, char **out, size_t *out_length, fr_error *err);
int fr_file_write_text(const char *path, const char *text, fr_error *err);

#endif
