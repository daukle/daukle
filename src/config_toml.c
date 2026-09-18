#include "config_toml.h"

#include "error.h"

#include "cJSON.h"
#include "toml.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static cJSON *table_to_json(toml_table_t *table);
static cJSON *array_to_json(toml_array_t *array);

static cJSON *datum_string(toml_datum_t datum) {
    if (!datum.ok) return NULL;
    cJSON *value = cJSON_CreateString(datum.u.s);
    free(datum.u.s);
    return value;
}

static cJSON *datum_timestamp(toml_timestamp_t *stamp) {
    char buffer[64];
    int length = 0;
    buffer[0] = '\0';

    if (stamp->year != NULL) {
        length += snprintf(buffer + length, sizeof buffer - (size_t) length, "%04d-%02d-%02d",
                           *stamp->year, *stamp->month, *stamp->day);
    }
    if (stamp->hour != NULL) {
        if (length > 0) buffer[length++] = 'T';
        length += snprintf(buffer + length, sizeof buffer - (size_t) length, "%02d:%02d:%02d",
                           *stamp->hour, *stamp->minute, *stamp->second);
        if (stamp->millisec != NULL) {
            length += snprintf(buffer + length, sizeof buffer - (size_t) length, ".%03d", *stamp->millisec);
        }
    }
    if (stamp->z != NULL) {
        length += snprintf(buffer + length, sizeof buffer - (size_t) length, "%s", stamp->z);
    }

    cJSON *value = cJSON_CreateString(buffer);
    free(stamp);
    return value;
}

static cJSON *value_in_table(toml_table_t *table, const char *key) {
    toml_table_t *child = toml_table_in(table, key);
    if (child != NULL) return table_to_json(child);

    toml_array_t *array = toml_array_in(table, key);
    if (array != NULL) return array_to_json(array);

    toml_datum_t text = toml_string_in(table, key);
    if (text.ok) return datum_string(text);

    toml_datum_t boolean = toml_bool_in(table, key);
    if (boolean.ok) return cJSON_CreateBool(boolean.u.b);

    toml_datum_t integer = toml_int_in(table, key);
    if (integer.ok) return cJSON_CreateNumber((double) integer.u.i);

    toml_datum_t real = toml_double_in(table, key);
    if (real.ok) return cJSON_CreateNumber(real.u.d);

    toml_datum_t stamp = toml_timestamp_in(table, key);
    if (stamp.ok) return datum_timestamp(stamp.u.ts);

    return NULL;
}

static cJSON *value_at_index(toml_array_t *array, int index) {
    toml_table_t *child = toml_table_at(array, index);
    if (child != NULL) return table_to_json(child);

    toml_array_t *nested = toml_array_at(array, index);
    if (nested != NULL) return array_to_json(nested);

    toml_datum_t text = toml_string_at(array, index);
    if (text.ok) return datum_string(text);

    toml_datum_t boolean = toml_bool_at(array, index);
    if (boolean.ok) return cJSON_CreateBool(boolean.u.b);

    toml_datum_t integer = toml_int_at(array, index);
    if (integer.ok) return cJSON_CreateNumber((double) integer.u.i);

    toml_datum_t real = toml_double_at(array, index);
    if (real.ok) return cJSON_CreateNumber(real.u.d);

    toml_datum_t stamp = toml_timestamp_at(array, index);
    if (stamp.ok) return datum_timestamp(stamp.u.ts);

    return NULL;
}

static cJSON *table_to_json(toml_table_t *table) {
    cJSON *object = cJSON_CreateObject();
    if (object == NULL) return NULL;
    for (int index = 0; ; index++) {
        const char *key = toml_key_in(table, index);
        if (key == NULL) break;
        cJSON *value = value_in_table(table, key);
        if (value == NULL) {
            cJSON_Delete(object);
            return NULL;
        }
        cJSON_AddItemToObject(object, key, value);
    }
    return object;
}

static cJSON *array_to_json(toml_array_t *array) {
    cJSON *items = cJSON_CreateArray();
    if (items == NULL) return NULL;
    int count = toml_array_nelem(array);
    for (int index = 0; index < count; index++) {
        cJSON *value = value_at_index(array, index);
        if (value == NULL) {
            cJSON_Delete(items);
            return NULL;
        }
        cJSON_AddItemToArray(items, value);
    }
    return items;
}

static int config_toml_load(void *state, const char *text, const char *origin, const char *base_dir,
                            fr_registry *registry, const cJSON *document,
                            cJSON **out, fr_error *err) {
    (void) state; (void) base_dir; (void) registry; (void) document;

    char *mutable_text = malloc(strlen(text) + 1);
    if (mutable_text == NULL) {
        fr_error_set(err, "out of memory reading \"%s\"", origin);
        return FR_ERR;
    }
    memcpy(mutable_text, text, strlen(text) + 1);

    char message[200];
    toml_table_t *table = toml_parse(mutable_text, message, sizeof message);
    free(mutable_text);
    if (table == NULL) {
        fr_error_set(err, "\"%s\" is not valid toml: %s", origin, message);
        return FR_ERR;
    }

    cJSON *root = table_to_json(table);
    toml_free(table);
    if (root == NULL) {
        fr_error_set(err, "\"%s\" holds a value daukle cannot represent", origin);
        return FR_ERR;
    }
    *out = root;
    return FR_OK;
}

const fr_config_plugin FR_CONFIG_TOML = { "daukle.config/toml", "daukle.toml", 0, config_toml_load, NULL };
