#include "resolve.h"

#include "error.h"
#include "manifest.h"
#include "semver.h"

#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    fr_resolved *items;
    size_t count;
    size_t capacity;
} fr_resolved_builder;

typedef struct {
    const char **items;
    size_t count;
    size_t capacity;
} fr_name_queue;

static char *duplicate(const char *text) {
    if (text == NULL) return NULL;
    size_t length = strlen(text) + 1;
    char *copy = malloc(length);
    if (copy != NULL) memcpy(copy, text, length);
    return copy;
}

static void format_range(const fr_range *range, char *buffer, size_t size) {
    const char *prefix = "";
    if (range->kind == FR_RANGE_CARET) prefix = "^";
    else if (range->kind == FR_RANGE_TILDE) prefix = "~";
    snprintf(buffer, size, "%s%d.%d.%d", prefix, range->base.major, range->base.minor, range->base.patch);
}

static void format_version(const fr_version *version, char *buffer, size_t size) {
    snprintf(buffer, size, "%d.%d.%d", version->major, version->minor, version->patch);
}

static int name_queue_contains(const fr_name_queue *queue, const char *name) {
    for (size_t index = 0; index < queue->count; index++) {
        if (strcmp(queue->items[index], name) == 0) return 1;
    }
    return 0;
}

static int name_queue_push(fr_name_queue *queue, const char *name, fr_error *err) {
    if (name_queue_contains(queue, name)) return FR_OK;
    if (queue->count == queue->capacity) {
        size_t capacity = queue->capacity == 0 ? 8 : queue->capacity * 2;
        const char **items = realloc(queue->items, capacity * sizeof *items);
        if (items == NULL) {
            fr_error_set(err, "out of memory queuing module \"%s\"", name);
            return FR_ERR;
        }
        queue->items = items;
        queue->capacity = capacity;
    }
    queue->items[queue->count++] = name;
    return FR_OK;
}

static int builder_contains(const fr_resolved_builder *builder, const char *project, const char *module) {
    for (size_t index = 0; index < builder->count; index++) {
        if (strcmp(builder->items[index].project, project) == 0
            && strcmp(builder->items[index].module, module) == 0) return 1;
    }
    return 0;
}

static int builder_push(fr_resolved_builder *builder, const fr_project *project,
                        const fr_module *module, const cJSON *block, fr_error *err) {
    if (builder_contains(builder, project->project, module->name)) return FR_OK;
    if (builder->count == builder->capacity) {
        size_t capacity = builder->capacity == 0 ? 8 : builder->capacity * 2;
        fr_resolved *items = realloc(builder->items, capacity * sizeof *items);
        if (items == NULL) {
            fr_error_set(err, "out of memory resolving module \"%s\"", module->name);
            return FR_ERR;
        }
        builder->items = items;
        builder->capacity = capacity;
    }

    fr_resolved entry;
    memset(&entry, 0, sizeof entry);
    entry.project = duplicate(project->project);
    entry.module = duplicate(module->name);
    entry.block = cJSON_Duplicate(block, 1);
    if (entry.project == NULL || entry.module == NULL || entry.block == NULL) {
        free(entry.project);
        free(entry.module);
        cJSON_Delete(entry.block);
        fr_error_set(err, "out of memory resolving module \"%s\"", module->name);
        return FR_ERR;
    }
    builder->items[builder->count++] = entry;
    return FR_OK;
}

