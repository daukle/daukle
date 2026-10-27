#include "migrate/result.h"

#include "config/jsonx.h"
#include "config/manifest.h"
#include "migrate/proof.h"
#include "util/error.h"

#include "cJSON.h"

#include <stdio.h>
#include <string.h>

#define DERIVED_DIR "build/daukle"
#define FR_MIGRATE_INSTANCE_PREFIX_SIZE 320

static int malformed(fr_error *err, const char *who, const char *field, const char *what) {
    fr_error_set(err, "%s returned a malformed result: %s %s", who, field, what);
    return FR_ERR;
}

static int is_string(const cJSON *value) {
    return cJSON_IsString(value) && value->valuestring != NULL;
}

static const cJSON *member(const cJSON *object, const char *key) {
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsNull(value) ? NULL : value;
}

/* fr_migrate_set_instance writes these from the detection, and it must be
   their only writer: a fragment's value would otherwise win for a root
   instance and lose for a subdirectory one. */
static int check_instance_keys(const cJSON *manifest, const char *who, fr_error *err) {
    static const char *const INSTANCE_KEYS[] = { "toolchain", "dir" };
    const cJSON *instance = NULL;
    cJSON_ArrayForEach(instance, cJSON_GetObjectItemCaseSensitive(manifest, "toolchains")) {
        if (!cJSON_IsObject(instance)) continue;
        for (size_t index = 0; index < sizeof INSTANCE_KEYS / sizeof *INSTANCE_KEYS; index++) {
            if (cJSON_GetObjectItemCaseSensitive(instance, INSTANCE_KEYS[index]) != NULL) {
                fr_error_set(err, "%s returned a malformed result: manifest sets toolchains.%s.%s,"
                                  " which core writes from the detection and a plugin may not",
                             who, instance->string, INSTANCE_KEYS[index]);
                return FR_ERR;
            }
        }
    }
    return FR_OK;
}

static int check_manifest(const cJSON *result, const char *who, fr_error *err) {
    const cJSON *manifest = member(result, "manifest");
    if (manifest == NULL) return FR_OK;
    if (!cJSON_IsObject(manifest)) return malformed(err, who, "manifest", "must be a table");
    static const char *const OWNED_BY_CORE[] = { "plugins", "schema" };
    for (size_t index = 0; index < sizeof OWNED_BY_CORE / sizeof *OWNED_BY_CORE; index++) {
        if (cJSON_GetObjectItemCaseSensitive(manifest, OWNED_BY_CORE[index]) != NULL) {
            fr_error_set(err, "%s returned a malformed result: manifest sets \"%s\", which core"
                              " owns and a plugin may not write", who, OWNED_BY_CORE[index]);
            return FR_ERR;
        }
    }
    return check_instance_keys(manifest, who, err);
}

static int check_foreign(const cJSON *result, const char *who, fr_error *err) {
    const cJSON *foreign = member(result, "foreign");
    if (foreign == NULL) return FR_OK;
    if (!fr_json_is_array_or_empty_table(foreign)) {
        return malformed(err, who, "foreign", "must be a list");
    }
    const cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, foreign) {
        const cJSON *config = member(entry, "config");
        if (!cJSON_IsObject(entry) || !is_string(cJSON_GetObjectItemCaseSensitive(entry, "kind"))
            || !is_string(cJSON_GetObjectItemCaseSensitive(entry, "id"))
            || (config != NULL && !cJSON_IsObject(config))) {
            return malformed(err, who, "foreign", "must hold { kind, id, config? } entries");
        }
    }
    return FR_OK;
}

static int check_blockers(const cJSON *result, const char *who, fr_blockers *blockers,
                          fr_error *err) {
    const cJSON *list = member(result, "blockers");
    if (list == NULL) return FR_OK;
    if (!fr_json_is_array_or_empty_table(list)) {
        return malformed(err, who, "blockers", "must be a list");
    }
    const cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, list) {
        const cJSON *construct = cJSON_GetObjectItemCaseSensitive(entry, "construct");
        const cJSON *where = cJSON_GetObjectItemCaseSensitive(entry, "where");
        const cJSON *why = cJSON_GetObjectItemCaseSensitive(entry, "why");
        const cJSON *surface = member(entry, "surface");
        if (!cJSON_IsObject(entry) || !is_string(construct) || !is_string(where) || !is_string(why)
            || (surface != NULL && !is_string(surface))) {
            return malformed(err, who, "blockers",
                             "must hold { construct, where, why, surface? } entries");
        }
        if (fr_blockers_add(blockers, who, construct->valuestring, where->valuestring,
                            why->valuestring, surface != NULL ? surface->valuestring : NULL,
                            err) != FR_OK) {
            return FR_ERR;
        }
    }
    return FR_OK;
}

