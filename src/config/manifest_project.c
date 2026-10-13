#include "config/manifest_internal.h"

#include "config/manifest.h"

#include "config/jsonx.h"
#include "util/error.h"
#include "util/semver.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static int check_schema(const cJSON *root, const char *file_path, fr_error *err) {
    const cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema");
    if (!cJSON_IsNumber(schema) || schema->valueint != FR_SCHEMA) {
        fr_error_set(err, "\"%s\" declares an unsupported schema, expected %d", file_path, FR_SCHEMA);
        return FR_ERR;
    }
    return FR_OK;
}

static int read_module(const cJSON *entry, const char *name, fr_module *out, fr_error *err) {
    memset(out, 0, sizeof *out);
    char path[256];
    snprintf(path, sizeof path, "modules.%s", name);

    out->name = fr_manifest_duplicate(name);
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

/* Every name the manifest vocabulary defines at the root, whether it is a key
   or a section. A name outside this list is refused rather than discarded,
   because a discarded one is a user believing they configured something:
   "[toolchain.java]" for "[toolchains.java]" used to sync clean, with no
   toolchain, no error and no warning. D-49. */
static const char *const MANIFEST_KEYS[] = {
    "schema", "project", "version", "modules", "sources", "consumers",
    "toolchains", "plugins", "publish", "resolvers", "tasks"
};

static int manifest_key_is_known(const char *name) {
    for (size_t index = 0; index < sizeof MANIFEST_KEYS / sizeof MANIFEST_KEYS[0]; index++) {
        if (strcmp(MANIFEST_KEYS[index], name) == 0) return 1;
    }
    return 0;
}

/* Answers only the mistake people actually make, a missing or surplus plural,
   rather than searching by edit distance: every section name here is plural,
   and the defect this was raised for is "toolchain" for "toolchains". A wrong
   suggestion costs more than none, so the rule stays one a reader can check. */
static const char *a_near_miss_for(const char *name) {
    size_t length = strlen(name);
    for (size_t index = 0; index < sizeof MANIFEST_KEYS / sizeof MANIFEST_KEYS[0]; index++) {
        const char *known = MANIFEST_KEYS[index];
        size_t known_length = strlen(known);
        if (known_length == length + 1 && known[known_length - 1] == 's'
            && strncmp(known, name, length) == 0) {
            return known;
        }
        if (length == known_length + 1 && name[length - 1] == 's'
            && strncmp(known, name, known_length) == 0) {
            return known;
        }
    }
    return NULL;
}

static int refuse_an_unknown_manifest_key(const cJSON *root, const char *file_path,
                                          fr_error *err) {
    for (const cJSON *member = root->child; member != NULL; member = member->next) {
        if (member->string == NULL || manifest_key_is_known(member->string)) continue;
        const char *near = a_near_miss_for(member->string);
        if (near != NULL) {
            fr_error_set(err, "\"%s\": \"%s\" is not a section daukle defines; did you mean"
                              " \"%s\"?", file_path, member->string, near);
        } else {
            fr_error_set(err, "\"%s\": \"%s\" is not a key or section daukle defines. It holds"
                              " schema, project, version, modules, sources, consumers,"
                              " toolchains, plugins, publish, resolvers and tasks",
                         file_path, member->string);
        }
        return FR_ERR;
    }
    return FR_OK;
}

int fr_manifest_project_from_json(const cJSON *root, const char *file_path, fr_project *out, fr_error *err) {
    memset(out, 0, sizeof *out);
    if (check_schema(root, file_path, err) != FR_OK) return FR_ERR;
    if (refuse_an_unknown_manifest_key(root, file_path, err) != FR_OK) return FR_ERR;

    const char *project_name = NULL;
    if (fr_json_string(root, "project", file_path, &project_name, err) != FR_OK) return FR_ERR;
    out->project = fr_manifest_duplicate(project_name);
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
