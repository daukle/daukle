#include "config_json.h"

#include "error.h"

#include <string.h>

static void report_parse_failure(const char *text, fr_error *err) {
    const char *position = cJSON_GetErrorPtr();
    if (position == NULL || position < text || position > text + strlen(text)) {
        fr_error_set(err, "not valid json");
        return;
    }
    fr_error_set(err, "not valid json at byte %zu, near \"%.30s\"",
                 (size_t) (position - text), position);
}

int fr_config_json_parse(const char *text, cJSON **out, fr_error *err) {
    cJSON *root = cJSON_Parse(text);
    if (root == NULL) {
        report_parse_failure(text, err);
        return FR_ERR;
    }
    *out = root;
    return FR_OK;
}
