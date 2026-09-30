#include "project/lang_region.h"

#include "project/region.h"

#include <stdlib.h>

int fr_lang_region_apply(const char *begin_marker, const char *end_marker, fr_lang_render render,
                         const fr_consumer *consumer, const fr_resolved *resolved, size_t count,
                         const char *original_text, char **out_text, fr_error *err) {
    *out_text = NULL;

    char *rendered = NULL;
    if (render(consumer, resolved, count, &rendered, err) != FR_OK) return FR_ERR;

    int result = fr_region_replace(original_text, begin_marker, end_marker, rendered, out_text, err);
    free(rendered);
    return result;
}
