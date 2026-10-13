#include "config/manifest_internal.h"

#include "config/jsonx.h"
#include "util/error.h"
#include "util/semver.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

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

int fr_manifest_read_source(const cJSON *entry, const char *project_name, fr_source *out, fr_error *err) {
    memset(out, 0, sizeof *out);
    char path[256];
    snprintf(path, sizeof path, "sources.%s", project_name);

    const char *kind = NULL;
    if (fr_json_string(entry, "kind", path, &kind, err) != FR_OK) return FR_ERR;

    out->project = fr_manifest_duplicate(project_name);
    out->kind = fr_manifest_duplicate(kind);
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

    out->project = fr_manifest_duplicate(project_name);
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

int fr_manifest_read_consumer(const cJSON *entry, size_t consumer_index, fr_consumer *out, fr_error *err) {
    memset(out, 0, sizeof *out);
    char path[64];
    snprintf(path, sizeof path, "consumers[%zu]", consumer_index);

    const char *id = NULL;
    if (fr_json_string(entry, "id", path, &id, err) != FR_OK) return FR_ERR;
    out->id = fr_manifest_duplicate(id);
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
    out->language = fr_manifest_duplicate(language);
    if (out->language == NULL) {
        fr_error_set(err, "out of memory reading %s.language", path);
        return FR_ERR;
    }

    const char *file_name = NULL;
    if (fr_json_string(entry, "file", path, &file_name, err) != FR_OK) return FR_ERR;
    out->file = fr_manifest_duplicate(file_name);
    if (out->file == NULL) {
        fr_error_set(err, "out of memory reading %s.file", path);
        return FR_ERR;
    }

    const char *configuration = NULL;
    if (fr_json_string(entry, "configuration", path, &configuration, err) != FR_OK) return FR_ERR;
    out->configuration = fr_manifest_duplicate(configuration);
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

int fr_manifest_read_toolchain(const cJSON *entry, const char *name, fr_toolchain *out, fr_error *err) {
    memset(out, 0, sizeof *out);
    char path[320];
    snprintf(path, sizeof path, "toolchains.%s", name);

    if (strcmp(name, "publish") == 0) {
        fr_error_set(err, "%s may not be named \"publish\": that prefix is reserved for publish"
                          " destinations, so a \"publish:\" task would never reach this toolchain",
                     path);
        return FR_ERR;
    }

    out->name = fr_manifest_duplicate(name);
    if (out->name == NULL) {
        fr_error_set(err, "out of memory reading %s", path);
        return FR_ERR;
    }

    if (cJSON_IsString(entry)) {
        out->version = fr_manifest_duplicate(entry->valuestring);
        if (out->version == NULL) {
            fr_error_set(err, "out of memory reading %s", path);
            return FR_ERR;
        }
        return FR_OK;
    }
    if (!cJSON_IsObject(entry)) {
        fr_error_set(err, "%s must be a table or a version string", path);
        return FR_ERR;
    }
    out->block = entry;

    const cJSON *version = cJSON_GetObjectItemCaseSensitive(entry, "version");
    if (version != NULL) {
        if (!cJSON_IsString(version)) {
            fr_error_set(err, "%s.version must be a string", path);
            return FR_ERR;
        }
        out->version = fr_manifest_duplicate(version->valuestring);
        if (out->version == NULL) {
            fr_error_set(err, "out of memory reading %s.version", path);
            return FR_ERR;
        }
    }

    const cJSON *dependencies_obj = cJSON_GetObjectItemCaseSensitive(entry, "dependencies");
    if (dependencies_obj == NULL) return FR_OK;
    if (!cJSON_IsObject(dependencies_obj)) {
        fr_error_set(err, "%s.dependencies must be a table", path);
        return FR_ERR;
    }
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
        if (read_dependency(dependency_entry, dependency_entry->string, path,
                            &out->dependencies[index], err) != FR_OK) {
            return FR_ERR;
        }
        index++;
    }
    return FR_OK;
}

int fr_manifest_read_publish(const cJSON *entry, const char *name, fr_publish_target *out,
                             fr_error *err) {
    memset(out, 0, sizeof *out);
    char path[320];
    snprintf(path, sizeof path, "publish.%s", name);

    out->name = fr_manifest_duplicate(name);
    if (out->name == NULL) {
        fr_error_set(err, "out of memory reading %s", path);
        return FR_ERR;
    }
    if (!cJSON_IsObject(entry)) {
        fr_error_set(err, "%s must be a table", path);
        return FR_ERR;
    }
    out->block = entry;

    const cJSON *from = cJSON_GetObjectItemCaseSensitive(entry, "from");
    if (from == NULL || !cJSON_IsString(from)) {
        fr_error_set(err, "%s needs a \"from\" string naming the toolchain whose output it"
                          " publishes", path);
        return FR_ERR;
    }
    out->from = fr_manifest_duplicate(from->valuestring);
    if (out->from == NULL) {
        fr_error_set(err, "out of memory reading %s.from", path);
        return FR_ERR;
    }
    return FR_OK;
}

int fr_manifest_check_publish_targets(const fr_manifest *out, const char *file_path, fr_error *err) {
    for (size_t index = 0; index < out->publish_count; index++) {
        const fr_publish_target *target = &out->publishes[index];
        int declared = 0;
        for (size_t other = 0; other < out->toolchain_count; other++) {
            if (strcmp(out->toolchains[other].name, target->from) == 0) {
                declared = 1;
                break;
            }
        }
        if (!declared) {
            fr_error_set(err, "%s.publish.%s.from names \"%s\", but this manifest declares no"
                              " toolchain \"%s\"", file_path, target->name, target->from,
                              target->from);
            return FR_ERR;
        }
    }
    return FR_OK;
}
