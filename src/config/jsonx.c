#include "config/jsonx.h"

#include "util/error.h"

#include <stdlib.h>
#include <string.h>

static char *duplicate(const char *text) {
    if (text == NULL) return NULL;
    size_t length = strlen(text) + 1;
    char *copy = malloc(length);
    if (copy != NULL) memcpy(copy, text, length);
    return copy;
}

int fr_json_string(const cJSON *object, const char *key, const char *path,
                   const char **out, fr_error *err) {
    const cJSON *member = cJSON_GetObjectItemCaseSensitive(object, key);
    if (member == NULL) {
        fr_error_set(err, "%s is missing \"%s\"", path, key);
        return FR_ERR;
    }
    if (!cJSON_IsString(member) || member->valuestring == NULL) {
        fr_error_set(err, "%s.%s must be a string", path, key);
        return FR_ERR;
    }
    *out = member->valuestring;
    return FR_OK;
}

int fr_json_object(const cJSON *object, const char *key, const char *path,
                   const cJSON **out, fr_error *err) {
    const cJSON *member = cJSON_GetObjectItemCaseSensitive(object, key);
    if (member == NULL) {
        fr_error_set(err, "%s is missing \"%s\"", path, key);
        return FR_ERR;
    }
    if (!cJSON_IsObject(member)) {
        fr_error_set(err, "%s.%s must be an object", path, key);
        return FR_ERR;
    }
    *out = member;
    return FR_OK;
}

int fr_json_is_array_or_empty_table(const cJSON *value) {
    if (cJSON_IsArray(value)) return 1;
    return cJSON_IsObject(value) && value->child == NULL;
}

int fr_json_array_of_strings(const cJSON *object, const char *key, const char *path,
                             char ***out, size_t *count, fr_error *err) {
    *out = NULL;
    *count = 0;
    const cJSON *member = cJSON_GetObjectItemCaseSensitive(object, key);
    if (member == NULL) return FR_OK;
    if (!fr_json_is_array_or_empty_table(member)) {
        fr_error_set(err, "%s.%s must be an array", path, key);
        return FR_ERR;
    }
    int size = cJSON_GetArraySize(member);
    if (size == 0) return FR_OK;

    char **items = calloc((size_t) size, sizeof *items);
    if (items == NULL) {
        fr_error_set(err, "out of memory reading %s.%s", path, key);
        return FR_ERR;
    }
    for (int index = 0; index < size; index++) {
        const cJSON *entry = cJSON_GetArrayItem(member, index);
        if (!cJSON_IsString(entry) || entry->valuestring == NULL) {
            fr_string_array_free(items, (size_t) index);
            fr_error_set(err, "%s.%s[%d] must be a string", path, key, index);
            return FR_ERR;
        }
        items[index] = duplicate(entry->valuestring);
        if (items[index] == NULL) {
            fr_string_array_free(items, (size_t) index);
            fr_error_set(err, "out of memory reading %s.%s", path, key);
            return FR_ERR;
        }
    }
    *out = items;
    *count = (size_t) size;
    return FR_OK;
}

void fr_string_array_free(char **items, size_t count) {
    if (items == NULL) return;
    for (size_t index = 0; index < count; index++) free(items[index]);
    free(items);
}

static void report_parse_failure(const char *text, fr_error *err) {
    const char *position = cJSON_GetErrorPtr();
    if (position == NULL || position < text || position > text + strlen(text)) {
        fr_error_set(err, "not valid json");
        return;
    }
    fr_error_set(err, "not valid json at byte %zu, near \"%.30s\"",
                 (size_t) (position - text), position);
}

int fr_json_parse(const char *text, cJSON **out, fr_error *err) {
    cJSON *root = cJSON_Parse(text);
    if (root == NULL) {
        report_parse_failure(text, err);
        return FR_ERR;
    }
    *out = root;
    return FR_OK;
}
