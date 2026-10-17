#include "migrate/result.h"

#include "config/jsonx.h"
#include "config/manifest.h"
#include "util/error.h"

#include "cJSON.h"

#include <stdio.h>
#include <string.h>

#define DERIVED_DIR "build/daukle"

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
    return FR_OK;
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

static int is_or_contains_derived_dir(const char *path) {
    size_t derived_size = strlen(DERIVED_DIR);
    if (strcmp(path, ".") == 0 || strcmp(path, "build") == 0) return 1;
    return strncmp(path, DERIVED_DIR, derived_size) == 0
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
        if (!fr_manifest_relative_path_is_safe(path) || strcmp(path, "daukle.toml") == 0
            || strcmp(path, "daukle.lua") == 0) {
            fr_error_set(err, "%s returned a malformed result: owned path \"%s\" must be a"
                              " relative path below the project that is not daukle.toml or"
                              " daukle.lua", who, path);
            return FR_ERR;
        }
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
    if (strcmp(kind->valuestring, "bytes") != 0 && strcmp(kind->valuestring, "archive") != 0) {
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
    return check_proof(result, who, blockers, err);
}

int fr_migrate_check_translation(const cJSON *result, const char *source, fr_blockers *blockers,
                                 fr_error *err) {
    if (!cJSON_IsObject(result)) return malformed(err, source, "the result", "must be a table");
    if (check_manifest(result, source, err) != FR_OK) return FR_ERR;
    return check_blockers(result, source, blockers, err);
}
