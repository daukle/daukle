#ifndef DAUKLE_SYNC_H
#define DAUKLE_SYNC_H

#include "types.h"
#include "registry.h"

#include <stddef.h>

typedef struct {
    char **files;
    size_t count;
} fr_sync_report;

/* A loaded manifest with its registry and its lua runtime still open.
   fr_sync opens one, syncs and closes it; a task run keeps it open across
   both, because a task's body is a callback in that runtime. */
typedef struct {
    fr_registry *registry;
    fr_manifest manifest;
    char *manifest_dir;
    char *manifest_path;
} fr_session;

int fr_build_registry(fr_registry **out, fr_error *err);
int fr_session_open(const char *manifest_path, int use_cache, fr_session *out, fr_error *err);
void fr_session_close(fr_session *session);
int fr_sync_session(const fr_session *session, int write, fr_sync_report *report, fr_error *err);
int fr_sync(const char *manifest_path, int write, int use_cache, fr_sync_report *report, fr_error *err);
void fr_sync_report_free(fr_sync_report *report);

/* On success it takes ownership even when it stores nothing: two producers can
   target one file, and a report naming it twice would have main.c count one
   changed file as two. */
int fr_sync_report_add(fr_sync_report *report, char *path, fr_error *err);

#endif