static int fold_ascii(int character) {
    return character >= 'A' && character <= 'Z' ? character + ('a' - 'A') : character;
}

static int equals_ignoring_case(const char *text, const char *expected) {
    for (; *text != '\0' && *expected != '\0'; text++, expected++) {
        if (fold_ascii((unsigned char) *text) != fold_ascii((unsigned char) *expected)) return 0;
    }
    return *text == *expected;
}

static int starts_with_ignoring_case(const char *text, const char *prefix) {
    for (; *prefix != '\0'; text++, prefix++) {
        if (fold_ascii((unsigned char) *text) != fold_ascii((unsigned char) *prefix)) return 0;
    }
    return 1;
}

static int is_or_contains_derived_dir(const char *path) {
    size_t derived_size = strlen(DERIVED_DIR);
    if (strcmp(path, ".") == 0 || equals_ignoring_case(path, "build")) return 1;
    return starts_with_ignoring_case(path, DERIVED_DIR)
           && (path[derived_size] == '\0' || path[derived_size] == '/');
}

static int check_owned(const cJSON *result, const char *who, fr_error *err) {
    const cJSON *owned = member(result, "owned");
    if (owned == NULL) return FR_OK;
    if (!fr_json_is_array_or_empty_table(owned)) {
        return malformed(err, who, "owned", "must be a list of paths");
    }
    const cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, owned) {
        if (!is_string(entry)) return malformed(err, who, "owned", "must be a list of paths");
        const char *path = entry->valuestring;
        if (is_or_contains_derived_dir(path)) {
            fr_error_set(err, "%s returned a malformed result: owned path \"%s\" is \".\", an"
                              " ancestor of " DERIVED_DIR " (such as \"build\"), or inside it, and"
                              " owned paths are moved into " DERIVED_DIR "/migrate", who, path);
            return FR_ERR;
        }
        if (!fr_manifest_relative_path_is_safe(path) || equals_ignoring_case(path, "daukle.toml")
            || equals_ignoring_case(path, "daukle.lua")) {
            fr_error_set(err, "%s returned a malformed result: owned path \"%s\" must be a"
                              " relative path below the project that is not daukle.toml or"
                              " daukle.lua", who, path);
            return FR_ERR;
        }
    }
    return FR_OK;
}

static const char *single_instance_name(const cJSON *result) {
    const cJSON *manifest = member(result, "manifest");
    const cJSON *toolchains = manifest != NULL
        ? cJSON_GetObjectItemCaseSensitive(manifest, "toolchains") : NULL;
    if (!cJSON_IsObject(toolchains) || cJSON_GetArraySize(toolchains) != 1) return NULL;
    return toolchains->child->string;
}

static int is_inside_instance_directory(const char *path, const char *instance) {
    char prefix[FR_MIGRATE_INSTANCE_PREFIX_SIZE];
    int length = snprintf(prefix, sizeof prefix, "daukle/%s/", instance);
    return length > 0 && (size_t) length < sizeof prefix
           && strncmp(path, prefix, (size_t) length) == 0 && path[length] != '\0';
}

static int check_file_entry(const cJSON *entry, const char *instance, const char *who,
                            fr_error *err) {
    const cJSON *path = cJSON_IsObject(entry) ? cJSON_GetObjectItemCaseSensitive(entry, "path") : NULL;
    const cJSON *text = cJSON_IsObject(entry) ? cJSON_GetObjectItemCaseSensitive(entry, "text") : NULL;
    if (!is_string(path) || !is_string(text)) {
        return malformed(err, who, "files", "must hold { path, text } entries with both strings");
    }
    if (!fr_manifest_relative_path_is_safe(path->valuestring)
        || !is_inside_instance_directory(path->valuestring, instance)) {
        fr_error_set(err, "%s returned a malformed result: files path \"%s\" must be a relative"
                          " file path below daukle/%s/, with \"/\" separators and no \"..\"",
                     who, path->valuestring, instance);
        return FR_ERR;
    }
    return FR_OK;
}

