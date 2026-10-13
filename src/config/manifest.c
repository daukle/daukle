#include "config/manifest.h"
#include "config/manifest_internal.h"

#include "config/jsonx.h"
#include "plugin/plugins.h"
#include "util/error.h"

#include <stdlib.h>
#include <string.h>

char *fr_manifest_duplicate(const char *text) {
    if (text == NULL) return NULL;
    size_t length = strlen(text) + 1;
    char *copy = malloc(length);
    if (copy != NULL) memcpy(copy, text, length);
    return copy;
}

static void module_free(fr_module *module) {
    if (module == NULL) return;
    free(module->name);
    fr_string_array_free(module->requires, module->requires_count);
}

static void source_free(fr_source *source) {
    if (source == NULL) return;
    free(source->project);
    free(source->kind);
}

static void dependency_free(fr_dependency *dependency) {
    if (dependency == NULL) return;
    free(dependency->project);
    fr_string_array_free(dependency->modules, dependency->module_count);
}

static void consumer_free(fr_consumer *consumer) {
    if (consumer == NULL) return;
    free(consumer->id);
    free(consumer->language);
    free(consumer->file);
    free(consumer->configuration);
    for (size_t index = 0; index < consumer->dependency_count; index++) {
        dependency_free(&consumer->dependencies[index]);
    }
    free(consumer->dependencies);
}

static int manifest_from_json(const cJSON *root, const char *file_path, fr_manifest *out, fr_error *err) {
    if (fr_manifest_project_from_json(root, file_path, &out->self, err) != FR_OK) return FR_ERR;

    const cJSON *sources_obj = cJSON_GetObjectItemCaseSensitive(root, "sources");
    if (sources_obj != NULL) {
        if (!cJSON_IsObject(sources_obj)) {
            fr_error_set(err, "%s.sources must be an object", file_path);
            return FR_ERR;
        }
        int size = cJSON_GetArraySize(sources_obj);
        if (size > 0) {
            out->sources = calloc((size_t) size, sizeof *out->sources);
            if (out->sources == NULL) {
                fr_error_set(err, "out of memory reading %s.sources", file_path);
                return FR_ERR;
            }
        }
        size_t index = 0;
        const cJSON *entry = NULL;
        cJSON_ArrayForEach(entry, sources_obj) {
            out->source_count = index + 1;
            if (fr_manifest_read_source(entry, entry->string, &out->sources[index], err) != FR_OK) return FR_ERR;
            index++;
        }
    }

    const cJSON *consumers_arr = cJSON_GetObjectItemCaseSensitive(root, "consumers");
    if (consumers_arr != NULL) {
        if (!fr_json_is_array_or_empty_table(consumers_arr)) {
            fr_error_set(err, "%s.consumers must be an array", file_path);
            return FR_ERR;
        }
        int size = cJSON_GetArraySize(consumers_arr);
        if (size > 0) {
            out->consumers = calloc((size_t) size, sizeof *out->consumers);
            if (out->consumers == NULL) {
                fr_error_set(err, "out of memory reading %s.consumers", file_path);
                return FR_ERR;
            }
        }
        size_t index = 0;
        const cJSON *entry = NULL;
        cJSON_ArrayForEach(entry, consumers_arr) {
            out->consumer_count = index + 1;
            if (fr_manifest_read_consumer(entry, index, &out->consumers[index], err) != FR_OK) return FR_ERR;
            index++;
        }
    }

    const cJSON *toolchains_obj = cJSON_GetObjectItemCaseSensitive(root, "toolchains");
    if (toolchains_obj != NULL) {
        if (!cJSON_IsObject(toolchains_obj)) {
            fr_error_set(err, "%s.toolchains must be a table", file_path);
            return FR_ERR;
        }
        int size = cJSON_GetArraySize(toolchains_obj);
        if (size > 0) {
            out->toolchains = calloc((size_t) size, sizeof *out->toolchains);
            if (out->toolchains == NULL) {
                fr_error_set(err, "out of memory reading %s.toolchains", file_path);
                return FR_ERR;
            }
        }
        size_t index = 0;
        const cJSON *entry = NULL;
        cJSON_ArrayForEach(entry, toolchains_obj) {
            out->toolchain_count = index + 1;
            if (fr_manifest_read_toolchain(entry, entry->string, &out->toolchains[index], err) != FR_OK) {
                return FR_ERR;
            }
            index++;
        }
    }

    const cJSON *publish_obj = cJSON_GetObjectItemCaseSensitive(root, "publish");
    if (publish_obj != NULL) {
        if (!cJSON_IsObject(publish_obj)) {
            fr_error_set(err, "%s.publish must be a table", file_path);
            return FR_ERR;
        }
        int size = cJSON_GetArraySize(publish_obj);
        if (size > 0) {
            out->publishes = calloc((size_t) size, sizeof *out->publishes);
            if (out->publishes == NULL) {
                fr_error_set(err, "out of memory reading %s.publish", file_path);
                return FR_ERR;
            }
        }
        size_t index = 0;
        const cJSON *entry = NULL;
        cJSON_ArrayForEach(entry, publish_obj) {
            out->publish_count = index + 1;
            if (fr_manifest_read_publish(entry, entry->string, &out->publishes[index], err) != FR_OK) {
                return FR_ERR;
            }
            index++;
        }
        if (fr_manifest_check_publish_targets(out, file_path, err) != FR_OK) return FR_ERR;
    }

    const cJSON *tasks_obj = cJSON_GetObjectItemCaseSensitive(root, "tasks");
    if (tasks_obj != NULL) {
        if (!cJSON_IsObject(tasks_obj)) {
            fr_error_set(err, "%s.tasks must be a table", file_path);
            return FR_ERR;
        }
        int size = cJSON_GetArraySize(tasks_obj);
        if (size > 0) {
            out->tasks = calloc((size_t) size, sizeof *out->tasks);
            if (out->tasks == NULL) {
                fr_error_set(err, "out of memory reading %s.tasks", file_path);
                return FR_ERR;
            }
        }
        size_t index = 0;
        const cJSON *entry = NULL;
        cJSON_ArrayForEach(entry, tasks_obj) {
            out->task_count = index + 1;
            if (fr_manifest_read_task(entry, entry->string, &out->tasks[index], err) != FR_OK) return FR_ERR;
            index++;
        }
    }

    return FR_OK;
}

