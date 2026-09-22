#include "registry.h"

#include "error.h"

#include <stdlib.h>
#include <string.h>

#define FR_REGISTRY_CAPACITY 16

struct fr_registry {
    fr_source_plugin sources[FR_REGISTRY_CAPACITY];
    size_t source_count;
    fr_language_plugin languages[FR_REGISTRY_CAPACITY];
    size_t language_count;
    fr_config_plugin configs[FR_REGISTRY_CAPACITY];
    size_t config_count;
    fr_toolchain_plugin toolchains[FR_REGISTRY_CAPACITY];
    size_t toolchain_count;
};

fr_registry *fr_registry_create(void) {
    return calloc(1, sizeof(fr_registry));
}

void fr_registry_destroy(fr_registry *registry) {
    free(registry);
}

int fr_registry_add_source(fr_registry *registry, const fr_source_plugin *plugin, fr_error *err) {
    for (size_t index = 0; index < registry->source_count; index++) {
        if (strcmp(registry->sources[index].capability, plugin->capability) == 0) {
            fr_error_set(err, "capability \"%s\" is already registered", plugin->capability);
            return FR_ERR;
        }
    }
    if (registry->source_count == FR_REGISTRY_CAPACITY) {
        fr_error_set(err, "cannot register \"%s\": registry is full", plugin->capability);
        return FR_ERR;
    }
    registry->sources[registry->source_count++] = *plugin;
    return FR_OK;
}

int fr_registry_add_language(fr_registry *registry, const fr_language_plugin *plugin, fr_error *err) {
    for (size_t index = 0; index < registry->language_count; index++) {
        if (strcmp(registry->languages[index].capability, plugin->capability) == 0) {
            fr_error_set(err, "capability \"%s\" is already registered", plugin->capability);
            return FR_ERR;
        }
    }
    if (registry->language_count == FR_REGISTRY_CAPACITY) {
        fr_error_set(err, "cannot register \"%s\": registry is full", plugin->capability);
        return FR_ERR;
    }
    registry->languages[registry->language_count++] = *plugin;
    return FR_OK;
}

const fr_source_plugin *fr_registry_source(const fr_registry *registry, const char *capability) {
    for (size_t index = 0; index < registry->source_count; index++) {
        if (strcmp(registry->sources[index].capability, capability) == 0) return &registry->sources[index];
    }
    return NULL;
}

const fr_language_plugin *fr_registry_language(const fr_registry *registry, const char *capability) {
    for (size_t index = 0; index < registry->language_count; index++) {
        if (strcmp(registry->languages[index].capability, capability) == 0) return &registry->languages[index];
    }
    return NULL;
}

int fr_registry_add_config(fr_registry *registry, const fr_config_plugin *plugin, fr_error *err) {
    for (size_t index = 0; index < registry->config_count; index++) {
        if (strcmp(registry->configs[index].capability, plugin->capability) == 0) {
            fr_error_set(err, "capability \"%s\" is already registered", plugin->capability);
            return FR_ERR;
        }
    }
    if (registry->config_count == FR_REGISTRY_CAPACITY) {
        fr_error_set(err, "cannot register \"%s\": registry is full", plugin->capability);
        return FR_ERR;
    }
    registry->configs[registry->config_count++] = *plugin;
    return FR_OK;
}

const fr_config_plugin *fr_registry_config(const fr_registry *registry, const char *capability) {
    for (size_t index = 0; index < registry->config_count; index++) {
        if (strcmp(registry->configs[index].capability, capability) == 0) return &registry->configs[index];
    }
    return NULL;
}

size_t fr_registry_config_count(const fr_registry *registry) {
    return registry->config_count;
}

const fr_config_plugin *fr_registry_config_at(const fr_registry *registry, size_t index) {
    if (index >= registry->config_count) return NULL;
    return &registry->configs[index];
}

int fr_registry_add_toolchain(fr_registry *registry, const fr_toolchain_plugin *plugin, fr_error *err) {
    for (size_t index = 0; index < registry->toolchain_count; index++) {
        if (strcmp(registry->toolchains[index].capability, plugin->capability) == 0) {
            fr_error_set(err, "capability \"%s\" is already registered", plugin->capability);
            return FR_ERR;
        }
    }
    if (registry->toolchain_count == FR_REGISTRY_CAPACITY) {
        fr_error_set(err, "cannot register \"%s\": registry is full", plugin->capability);
        return FR_ERR;
    }
    registry->toolchains[registry->toolchain_count++] = *plugin;
    return FR_OK;
}

const fr_toolchain_plugin *fr_registry_toolchain(const fr_registry *registry, const char *capability) {
    for (size_t index = 0; index < registry->toolchain_count; index++) {
        if (strcmp(registry->toolchains[index].capability, capability) == 0) return &registry->toolchains[index];
    }
    return NULL;
}