static int resolve_dependency(const fr_dependency *dependency, const fr_manifest *manifest,
                              const char *manifest_dir, const fr_registry *registry,
                              const char *language, fr_resolved_builder *builder, fr_error *err) {
    const fr_source *entry = fr_manifest_source(manifest, dependency->project);
    if (entry == NULL) {
        fr_error_set(err, "sources has no entry for \"%s\"", dependency->project);
        return FR_ERR;
    }

    char capability[256];
    snprintf(capability, sizeof capability, "tiestone.source/%s", entry->kind);
    const fr_source_plugin *source = fr_registry_source(registry, capability);
    if (source == NULL) {
        fr_error_set(err, "no source plugin registered for \"%s\"", capability);
        return FR_ERR;
    }

    fr_project project;
    if (source->load(source->state, dependency->project, entry->block, manifest_dir, &project, err) != FR_OK) {
        return FR_ERR;
    }

    if (strcmp(project.project, dependency->project) != 0) {
        fr_error_set(err, "source for \"%s\" declares project \"%s\"", dependency->project, project.project);
        fr_project_free(&project);
        return FR_ERR;
    }

    if (!fr_range_satisfies(&dependency->range, &project.version)) {
        char range_text[64];
        char version_text[64];
        format_range(&dependency->range, range_text, sizeof range_text);
        format_version(&project.version, version_text, sizeof version_text);
        fr_error_set(err, "project \"%s\" requires %s but found %s",
                    dependency->project, range_text, version_text);
        fr_project_free(&project);
        return FR_ERR;
    }

    fr_name_queue queue;
    memset(&queue, 0, sizeof queue);
    for (size_t index = 0; index < dependency->module_count; index++) {
        if (name_queue_push(&queue, dependency->modules[index], err) != FR_OK) {
            free(queue.items);
            fr_project_free(&project);
            return FR_ERR;
        }
    }

    for (size_t head = 0; head < queue.count; head++) {
        const char *name = queue.items[head];
        const fr_module *module = fr_project_module(&project, name);
        if (module == NULL) {
            fr_error_set(err, "project \"%s\" has no module \"%s\"", dependency->project, name);
            free(queue.items);
            fr_project_free(&project);
            return FR_ERR;
        }

        const cJSON *block = cJSON_GetObjectItemCaseSensitive(module->blocks, language);
        if (!cJSON_IsObject(block)) {
            fr_error_set(err, "module \"%s\" of \"%s\" has no \"%s\" block",
                        name, dependency->project, language);
            free(queue.items);
            fr_project_free(&project);
            return FR_ERR;
        }

        if (builder_push(builder, &project, module, block, err) != FR_OK) {
            free(queue.items);
            fr_project_free(&project);
            return FR_ERR;
        }

        for (size_t index = 0; index < module->requires_count; index++) {
            if (name_queue_push(&queue, module->requires[index], err) != FR_OK) {
                free(queue.items);
                fr_project_free(&project);
                return FR_ERR;
            }
        }
    }

    free(queue.items);
    fr_project_free(&project);
    return FR_OK;
}

static int compare_resolved(const void *left, const void *right) {
    const fr_resolved *a = left;
    const fr_resolved *b = right;
    int project_cmp = strcmp(a->project, b->project);
    if (project_cmp != 0) return project_cmp;
    return strcmp(a->module, b->module);
}

int fr_resolve_consumer(const fr_consumer *consumer, const fr_manifest *manifest,
                        const char *manifest_dir, const fr_registry *registry,
                        fr_resolved **out, size_t *count, fr_error *err) {
    *out = NULL;
    *count = 0;

    fr_resolved_builder builder;
    memset(&builder, 0, sizeof builder);

    for (size_t index = 0; index < consumer->dependency_count; index++) {
        if (resolve_dependency(&consumer->dependencies[index], manifest, manifest_dir, registry,
                               consumer->language, &builder, err) != FR_OK) {
            fr_resolved_free(builder.items, builder.count);
            return FR_ERR;
        }
    }

    if (builder.count > 1) qsort(builder.items, builder.count, sizeof *builder.items, compare_resolved);

    *out = builder.items;
    *count = builder.count;
    return FR_OK;
}

void fr_resolved_free(fr_resolved *items, size_t count) {
    if (items == NULL) return;
    for (size_t index = 0; index < count; index++) {
        free(items[index].project);
        free(items[index].module);
        cJSON_Delete(items[index].block);
    }
    free(items);
}
