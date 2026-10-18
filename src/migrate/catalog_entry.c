#include "migrate/catalog_internal.h"

#include "plugin/plugins.h"
#include "util/error.h"

#include <stdlib.h>
#include <string.h>

#define FR_CATALOG_ID_MAX 64

static int is_lower_alnum(char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

static int is_valid_id(const char *id) {
    size_t length = strlen(id);
    if (length == 0 || length > FR_CATALOG_ID_MAX || !is_lower_alnum(id[0])) return 0;
    for (size_t index = 1; index < length; index++) {
        if (!is_lower_alnum(id[index]) && id[index] != '_' && id[index] != '-') return 0;
    }
    return 1;
}

static int refuse(fr_error *reason, const char *text) {
    fr_error_set(reason, "%s", text);
    return FR_ENTRY_REFUSED;
}

static int is_unsafe_file(const char *file) {
    if (file[0] == '/' || strstr(file, "..") != NULL || strchr(file, '\\') != NULL) return 1;
    return strchr(file, '/') != NULL && strpbrk(file, "*?") != NULL;
}

static int check_files(char *const *files, size_t count, fr_error *reason) {
    for (size_t index = 0; index < count; index++) {
        if (files[index][0] == '\0') return refuse(reason, "has an empty file name");
        if (is_unsafe_file(files[index])) {
            fr_error_set(reason, "names the file \"%s\", which is absolute, climbs out, uses a "
                         "backslash or holds a wildcard inside a path", files[index]);
            return FR_ENTRY_REFUSED;
        }
    }
    return FR_ENTRY_OK;
}

static int check_patterns(char *const *patterns, size_t count, fr_error *reason) {
    for (size_t index = 0; index < count; index++) {
        if (fr_catalog_pattern_has_bare_hyphen(patterns[index])) {
            fr_error_set(reason, "holds the pattern \"%s\" with a bare hyphen; write %%- for a "
                         "literal hyphen", patterns[index]);
            return FR_ENTRY_REFUSED;
        }
    }
    return FR_ENTRY_OK;
}

static int copy_strings(const cJSON *array, char **destination, size_t maximum, size_t *count,
                        fr_error *reason) {
    if ((size_t) cJSON_GetArraySize(array) > maximum) return refuse(reason, "has too many items in a detect list");
    const cJSON *item;
    cJSON_ArrayForEach(item, array) {
        if (!cJSON_IsString(item)) return refuse(reason, "has a non-string item in a detect list");
        destination[*count] = fr_dup_string(item->valuestring);
        if (destination[*count] == NULL) return FR_ENTRY_NO_MEMORY;
        (*count)++;
    }
    return FR_ENTRY_OK;
}

static void release_rule(fr_detect_rule *rule) {
    for (size_t index = 0; index < rule->file_count; index++) free(rule->files[index]);
    for (size_t index = 0; index < rule->contains_count; index++) free(rule->contains[index]);
}

static int read_scope(const cJSON *rule, fr_detect_scope *scope, fr_error *reason) {
    const cJSON *member = cJSON_GetObjectItemCaseSensitive(rule, "scope");
    *scope = FR_DETECT_ROOT;
    if (member == NULL) return FR_ENTRY_OK;
    if (cJSON_IsString(member) && strcmp(member->valuestring, "root") == 0) return FR_ENTRY_OK;
    if (cJSON_IsString(member) && strcmp(member->valuestring, "tree") == 0) {
        *scope = FR_DETECT_TREE;
        return FR_ENTRY_OK;
    }
    return refuse(reason, "has a detect scope that is neither root nor tree");
}

static int read_contains(const cJSON *contains, fr_detect_rule *rule, fr_error *reason) {
    if (contains == NULL) return FR_ENTRY_OK;
    if (!cJSON_IsArray(contains)) return refuse(reason, "has a \"contains\" that is not an array");
    int status = copy_strings(contains, rule->contains, FR_CATALOG_MAX_PATTERNS, &rule->contains_count, reason);
    if (status != FR_ENTRY_OK) return status;
    return check_patterns(rule->contains, rule->contains_count, reason);
}

static int read_rule_fields(const cJSON *json, fr_detect_rule *rule, fr_error *reason) {
    const cJSON *files = cJSON_GetObjectItemCaseSensitive(json, "files");
    if (!cJSON_IsArray(files) || cJSON_GetArraySize(files) == 0) {
        return refuse(reason, "has a detect rule without a non-empty \"files\" array");
    }
    int status = copy_strings(files, rule->files, FR_CATALOG_MAX_FILES, &rule->file_count, reason);
    if (status == FR_ENTRY_OK) status = check_files(rule->files, rule->file_count, reason);
    if (status == FR_ENTRY_OK) status = read_contains(cJSON_GetObjectItemCaseSensitive(json, "contains"), rule, reason);
    if (status == FR_ENTRY_OK) status = read_scope(json, &rule->scope, reason);
    return status;
}

static int read_rule(const cJSON *json, fr_detect_rule *rule, fr_error *reason) {
    memset(rule, 0, sizeof *rule);
    if (!cJSON_IsObject(json)) return refuse(reason, "has a detect rule that is not an object");
    int status = read_rule_fields(json, rule, reason);
    if (status != FR_ENTRY_OK) release_rule(rule);
    return status;
}

static int read_rules(const cJSON *entry, fr_catalog_plugin *out, fr_error *reason) {
    const cJSON *detect = cJSON_GetObjectItemCaseSensitive(entry, "detect");
    if (detect == NULL) return FR_ENTRY_OK;
    if (!cJSON_IsArray(detect)) return refuse(reason, "has a \"detect\" that is not an array");
    size_t size = (size_t) cJSON_GetArraySize(detect);
    if (size == 0) return FR_ENTRY_OK;
    out->rules = calloc(size, sizeof *out->rules);
    if (out->rules == NULL) return FR_ENTRY_NO_MEMORY;
    const cJSON *json;
    cJSON_ArrayForEach(json, detect) {
        int status = read_rule(json, &out->rules[out->rule_count], reason);
        if (status != FR_ENTRY_OK) return status;
        out->rule_count++;
    }
    return FR_ENTRY_OK;
}

static int read_asset(const cJSON *entry, fr_catalog_plugin *out, fr_error *reason) {
    const cJSON *asset = cJSON_GetObjectItemCaseSensitive(entry, "asset");
    if (asset == NULL || cJSON_IsNull(asset)) return FR_ENTRY_OK;
    const cJSON *url = cJSON_GetObjectItemCaseSensitive(asset, "url");
    const cJSON *digest = cJSON_GetObjectItemCaseSensitive(asset, "sha256");
    if (!cJSON_IsObject(asset) || !cJSON_IsString(url) || !cJSON_IsString(digest)) {
        return refuse(reason, "has an \"asset\" that is neither null nor {url, sha256}");
    }
    out->asset_url = fr_dup_string(url->valuestring);
    out->asset_sha256 = fr_dup_string(digest->valuestring);
    return out->asset_url != NULL && out->asset_sha256 != NULL ? FR_ENTRY_OK : FR_ENTRY_NO_MEMORY;
}

static int is_string_array(const cJSON *array) {
    if (!cJSON_IsArray(array)) return 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, array) {
        if (!cJSON_IsString(item)) return 0;
    }
    return 1;
}

