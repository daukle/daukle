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

/* Deletes fr_derived_root(manifest_dir) and everything beneath it. Existence
   is checked first: a root that does not exist, including a dangling link,
   is FR_OK, since cleaning a project that never generated is not a failure,
   and the link check below never runs for it. Once the root is known to
   exist, refuses outright if it is itself a symlink or a junction: the
   derived root is legitimately a plain directory, never a link, and a
   base-versus-target compare cannot catch this case on its own, since a link
   whose target happens to sit inside the base compares as contained even
   though following it deletes whatever it actually points at. Otherwise the
   root is canonicalised with fr_lua_sandbox_canonical_dir and the result is
   asserted to be a strict descendant of the canonical form of manifest_dir,
   via fr_derived_root_is_contained below, the same rule lua_sandbox.c's own
   within_base_dir applies for a read (equality does not count; the base
   directory itself is never the thing to delete). Deletion is the one
   operation in this codebase whose failure mode is unrecoverable data loss,
   so both checks are a precondition rather than the notice-and-continue
   treatment fr_derived_apply gives an escaping path.
   The removal walk deletes a symlink or a junction it meets below the root
   rather than following it out of the tree, and it is best-effort per file,
   so the outcome is re-checked once it finishes: anything left behind (a
   locked file, say) is reported as FR_ERR rather than claimed as a success
   that did not happen. */
int fr_derived_clean(const char *manifest_dir, fr_error *err);

/* The containment predicate fr_derived_clean applies to a canonicalised root
   against a canonicalised base: a strict descendant passes; the base itself,
   or anything sharing only a name prefix with it, does not. Exposed, rather
   than kept static, so test_derived.c can drive it directly with string pairs:
   canonicalisation applied identically to both sides cannot itself diverge
   without a real symlink or junction on disk, so this predicate's own
   boundary logic is the one part of the containment check a path-only test
   can exercise on every machine. Every real caller reaches it only through
   fr_derived_clean. */
int fr_derived_root_is_contained(const char *canonical_base, const char *canonical_root);

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
