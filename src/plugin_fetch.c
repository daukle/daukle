#include "plugin_fetch.h"

#include "cache.h"
#include "error.h"
#include "http.h"
#include "region.h"
#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int entry_path(const char *url, char *out, size_t out_size, fr_error *err) {
    char root[1024];
    if (fr_cache_root(root, sizeof root, err) != FR_OK) return FR_ERR;

    char digest[65];
    fr_sha256_hex(url, strlen(url), digest);

    int written = snprintf(out, out_size, "%s/plugins/%s/plugin.lua", root, digest);
    if (written < 0 || (size_t) written >= out_size) {
        fr_error_set(err, "the plugin cache path for \"%s\" is too long", url);
        return FR_ERR;
    }
    return FR_OK;
}

int fr_plugin_fetch(const char *url, char **out_text, fr_error *err) {
    *out_text = NULL;

    char path[1024];
    int have_path = entry_path(url, path, sizeof path, err) == FR_OK;

    if (have_path && fr_cache_enabled()) {
        FILE *probe = fopen(path, "rb");
        if (probe != NULL) {
            fclose(probe);
            return fr_file_read_text(path, out_text, err);
        }
    }

    char *body = NULL;
    size_t length = 0;
    if (fr_http_get(url, NULL, 0, &body, &length, err) != FR_OK) return FR_ERR;

    if (have_path && fr_cache_enabled()) fr_cache_write_atomic(path, body, length);

    *out_text = body;
    return FR_OK;
}

void fr_plugin_fetch_discard(const char *url) {
    fr_error ignored;
    char path[1024];
    if (entry_path(url, path, sizeof path, &ignored) != FR_OK) return;
    remove(path);
}