int fr_project_parse(const char *text, const char *origin, fr_project *out, fr_error *err) {
    memset(out, 0, sizeof *out);
    cJSON *root = cJSON_Parse(text);
    if (root == NULL) {
        fr_error_set(err, "\"%s\" is not valid json", origin);
        return FR_ERR;
    }

    const char *project_name = NULL;
    if (fr_json_string(root, "project", origin, &project_name, err) != FR_OK) {
        cJSON_Delete(root);
        return FR_ERR;
    }
    if (fr_plugins_reject_in_fetched(root, project_name, err) != FR_OK) {
        cJSON_Delete(root);
        return FR_ERR;
    }

    if (fr_manifest_project_from_json(root, origin, out, err) != FR_OK) {
        fr_project_free(out);
        cJSON_Delete(root);
        return FR_ERR;
    }
    out->document = root;
    return FR_OK;
}

int fr_manifest_from_document(cJSON *root, const char *origin, fr_manifest *out, fr_error *err) {
    memset(out, 0, sizeof *out);
    if (manifest_from_json(root, origin, out, err) != FR_OK) {
        fr_manifest_free(out);
        cJSON_Delete(root);
        return FR_ERR;
    }
    out->document = root;
    return FR_OK;
}

void fr_project_free(fr_project *project) {
    if (project == NULL) return;
    for (size_t index = 0; index < project->module_count; index++) {
        module_free(&project->modules[index]);
    }
    free(project->modules);
    free(project->project);
    cJSON_Delete(project->document);
    project->modules = NULL;
    project->module_count = 0;
    project->project = NULL;
    project->document = NULL;
}

void fr_manifest_free(fr_manifest *manifest) {
    if (manifest == NULL) return;
    fr_project_free(&manifest->self);
    for (size_t index = 0; index < manifest->source_count; index++) {
        source_free(&manifest->sources[index]);
    }
    free(manifest->sources);
    for (size_t index = 0; index < manifest->consumer_count; index++) {
        consumer_free(&manifest->consumers[index]);
    }
    free(manifest->consumers);
    for (size_t index = 0; index < manifest->toolchain_count; index++) {
        fr_toolchain *toolchain = &manifest->toolchains[index];
        free(toolchain->name);
        free(toolchain->version);
        for (size_t d = 0; d < toolchain->dependency_count; d++) {
            dependency_free(&toolchain->dependencies[d]);
        }
        free(toolchain->dependencies);
    }
    free(manifest->toolchains);
    for (size_t index = 0; index < manifest->publish_count; index++) {
        free(manifest->publishes[index].name);
        free(manifest->publishes[index].from);
    }
    free(manifest->publishes);
    for (size_t index = 0; index < manifest->task_count; index++) {
        fr_task *task = &manifest->tasks[index];
        free(task->name);
        free(task->part_of);
        for (size_t d = 0; d < task->depends_on_count; d++) free(task->depends_on[d]);
        free(task->depends_on);
        fr_manifest_task_command_free(task->run);
    }
    free(manifest->tasks);
    manifest->sources = NULL;
    manifest->source_count = 0;
    manifest->consumers = NULL;
    manifest->consumer_count = 0;
    manifest->toolchains = NULL;
    manifest->toolchain_count = 0;
    manifest->publishes = NULL;
    manifest->publish_count = 0;
    manifest->tasks = NULL;
    manifest->task_count = 0;
    cJSON_Delete(manifest->document);
    manifest->document = NULL;
}

const fr_module *fr_project_module(const fr_project *project, const char *name) {
    if (project == NULL || name == NULL) return NULL;
    for (size_t index = 0; index < project->module_count; index++) {
        if (strcmp(project->modules[index].name, name) == 0) return &project->modules[index];
    }
    return NULL;
}

const fr_source *fr_manifest_source(const fr_manifest *manifest, const char *project) {
    if (manifest == NULL || project == NULL) return NULL;
    for (size_t index = 0; index < manifest->source_count; index++) {
        if (strcmp(manifest->sources[index].project, project) == 0) return &manifest->sources[index];
    }
    return NULL;
}
