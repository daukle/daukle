#ifndef DAUKLE_RESOLVE_H
#define DAUKLE_RESOLVE_H

#include "types.h"
#include "registry.h"

#include <stddef.h>

struct fr_resolved {
    char *project;
    char *module;
    struct cJSON *block;
};

int fr_resolve_consumer(const fr_consumer *consumer, const fr_manifest *manifest,
                        const char *manifest_dir, const fr_registry *registry,
                        fr_resolved **out, size_t *count, fr_error *err);

/* A toolchain's dependencies resolve through the consumer resolver, with the
   toolchain's name where the language's would go: that name is also what
   selects the per-module block, so managed and adopted mode see the same
   producer data. One resolver, one block-selection rule. */
int fr_resolve_toolchain(const fr_toolchain *toolchain, const fr_manifest *manifest,
                         const char *manifest_dir, const fr_registry *registry,
                         fr_resolved **out, size_t *out_count, fr_error *err);

void fr_resolved_free(fr_resolved *items, size_t count);

#endif
