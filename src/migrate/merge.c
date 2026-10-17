#include "migrate/merge.h"

#include "util/error.h"

#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KEY_PATH_MAX 512

typedef struct {
    cJSON *owner_of;
    const char *source;
    fr_blockers *blockers;
    fr_error *err;
} merge_context;

static int out_of_memory(fr_error *err, const char *doing) {
    fr_error_set(err, "out of memory %s", doing);
    return FR_ERR;
}

static int child_path(const char *prefix, const char *key, char *out) {
    int written = prefix[0] == '\0' ? snprintf(out, KEY_PATH_MAX, "%s", key)
                                    : snprintf(out, KEY_PATH_MAX, "%s.%s", prefix, key);
    return written > 0 && written < KEY_PATH_MAX;
}

static int record_owner(const merge_context *context, const char *path, const cJSON *value) {
    if (cJSON_GetObjectItemCaseSensitive(context->owner_of, path) == NULL
        && !cJSON_AddStringToObject(context->owner_of, path, context->source)) {
        return out_of_memory(context->err, "recording who set a manifest key");
    }
    if (!cJSON_IsObject(value)) return FR_OK;
    const cJSON *child = NULL;
    cJSON_ArrayForEach(child, value) {
        char nested[KEY_PATH_MAX];
        if (!child_path(path, child->string, nested)) {
            fr_error_set(context->err, "a manifest key path under \"%s\" is too long to merge", path);
            return FR_ERR;
        }
        if (record_owner(context, nested, child) != FR_OK) return FR_ERR;
    }
    return FR_OK;
}

static int report_collision(const merge_context *context, const char *path, const cJSON *existing,
                            const cJSON *incoming) {
    const cJSON *owner = cJSON_GetObjectItemCaseSensitive(context->owner_of, path);
    const char *first = cJSON_IsString(owner) ? owner->valuestring : "an earlier source";
    char *first_value = cJSON_PrintUnformatted(existing);
    char *second_value = cJSON_PrintUnformatted(incoming);
    if (first_value == NULL || second_value == NULL) {
        free(first_value);
        free(second_value);
        return out_of_memory(context->err, "describing a manifest key collision");
    }
    size_t size = strlen(first) + strlen(first_value) + strlen(context->source)
                  + strlen(second_value) + 32;
    char *why = malloc(size);
    int status = FR_ERR;
    if (why == NULL) {
        out_of_memory(context->err, "describing a manifest key collision");
    } else {
        snprintf(why, size, "%s sets it to %s and %s to %s", first, first_value, context->source,
                 second_value);
        status = fr_blockers_add(context->blockers, "core", "manifest key", path, why, NULL,
                                 context->err);
    }
    free(why);
    free(first_value);
    free(second_value);
    return status;
}

static int merge_value(const merge_context *context, cJSON *into, const cJSON *fragment,
                       const char *prefix);

static int merge_entry(const merge_context *context, cJSON *into, const cJSON *entry,
                       const char *prefix) {
    char path[KEY_PATH_MAX];
    if (!child_path(prefix, entry->string, path)) {
        fr_error_set(context->err, "a manifest key path under \"%s\" is too long to merge", prefix);
        return FR_ERR;
    }
    cJSON *existing = cJSON_GetObjectItemCaseSensitive(into, entry->string);
    if (existing == NULL) {
        cJSON *copy = cJSON_Duplicate(entry, 1);
        if (copy == NULL) return out_of_memory(context->err, "copying a manifest fragment");
        cJSON_AddItemToObject(into, entry->string, copy);
        return record_owner(context, path, entry);
    }
    if (cJSON_IsObject(existing) && cJSON_IsObject(entry)) {
        return merge_value(context, existing, entry, path);
    }
    if (cJSON_Compare(existing, entry, 1)) return FR_OK;
    return report_collision(context, path, existing, entry);
}

static int merge_value(const merge_context *context, cJSON *into, const cJSON *fragment,
                       const char *prefix) {
    const cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, fragment) {
        if (merge_entry(context, into, entry, prefix) != FR_OK) return FR_ERR;
    }
    return FR_OK;
}

int fr_migrate_merge(cJSON *into, const cJSON *fragment, const char *source, cJSON *owner_of,
                     fr_blockers *blockers, fr_error *err) {
    merge_context context = { owner_of, source, blockers, err };
    return merge_value(&context, into, fragment, "");
}

static cJSON *ensure_object(cJSON *parent, const char *key, const char *what, fr_error *err) {
    cJSON *existing = cJSON_GetObjectItemCaseSensitive(parent, key);
    if (existing == NULL) {
        existing = cJSON_AddObjectToObject(parent, key);
        if (existing == NULL) out_of_memory(err, what);
        return existing;
    }
    if (!cJSON_IsObject(existing)) {
        fr_error_set(err, "%s: \"%s\" is not a table", what, key);
        return NULL;
    }
    return existing;
}

static int set_string(cJSON *object, const char *key, const char *value, fr_error *err) {
    cJSON_DeleteItemFromObjectCaseSensitive(object, key);
    if (!cJSON_AddStringToObject(object, key, value)) {
        return out_of_memory(err, "writing a manifest key");
    }
    return FR_OK;
}

int fr_migrate_set_instance(cJSON *manifest, const char *instance, const char *toolchain,
                            const char *dir, fr_error *err) {
    cJSON *toolchains = ensure_object(manifest, "toolchains", "writing toolchains", err);
    if (toolchains == NULL) return FR_ERR;
    cJSON *entry = ensure_object(toolchains, instance, "writing toolchains", err);
    if (entry == NULL) return FR_ERR;
    if (strcmp(toolchain, instance) != 0 && set_string(entry, "toolchain", toolchain, err) != FR_OK) {
        return FR_ERR;
    }
    if (dir != NULL && dir[0] != '\0') return set_string(entry, "dir", dir, err);
    return FR_OK;
}

int fr_migrate_fill_defaults(cJSON *manifest, const char *project_name, fr_error *err) {
    if (cJSON_GetObjectItemCaseSensitive(manifest, "schema") == NULL
        && !cJSON_AddNumberToObject(manifest, "schema", 1)) {
        return out_of_memory(err, "filling manifest defaults");
    }
    if (cJSON_GetObjectItemCaseSensitive(manifest, "project") == NULL
        && !cJSON_AddStringToObject(manifest, "project", project_name)) {
        return out_of_memory(err, "filling manifest defaults");
    }
    if (cJSON_GetObjectItemCaseSensitive(manifest, "version") == NULL
        && !cJSON_AddStringToObject(manifest, "version", "0.1.0")) {
        return out_of_memory(err, "filling manifest defaults");
    }
    if (cJSON_GetObjectItemCaseSensitive(manifest, "modules") == NULL
        && !cJSON_AddObjectToObject(manifest, "modules")) {
        return out_of_memory(err, "filling manifest defaults");
    }
    return FR_OK;
}

int fr_migrate_add_plugin(cJSON *manifest, const char *id, const char *url, const char *sha256,
                          fr_error *err) {
    cJSON *plugins = ensure_object(manifest, "plugins", "writing plugins", err);
    if (plugins == NULL) return FR_ERR;
    cJSON_DeleteItemFromObjectCaseSensitive(plugins, id);
    cJSON *entry = cJSON_AddObjectToObject(plugins, id);
    if (entry == NULL) return out_of_memory(err, "writing plugins");
    if (set_string(entry, "url", url, err) != FR_OK) return FR_ERR;
    return set_string(entry, "sha256", sha256, err);
}