static int check_files(const cJSON *result, const char *who, fr_error *err) {
    const cJSON *files = member(result, "files");
    if (files == NULL) return FR_OK;
    if (!fr_json_is_array_or_empty_table(files)) {
        return malformed(err, who, "files", "must be a list of { path, text } entries");
    }
    const char *instance = single_instance_name(result);
    if (instance == NULL) {
        return malformed(err, who, "files", "needs a manifest with exactly one toolchain, whose"
                                            " name is the directory the files belong to");
    }
    const cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, files) {
        if (check_file_entry(entry, instance, who, err) != FR_OK) return FR_ERR;
    }
    return FR_OK;
}

static int check_compare_entry(const cJSON *entry, const char *who, fr_error *err) {
    const cJSON *old_path = cJSON_GetObjectItemCaseSensitive(entry, "old");
    const cJSON *new_path = cJSON_GetObjectItemCaseSensitive(entry, "new");
    const cJSON *kind = cJSON_GetObjectItemCaseSensitive(entry, "as");
    if (!cJSON_IsObject(entry) || !is_string(old_path) || !is_string(new_path) || !is_string(kind)) {
        return malformed(err, who, "proof.compare", "must hold { old, new, as } entries");
    }
    fr_proof_kind parsed_kind;
    if (fr_proof_kind_parse(kind->valuestring, &parsed_kind) != FR_OK) {
        fr_error_set(err, "%s returned a malformed result: proof.compare \"as\" is \"%s\", and"
                          " must be \"bytes\" or \"archive\"", who, kind->valuestring);
        return FR_ERR;
    }
    if (!fr_manifest_relative_path_is_safe(old_path->valuestring)
        || !fr_manifest_relative_path_is_safe(new_path->valuestring)) {
        fr_error_set(err, "%s returned a malformed result: proof.compare paths \"%s\" and \"%s\""
                          " must be relative paths below the project", who, old_path->valuestring,
                     new_path->valuestring);
        return FR_ERR;
    }
    if (equals_ignoring_case(old_path->valuestring, new_path->valuestring)) {
        fr_error_set(err, "%s returned a malformed result: proof.compare old \"%s\" and new \"%s\""
                          " name the same file, so comparing them proves nothing", who,
                     old_path->valuestring, new_path->valuestring);
        return FR_ERR;
    }
    return FR_OK;
}

static int add_unprovable(fr_blockers *blockers, const char *who, const char *why, fr_error *err) {
    return fr_blockers_add(blockers, who, "proof", "proof.compare", why, NULL, err);
}

static int check_proof(const cJSON *result, const char *who, fr_blockers *blockers,
                       fr_error *err) {
    const cJSON *proof = member(result, "proof");
    if (proof == NULL) {
        return add_unprovable(blockers, who, "the migrator returned no proof, so the migration"
                                             " cannot be shown to build the same artifacts", err);
    }
    const cJSON *compare = cJSON_GetObjectItemCaseSensitive(proof, "compare");
    if (!cJSON_IsObject(proof) || !is_string(cJSON_GetObjectItemCaseSensitive(proof, "task"))
        || !fr_json_is_array_or_empty_table(compare)) {
        return malformed(err, who, "proof", "must be { task, compare } with compare a list");
    }
    if (cJSON_GetArraySize(compare) == 0) {
        return add_unprovable(blockers, who, "the migrator named no artifact to compare, so the"
                                             " migration cannot be shown to build the same"
                                             " artifacts", err);
    }
    const cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, compare) {
        if (check_compare_entry(entry, who, err) != FR_OK) return FR_ERR;
    }
    return FR_OK;
}

int fr_migrate_check_run_result(const cJSON *result, const char *label, fr_blockers *blockers,
                                fr_error *err) {
    char who[256];
    snprintf(who, sizeof who, "migrator %s", label);
    if (!cJSON_IsObject(result)) return malformed(err, who, "the result", "must be a table");
    if (check_manifest(result, who, err) != FR_OK) return FR_ERR;
    if (check_foreign(result, who, err) != FR_OK) return FR_ERR;
    if (check_blockers(result, who, blockers, err) != FR_OK) return FR_ERR;
    if (check_owned(result, who, err) != FR_OK) return FR_ERR;
    if (check_files(result, who, err) != FR_OK) return FR_ERR;
    return check_proof(result, who, blockers, err);
}

int fr_migrate_check_translation(const cJSON *result, const char *source, fr_blockers *blockers,
                                 fr_error *err) {
    if (!cJSON_IsObject(result)) return malformed(err, source, "the result", "must be a table");
    if (check_manifest(result, source, err) != FR_OK) return FR_ERR;
    return check_blockers(result, source, blockers, err);
}
