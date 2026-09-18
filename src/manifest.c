#include "manifest.h"

#include "error.h"
#include "jsonx.h"
#include "region.h"
#include "semver.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define FR_SCHEMA 1

static char *duplicate(const char *text) {
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

static int check_schema(const cJSON *root, const char *file_path, fr_error *err) {
    const cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema");
    if (!cJSON_IsNumber(schema) || schema->valueint != FR_SCHEMA) {
        fr_error_set(err, "\"%s\" declares an unsupported schema, expected %d", file_path, FR_SCHEMA);
        return FR_ERR;
    }
    return FR_OK;
}

/* A module entry carries its language blocks beside the keys the core reads
   from that same entry, so a language may not be named after one: the lookup
   would find the core's own value instead of a language block. */
static const char *RESERVED_MODULE_KEYS[] = { "requires" };

static int is_reserved_module_key(const char *name) {
    size_t count = sizeof RESERVED_MODULE_KEYS / sizeof RESERVED_MODULE_KEYS[0];
    for (size_t index = 0; index < count; index++) {
        if (strcmp(RESERVED_MODULE_KEYS[index], name) == 0) return 1;
    }
    return 0;
}

static int read_module(const cJSON *entry, const char *name, fr_module *out, fr_error *err) {
    memset(out, 0, sizeof *out);
    char path[256];
    snprintf(path, sizeof path, "modules.%s", name);

    out->name = duplicate(name);
    if (out->name == NULL) {
        fr_error_set(err, "out of memory reading %s", path);
        return FR_ERR;
    }
    if (fr_json_array_of_strings(entry, "requires", path, &out->requires, &out->requires_count, err) != FR_OK) {
        return FR_ERR;
    }
    out->blocks = entry;
    return FR_OK;
}

static int project_from_json(const cJSON *root, const char *file_path, fr_project *out, fr_error *err) {
    memset(out, 0, sizeof *out);
    if (check_schema(root, file_path, err) != FR_OK) return FR_ERR;

    const char *project_name = NULL;
    if (fr_json_string(root, "project", file_path, &project_name, err) != FR_OK) return FR_ERR;
    out->project = duplicate(project_name);
    if (out->project == NULL) {
        fr_error_set(err, "out of memory reading %s.project", file_path);
        return FR_ERR;
    }

    const char *version_text = NULL;
    if (fr_json_string(root, "version", file_path, &version_text, err) != FR_OK) return FR_ERR;
    if (fr_version_parse(version_text, &out->version, err) != FR_OK) return FR_ERR;

    const cJSON *modules_obj = NULL;
    if (fr_json_object(root, "modules", file_path, &modules_obj, err) != FR_OK) return FR_ERR;

    int size = cJSON_GetArraySize(modules_obj);
    if (size > 0) {
        out->modules = calloc((size_t) size, sizeof *out->modules);
        if (out->modules == NULL) {
            fr_error_set(err, "out of memory reading %s.modules", file_path);
            return FR_ERR;
        }
    }
    size_t index = 0;
    const cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, modules_obj) {
        out->module_count = index + 1;
        if (read_module(entry, entry->string, &out->modules[index], err) != FR_OK) return FR_ERR;
        index++;
    }
    return FR_OK;
}

static int read_source(const cJSON *entry, const char *project_name, fr_source *out, fr_error *err) {
    memset(out, 0, sizeof *out);
    char path[256];
    snprintf(path, sizeof path, "sources.%s", project_name);

    const char *kind = NULL;
    if (fr_json_string(entry, "kind", path, &kind, err) != FR_OK) return FR_ERR;

    out->project = duplicate(project_name);
    out->kind = duplicate(kind);
    if (out->project == NULL || out->kind == NULL) {
        fr_error_set(err, "out of memory reading %s", path);
        return FR_ERR;
    }
    out->block = entry;
    return FR_OK;
}

static int read_dependency(const cJSON *entry, const char *project_name, const char *consumer_path,
                           fr_dependency *out, fr_error *err) {
    memset(out, 0, sizeof *out);
    char path[320];
    snprintf(path, sizeof path, "%s.dependencies.%s", consumer_path, project_name);

    out->project = duplicate(project_name);
    if (out->project == NULL) {
        fr_error_set(err, "out of memory reading %s", path);
        return FR_ERR;
    }
    const char *version_text = NULL;
    if (fr_json_string(entry, "version", path, &version_text, err) != FR_OK) return FR_ERR;
    if (fr_range_parse(version_text, &out->range, err) != FR_OK) return FR_ERR;
    if (fr_json_array_of_strings(entry, "modules", path, &out->modules, &out->module_count, err) != FR_OK) {
        return FR_ERR;
    }
    return FR_OK;
}

