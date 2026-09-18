#include "config_json.h"

#include "error.h"
#include "jsonx.h"

#include <string.h>

/* cJSON reports the position it stopped at as a pointer into the text it was
   given, which is the only positional information it offers; toml and lua both
   name a line, so a json manifest should not be the one format that says only
   that something, somewhere, is wrong. */
static void report_parse_failure(const char *text, const char *origin, fr_error *err) {
    const char *position = cJSON_GetErrorPtr();
    if (position == NULL || position < text || position > text + strlen(text)) {
        fr_error_set(err, "\"%s\" is not valid json", origin);
        return;
    }
    fr_error_set(err, "\"%s\" is not valid json at byte %zu, near \"%.30s\"", origin,
                 (size_t) (position - text), position);
}

static int config_json_load(void *state, const char *text, const char *origin, const char *base_dir,
                            fr_registry *registry, const cJSON *document,
                            cJSON **out, fr_error *err) {
    (void) state; (void) base_dir; (void) registry; (void) document;
    cJSON *root = cJSON_Parse(text);
    if (root == NULL) {
        report_parse_failure(text, origin, err);
        return FR_ERR;
    }
    *out = root;
    return FR_OK;
}

const fr_config_plugin FR_CONFIG_JSON = { "daukle.config/json", "daukle.json", 0, config_json_load, NULL };
