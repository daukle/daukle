#ifndef TERKO_REGION_H
#define TERKO_REGION_H

#include "types.h"

int fr_region_replace(const char *original, const char *begin_marker, const char *end_marker,
                      const char *replacement, char **out, fr_error *err);
int fr_file_read_text(const char *path, char **out, fr_error *err);
int fr_file_write_text(const char *path, const char *text, fr_error *err);

#endif
