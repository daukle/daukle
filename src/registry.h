#ifndef DAUKLE_REGISTRY_H
#define DAUKLE_REGISTRY_H

#include "types.h"

#include <stddef.h>

typedef struct fr_registry fr_registry;

typedef struct {
    const char *capability;
    int (*load)(void *state, const char *project, const struct cJSON *block,
                const char *base_dir, fr_project *out, fr_error *err);
    void *state;
} fr_source_plugin;

typedef struct fr_resolved fr_resolved;

typedef struct {
    const char *capability;
    int (*apply)(void *state, const fr_consumer *consumer,
                 const fr_resolved *resolved, size_t count,
                 const char *original_text, char **out_text, fr_error *err);
    void *state;
} fr_language_plugin;

typedef struct {
    const char *capability;
    const char *file_name;
    int overlay;
    int (*load)(void *state, const char *text, const char *origin, const char *base_dir,
                fr_registry *registry, const struct cJSON *document,
                struct cJSON **out, fr_error *err);
    void *state;
} fr_config_plugin;

fr_registry *fr_registry_create(void);
void fr_registry_destroy(fr_registry *registry);
int fr_registry_add_source(fr_registry *registry, const fr_source_plugin *plugin, fr_error *err);
int fr_registry_add_language(fr_registry *registry, const fr_language_plugin *plugin, fr_error *err);
int fr_registry_add_config(fr_registry *registry, const fr_config_plugin *plugin, fr_error *err);
const fr_source_plugin *fr_registry_source(const fr_registry *registry, const char *capability);
const fr_language_plugin *fr_registry_language(const fr_registry *registry, const char *capability);
const fr_config_plugin *fr_registry_config(const fr_registry *registry, const char *capability);
size_t fr_registry_config_count(const fr_registry *registry);
const fr_config_plugin *fr_registry_config_at(const fr_registry *registry, size_t index);

#endif