static int is_replaces_shape(const cJSON *replaces) {
    if (!cJSON_IsObject(replaces)) return 0;
    for (const cJSON *tool = replaces->child; tool != NULL; tool = tool->next) {
        if (!cJSON_IsObject(tool)) return 0;
        for (const cJSON *kind = tool->child; kind != NULL; kind = kind->next) {
            if (!is_string_array(kind)) return 0;
        }
    }
    return 1;
}

static int read_replaces(const cJSON *entry, fr_catalog_plugin *out, fr_error *reason) {
    const cJSON *replaces = cJSON_GetObjectItemCaseSensitive(entry, "replaces");
    if (replaces == NULL || cJSON_IsNull(replaces)) return FR_ENTRY_OK;
    if (!is_replaces_shape(replaces)) {
        return refuse(reason, "has a \"replaces\" that is not tool -> kind -> array of strings");
    }
    out->replaces = cJSON_Duplicate(replaces, 1);
    return out->replaces != NULL ? FR_ENTRY_OK : FR_ENTRY_NO_MEMORY;
}

static int read_fields(const cJSON *entry, fr_catalog_plugin *out, fr_error *reason) {
    int status = read_asset(entry, out, reason);
    if (status == FR_ENTRY_OK) status = read_rules(entry, out, reason);
    if (status == FR_ENTRY_OK) status = read_replaces(entry, out, reason);
    return status;
}

void fr_catalog_plugin_release(fr_catalog_plugin *plugin) {
    free(plugin->id);
    free(plugin->asset_url);
    free(plugin->asset_sha256);
    free(plugin->location);
    for (size_t index = 0; index < plugin->rule_count; index++) release_rule(&plugin->rules[index]);
    free(plugin->rules);
    cJSON_Delete(plugin->replaces);
    memset(plugin, 0, sizeof *plugin);
}

int fr_catalog_read_entry(const cJSON *entry, fr_catalog_plugin *out, fr_error *reason,
                          const char **id_seen) {
    const cJSON *id = cJSON_IsObject(entry) ? cJSON_GetObjectItemCaseSensitive(entry, "id") : NULL;
    *id_seen = cJSON_IsString(id) ? id->valuestring : NULL;
    if (!cJSON_IsObject(entry)) return refuse(reason, "is not an object");
    if (*id_seen == NULL || !is_valid_id(*id_seen)) {
        return refuse(reason, "has no usable id (1-64 characters of a-z, 0-9, _ and -, starting with a letter or digit)");
    }
    out->id = fr_dup_string(*id_seen);
    int status = out->id != NULL ? read_fields(entry, out, reason) : FR_ENTRY_NO_MEMORY;
    if (status != FR_ENTRY_OK) fr_catalog_plugin_release(out);
    return status;
}
