#ifndef DAUKLE_DERIVED_H
#define DAUKLE_DERIVED_H

#include "types.h"

#include <stddef.h>

typedef struct {
    char **paths;   /* full paths (derived_dir joined with each file's relative
                        path), not merely relative to the manifest dir */
    size_t count;
} fr_derived_report;

int fr_derived_root(const char *manifest_dir, char **out_dir, fr_error *err);
int fr_derived_dir(const char *manifest_dir, const char *toolchain, char **out_dir, fr_error *err);

/* Creates derived_root, if needed, and writes "*\n" to
   <derived_root>/.gitignore when that file is absent, never overwriting one
   that exists: a user who edited it had a reason. Not called by
   fr_derived_apply, which only ever touches the directory it is handed and
   never the toolchain-agnostic root above it; the caller that resolved
   derived_root through fr_derived_root calls this once before generating. */
int fr_derived_ensure_root(const char *derived_root, fr_error *err);

/* Deletes fr_derived_root(manifest_dir) and everything beneath it. The root is
   canonicalised with fr_lua_sandbox_canonical_dir and the result is asserted
   to sit inside the canonical form of manifest_dir before anything is
   removed: deletion is the one operation in this codebase whose failure mode
   is unrecoverable data loss, so containment here is a precondition rather
   than the notice-and-continue treatment fr_derived_apply gives an escaping
   path. A root that does not exist is FR_OK, since cleaning a project that
   never generated is not a failure, and the removal walk deletes a symlink or
   a junction it meets rather than following it out of the tree. */
int fr_derived_clean(const char *manifest_dir, fr_error *err);

/* write == 0 reports what would change and touches nothing, including the
   ledger, so "check" and "sync" differ only in whether they write. A file the
   ledger records but whose digest no longer matches was changed outside daukle:
   it is overwritten when still generated and KEPT when not, because deleting a
   file someone edited is unrecoverable where a stale one is merely visible. A
   ledger line or a generated path that could escape derived_dir is refused
   outright: this directory is meant to hold whatever a plugin tool writes
   beside daukle's own files, and a planted ledger line must never turn into a
   delete of something outside it. */
int fr_derived_apply(const char *derived_dir, const fr_generated_file *files, size_t count,
                     int write, fr_derived_report *report, fr_error *err);
void fr_derived_report_free(fr_derived_report *report);
void fr_derived_free_files(fr_generated_file *files, size_t count);

/* NULL (the default) discards notices; library code never prints, so the caller
   supplies a sink if it wants an overwrite or a keep surfaced. Mirrors
   fr_lua_set_log_sink. */
void fr_derived_set_notice_sink(void (*sink)(const char *message));

#endif
