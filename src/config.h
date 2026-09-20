#ifndef DAUKLE_CONFIG_H
#define DAUKLE_CONFIG_H

#include "types.h"
#include "registry.h"

/* Dispatches on the file's extension alone, so the set of formats is whatever
   is registered and this module never names one. */
int fr_config_load_file(const char *file_path, fr_registry *registry, fr_manifest *out, fr_error *err);
int fr_config_find(const char *directory, const fr_registry *registry, char **out_path, fr_error *err);

/* The registered format that owns file_path's extension, or NULL with err set
   naming the extension. Exposed so a caller reading a manifest for itself,
   rather than through fr_config_load_file, dispatches through the registry
   instead of keeping a second extension-to-format table of its own. */
const fr_config_plugin *fr_config_plugin_for(const fr_registry *registry, const char *file_path,
                                             fr_error *err);

#endif
