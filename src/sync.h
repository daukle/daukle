#ifndef DAUKLE_SYNC_H
#define DAUKLE_SYNC_H

#include "types.h"
#include "registry.h"

#include <stddef.h>

typedef struct {
    char **files;
    size_t count;
} fr_sync_report;

int fr_build_registry(fr_registry **out, fr_error *err);
int fr_sync(const char *manifest_path, int write, int use_cache, fr_sync_report *report, fr_error *err);
void fr_sync_report_free(fr_sync_report *report);

/* On success it takes ownership even when it stores nothing: two producers can
   target one file, and a report naming it twice would have main.c count one
   changed file as two. */
int fr_sync_report_add(fr_sync_report *report, char *path, fr_error *err);

#endif
