#include "plugin/registry.h"

#include "util/error.h"

#include <stdlib.h>
#include <string.h>

#define FR_REGISTRY_FIRST_CAPACITY 16

/* Each plugin is its own allocation, so a pointer a lookup returned stays valid
   while later registrations grow the list. */
typedef struct {
    void **entries;
    size_t count;
    size_t capacity;
} plugin_list;

struct fr_registry {
    plugin_list sources;
    plugin_list configs;
    plugin_list toolchains;
    plugin_list tasks;
    plugin_list migrators;
    plugin_list translators;
};

fr_registry *fr_registry_create(void) {
    return calloc(1, sizeof(fr_registry));
}

static void list_release(plugin_list *list) {
    for (size_t index = 0; index < list->count; index++) free(list->entries[index]);
    free(list->entries);
}

void fr_registry_destroy(fr_registry *registry) {
    if (registry == NULL) return;
    list_release(&registry->sources);
    list_release(&registry->configs);
    list_release(&registry->toolchains);
    list_release(&registry->tasks);
    list_release(&registry->migrators);
    list_release(&registry->translators);
    free(registry);
}

/* Every plugin struct starts with its `const char *capability`. */
static const char *capability_of(const void *plugin) {
    return *(const char *const *) plugin;
}

static void *list_find(const plugin_list *list, const char *capability) {
    for (size_t index = 0; index < list->count; index++) {
        if (strcmp(capability_of(list->entries[index]), capability) == 0) return list->entries[index];
    }
    return NULL;
}

static void *list_at(const plugin_list *list, size_t index) {
    return index < list->count ? list->entries[index] : NULL;
}

static int list_reserve_one(plugin_list *list) {
    if (list->count < list->capacity) return FR_OK;
    size_t capacity = list->capacity == 0 ? FR_REGISTRY_FIRST_CAPACITY : list->capacity * 2;
    void **entries = realloc(list->entries, capacity * sizeof *entries);
    if (entries == NULL) return FR_ERR;
    list->entries = entries;
    list->capacity = capacity;
    return FR_OK;
}

static int list_add(plugin_list *list, const void *plugin, size_t size, fr_error *err) {
    const char *capability = capability_of(plugin);
    if (list_find(list, capability) != NULL) {
        fr_error_set(err, "capability \"%s\" is already registered", capability);
        return FR_ERR;
    }
    void *copy = malloc(size);
    if (copy == NULL || list_reserve_one(list) != FR_OK) {
        free(copy);
        fr_error_set(err, "cannot register \"%s\": out of memory", capability);
        return FR_ERR;
    }
    memcpy(copy, plugin, size);
    list->entries[list->count++] = copy;
    return FR_OK;
}

int fr_registry_add_source(fr_registry *registry, const fr_source_plugin *plugin, fr_error *err) {
    return list_add(&registry->sources, plugin, sizeof *plugin, err);
}

const fr_source_plugin *fr_registry_source(const fr_registry *registry, const char *capability) {
    return list_find(&registry->sources, capability);
}

int fr_registry_add_config(fr_registry *registry, const fr_config_plugin *plugin, fr_error *err) {
    return list_add(&registry->configs, plugin, sizeof *plugin, err);
}

const fr_config_plugin *fr_registry_config(const fr_registry *registry, const char *capability) {
    return list_find(&registry->configs, capability);
}

size_t fr_registry_config_count(const fr_registry *registry) {
    return registry->configs.count;
}

const fr_config_plugin *fr_registry_config_at(const fr_registry *registry, size_t index) {
    return list_at(&registry->configs, index);
}

int fr_registry_add_toolchain(fr_registry *registry, const fr_toolchain_plugin *plugin, fr_error *err) {
    return list_add(&registry->toolchains, plugin, sizeof *plugin, err);
}

const fr_toolchain_plugin *fr_registry_toolchain(const fr_registry *registry, const char *capability) {
    return list_find(&registry->toolchains, capability);
}

int fr_registry_add_task(fr_registry *registry, const fr_task_plugin *plugin, fr_error *err) {
    return list_add(&registry->tasks, plugin, sizeof *plugin, err);
}

const fr_task_plugin *fr_registry_task(const fr_registry *registry, const char *capability) {
    return list_find(&registry->tasks, capability);
}

size_t fr_registry_task_count(const fr_registry *registry) {
    return registry->tasks.count;
}

const fr_task_plugin *fr_registry_task_at(const fr_registry *registry, size_t index) {
    return list_at(&registry->tasks, index);
}

int fr_registry_add_migrator(fr_registry *registry, const fr_migrator_plugin *plugin, fr_error *err) {
    return list_add(&registry->migrators, plugin, sizeof *plugin, err);
}

int fr_registry_add_translator(fr_registry *registry, const fr_translator_plugin *plugin,
                               fr_error *err) {
    return list_add(&registry->translators, plugin, sizeof *plugin, err);
}

size_t fr_registry_migrator_count(const fr_registry *registry) {
    return registry->migrators.count;
}

const fr_migrator_plugin *fr_registry_migrator_at(const fr_registry *registry, size_t index) {
    return list_at(&registry->migrators, index);
}

const fr_migrator_plugin *fr_registry_migrator_of(const fr_registry *registry, const char *label) {
    for (size_t index = 0; index < registry->migrators.count; index++) {
        const fr_migrator_plugin *candidate = registry->migrators.entries[index];
        if (strcmp(candidate->label, label) == 0) return candidate;
    }
    return NULL;
}

const fr_translator_plugin *fr_registry_translator(const fr_registry *registry, const char *tool,
                                                   const char *id) {
    for (size_t index = 0; index < registry->translators.count; index++) {
        const fr_translator_plugin *candidate = registry->translators.entries[index];
        if (strcmp(candidate->tool, tool) == 0 && strcmp(candidate->id, id) == 0) return candidate;
    }
    return NULL;
}
