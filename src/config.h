#ifndef DAUKLE_CONFIG_H
#define DAUKLE_CONFIG_H

#include "types.h"
#include "registry.h"

/* Dispatches on the file's extension alone, so the set of formats is whatever
   is registered and this module never names one. */
int fr_config_load_file(const char *file_path, fr_registry *registry, fr_manifest *out, fr_error *err);
int fr_config_find(const char *directory, const fr_registry *registry, char **out_path, fr_error *err);

#endif
