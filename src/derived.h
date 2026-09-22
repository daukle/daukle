#ifndef DAUKLE_DERIVED_H
#define DAUKLE_DERIVED_H

#include "types.h"

#include <stddef.h>

typedef struct {
    char **paths;
    size_t count;
} fr_derived_report;

int fr_derived_root(const char *manifest_dir, char **out_dir, fr_error *err);
int fr_derived_dir(const char *manifest_dir, const char *toolchain, char **out_dir, fr_error *err);

/* write == 0 reports what would change and touches nothing, including the
   ledger, so "check" and "sync" differ only in whether they write. A file the
   ledger records but whose digest no longer matches was changed outside daukle:
   it is overwritten when still generated and KEPT when not, because deleting a
   file someone edited is unrecoverable where a stale one is merely visible. */
int fr_derived_apply(const char *derived_dir, const fr_generated_file *files, size_t count,
                     int write, fr_derived_report *report, fr_error *err);
void fr_derived_report_free(fr_derived_report *report);
void fr_derived_free_files(fr_generated_file *files, size_t count);

/* NULL (the default) discards notices; library code never prints, so the caller
   supplies a sink if it wants an overwrite or a keep surfaced. Mirrors
   fr_lua_set_log_sink. */
void fr_derived_set_notice_sink(void (*sink)(const char *message));

#endif
