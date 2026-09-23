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
    int (*generate)(void *state, const fr_toolchain *toolchain, const char *project,
                    const char *version, const char *root,
                    const fr_resolved *resolved, size_t count,
                    fr_generated_file **out_files, size_t *out_count, fr_error *err);
    void *state;
} fr_toolchain_plugin;

/* What a task's run callback is handed. fr_tasks_run builds this only for a
   task whose plugin has a run callback, and every such task carries a
   toolchain (a bare task with no toolchain in its name is refused a run
   callback at collection time), so toolchain and derived_dir_relative are
   never NULL here. */
typedef struct {
    const char *name;
    const fr_toolchain *toolchain;
    const char *project;
    const char *version;
    const char *root;
    /* The toolchain's derived directory relative to the sandbox base (the
       manifest directory), e.g. "build/daukle/cmake", not a filesystem path:
       this is what fr_lua_task_cwd publishes for daukle.exec's cwd default,
       and daukle.exec's cwd is always resolved relative to that same base. */
    const char *derived_dir_relative;
    const fr_resolved *resolved;
    size_t resolved_count;
} fr_task_run_context;

/* run == NULL is an aggregator: a name for a group, with nothing to execute.
   depends_on and part_of are borrowed from whatever declared the task and
   outlive the registry entry. */
typedef struct {
    const char *capability;
    const char *part_of;
    const char *const *depends_on;
    size_t depends_on_count;
    int (*run)(void *state, const fr_task_run_context *context, fr_error *err);
    void *state;
} fr_task_plugin;

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
int fr_registry_add_toolchain(fr_registry *registry, const fr_toolchain_plugin *plugin, fr_error *err);
int fr_registry_add_task(fr_registry *registry, const fr_task_plugin *plugin, fr_error *err);
const fr_source_plugin *fr_registry_source(const fr_registry *registry, const char *capability);
const fr_language_plugin *fr_registry_language(const fr_registry *registry, const char *capability);
const fr_config_plugin *fr_registry_config(const fr_registry *registry, const char *capability);
const fr_toolchain_plugin *fr_registry_toolchain(const fr_registry *registry, const char *capability);
const fr_task_plugin *fr_registry_task(const fr_registry *registry, const char *capability);
size_t fr_registry_config_count(const fr_registry *registry);
const fr_config_plugin *fr_registry_config_at(const fr_registry *registry, size_t index);
size_t fr_registry_task_count(const fr_registry *registry);
const fr_task_plugin *fr_registry_task_at(const fr_registry *registry, size_t index);

#endif
