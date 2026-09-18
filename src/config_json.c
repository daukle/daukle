#include "config_json.h"

#include "error.h"
#include "jsonx.h"

static int config_json_load(void *state, const char *text, const char *origin, const char *base_dir,
                            fr_registry *registry, const cJSON *document,
                            cJSON **out, fr_error *err) {
    (void) state; (void) base_dir; (void) registry; (void) document;
    cJSON *root = cJSON_Parse(text);
    if (root == NULL) {
        fr_error_set(err, "\"%s\" is not valid json", origin);
        return FR_ERR;
    }
    *out = root;
    return FR_OK;
}

const fr_config_plugin FR_CONFIG_JSON = { "daukle.config/json", "daukle.json", 0, config_json_load, NULL };
