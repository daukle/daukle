#ifndef DAUKLE_GENERATE_H
#define DAUKLE_GENERATE_H

#include "registry.h"
#include "sync.h"
#include "types.h"

/* Resolves each toolchain manifest->toolchains names, hands its plugin what it
   needs to generate, validates what comes back, and hands the result to
   derived.c. write == 0 reports what would change and touches nothing, the
   same contract fr_sync and fr_derived_apply already share. */
int fr_generate(const fr_manifest *manifest, const char *manifest_path, const char *manifest_dir,
                const fr_registry *registry, int write, fr_sync_report *report, fr_error *err);

#endif
