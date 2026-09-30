#include "config/jsonx.h"

#include "util/error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *duplicate(const char *text) {
    if (text == NULL) return NULL;
    size_t length = strlen(text) + 1;
    char *copy = malloc(length);
    if (copy != NULL) memcpy(copy, text, length);
    return copy;
}

int fr_json_read_file(const char *path, cJSON **out, fr_error *err) {
    *out = NULL;
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        fr_error_set(err, "cannot open \"%s\"", path);
        return FR_ERR;
    }
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (size < 0) {
        fclose(file);
        fr_error_set(err, "cannot size \"%s\"", path);
        return FR_ERR;
    }
    char *buffer = malloc((size_t) size + 1);
    if (buffer == NULL) {
        fclose(file);
        fr_error_set(err, "out of memory reading \"%s\"", path);
        return FR_ERR;
    }
    size_t read = fread(buffer, 1, (size_t) size, file);
    fclose(file);
    if (read != (size_t) size) {
        free(buffer);
        fr_error_set(err, "cannot read \"%s\"", path);
        return FR_ERR;
    }
    buffer[read] = '\0';

    cJSON *root = cJSON_Parse(buffer);
    free(buffer);
    if (root == NULL) {
        fr_error_set(err, "\"%s\" is not valid json", path);
        return FR_ERR;
    }
    *out = root;
    return FR_OK;
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

int fr_json_array_of_strings(const cJSON *object, const char *key, const char *path,
                             char ***out, size_t *count, fr_error *err) {
    *out = NULL;
    *count = 0;
    const cJSON *member = cJSON_GetObjectItemCaseSensitive(object, key);
    if (member == NULL) return FR_OK;
    if (!cJSON_IsArray(member)) {
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
