#ifndef DAUKLE_LANG_REGION_H
#define DAUKLE_LANG_REGION_H

#include "resolve.h"
#include "types.h"

#include <stddef.h>

typedef int (*fr_lang_render)(const fr_consumer *consumer, const fr_resolved *resolved,
                              size_t count, char **out_text, fr_error *err);

int fr_lang_region_apply(const char *begin_marker, const char *end_marker, fr_lang_render render,
                         const fr_consumer *consumer, const fr_resolved *resolved, size_t count,
                         const char *original_text, char **out_text, fr_error *err);

#endif
