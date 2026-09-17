#ifndef TIESTONE_JSONX_H
#define TIESTONE_JSONX_H

#include "types.h"

#include <stddef.h>

#include "cJSON.h"

int fr_json_read_file(const char *path, cJSON **out, fr_error *err);
int fr_json_string(const cJSON *object, const char *key, const char *path, const char **out, fr_error *err);
int fr_json_object(const cJSON *object, const char *key, const char *path, const cJSON **out, fr_error *err);
int fr_json_array_of_strings(const cJSON *object, const char *key, const char *path,
                             char ***out, size_t *count, fr_error *err);
void fr_string_array_free(char **items, size_t count);

#endif