static int read_consumer(const cJSON *entry, size_t consumer_index, fr_consumer *out, fr_error *err) {
    memset(out, 0, sizeof *out);
    char path[64];
    snprintf(path, sizeof path, "consumers[%zu]", consumer_index);

    const char *id = NULL;
    if (fr_json_string(entry, "id", path, &id, err) != FR_OK) return FR_ERR;
    out->id = duplicate(id);
    if (out->id == NULL) {
        fr_error_set(err, "out of memory reading %s.id", path);
        return FR_ERR;
    }

    const char *language = NULL;
    if (fr_json_string(entry, "language", path, &language, err) != FR_OK) return FR_ERR;
    if (is_reserved_module_key(language)) {
        fr_error_set(err, "%s.language \"%s\" is a reserved module key", path, language);
        return FR_ERR;
    }
    out->language = duplicate(language);
    if (out->language == NULL) {
        fr_error_set(err, "out of memory reading %s.language", path);
        return FR_ERR;
    }

    const char *file_name = NULL;
    if (fr_json_string(entry, "file", path, &file_name, err) != FR_OK) return FR_ERR;
    out->file = duplicate(file_name);
    if (out->file == NULL) {
        fr_error_set(err, "out of memory reading %s.file", path);
        return FR_ERR;
    }

    const char *configuration = NULL;
    if (fr_json_string(entry, "configuration", path, &configuration, err) != FR_OK) return FR_ERR;
    out->configuration = duplicate(configuration);
    if (out->configuration == NULL) {
        fr_error_set(err, "out of memory reading %s.configuration", path);
        return FR_ERR;
    }

    const cJSON *dependencies_obj = NULL;
    if (fr_json_object(entry, "dependencies", path, &dependencies_obj, err) != FR_OK) return FR_ERR;

    int size = cJSON_GetArraySize(dependencies_obj);
    if (size > 0) {
        out->dependencies = calloc((size_t) size, sizeof *out->dependencies);
        if (out->dependencies == NULL) {
            fr_error_set(err, "out of memory reading %s.dependencies", path);
            return FR_ERR;
        }
    }
    size_t index = 0;
    const cJSON *dependency_entry = NULL;
    cJSON_ArrayForEach(dependency_entry, dependencies_obj) {
        out->dependency_count = index + 1;
        if (read_dependency(dependency_entry, dependency_entry->string, path, &out->dependencies[index], err) != FR_OK) {
            return FR_ERR;
        }
        index++;
    }
    return FR_OK;
}

static int manifest_from_json(const cJSON *root, const char *file_path, fr_manifest *out, fr_error *err) {
    if (project_from_json(root, file_path, &out->self, err) != FR_OK) return FR_ERR;

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
            if (read_source(entry, entry->string, &out->sources[index], err) != FR_OK) return FR_ERR;
            index++;
        }
    }

    const cJSON *consumers_arr = cJSON_GetObjectItemCaseSensitive(root, "consumers");
    if (consumers_arr != NULL) {
        if (!cJSON_IsArray(consumers_arr)) {
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
            if (read_consumer(entry, index, &out->consumers[index], err) != FR_OK) return FR_ERR;
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
    if (project_from_json(root, origin, out, err) != FR_OK) {
        fr_project_free(out);
        cJSON_Delete(root);
        return FR_ERR;
    }
    out->document = root;
    return FR_OK;
}

/* Initialised up front, not only by fr_project_parse further down, so that
   *out is safe to inspect and to free however early this fails. */
int fr_project_read(const char *file_path, fr_project *out, fr_error *err) {
    memset(out, 0, sizeof *out);
    char *text = NULL;
    if (fr_file_read_text(file_path, &text, err) != FR_OK) return FR_ERR;
    int result = fr_project_parse(text, file_path, out, err);
    free(text);
    return result;
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

int fr_manifest_read(const char *file_path, fr_manifest *out, fr_error *err) {
    memset(out, 0, sizeof *out);
    cJSON *root = NULL;
    if (fr_json_read_file(file_path, &root, err) != FR_OK) return FR_ERR;
    return fr_manifest_from_document(root, file_path, out, err);
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
    manifest->sources = NULL;
    manifest->source_count = 0;
    manifest->consumers = NULL;
    manifest->consumer_count = 0;
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
