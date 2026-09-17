#ifndef DAUKLE_SYNC_H
#define DAUKLE_SYNC_H

#include "types.h"

#include <stddef.h>

typedef struct {
    char **files;
    size_t count;
} fr_sync_report;

int fr_sync(const char *manifest_path, int write, int use_cache, fr_sync_report *report, fr_error *err);
void fr_sync_report_free(fr_sync_report *report);

#endif
