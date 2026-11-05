#ifndef DAUKLE_RESOLVE_H
#define DAUKLE_RESOLVE_H

#include "plugin/registry.h"
#include "util/types.h"

#include <stddef.h>

struct fr_resolved {
    char *project;
    char *module;
    struct cJSON *block;
};

int fr_resolve_toolchain(const fr_toolchain *toolchain, const fr_manifest *manifest,
                         const char *manifest_dir, const fr_registry *registry,
                         fr_resolved **out, size_t *out_count, fr_error *err);

void fr_resolved_free(fr_resolved *items, size_t count);

#endif
