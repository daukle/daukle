#include "config.h"

#include "error.h"
#include "manifest.h"
#include "region.h"

#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *extension_of(const char *file_path) {
    const char *dot = strrchr(file_path, '.');
    return dot == NULL ? "" : dot + 1;
}

static char *directory_of(const char *file_path) {
    const char *slash = strrchr(file_path, '/');
    const char *backslash = strrchr(file_path, '\\');
    const char *last;
    if (slash == NULL) {
        last = backslash;
    } else if (backslash == NULL) {
        last = slash;
    } else {
        last = slash > backslash ? slash : backslash;
    }
    if (last == NULL) {
        char *here = malloc(2);
        if (here != NULL) memcpy(here, ".", 2);
        return here;
    }
    size_t length = (size_t) (last - file_path);
    char *directory = malloc(length + 1);
    if (directory != NULL) {
        memcpy(directory, file_path, length);
        directory[length] = '\0';
    }
    return directory;
}

static char *join(const char *directory, const char *name) {
    size_t length = strlen(directory) + 1 + strlen(name) + 1;
    char *path = malloc(length);
    if (path != NULL) snprintf(path, length, "%s/%s", directory, name);
    return path;
}

static int file_exists(const char *path) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) return 0;
    fclose(file);
    return 1;
}

static const fr_config_plugin *plugin_for(const fr_registry *registry, const char *file_path,
                                          fr_error *err) {
    char capability[128];
    snprintf(capability, sizeof capability, "daukle.config/%s", extension_of(file_path));
    const fr_config_plugin *plugin = fr_registry_config(registry, capability);
    if (plugin == NULL) {
        fr_error_set(err, "\"%s\": no config format registered for \"%s\"",
                     file_path, extension_of(file_path));
    }
    return plugin;
}

static int apply_overlays(const char *directory, fr_registry *registry,
                          cJSON *document, cJSON **out, fr_error *err) {
    cJSON *current = document;
    for (size_t index = 0; index < fr_registry_config_count(registry); index++) {
        const fr_config_plugin *plugin = fr_registry_config_at(registry, index);
        if (!plugin->overlay) continue;

        char *path = join(directory, plugin->file_name);
        if (path == NULL) {
            fr_error_set(err, "out of memory joining \"%s\"", plugin->file_name);
            cJSON_Delete(current);
            return FR_ERR;
        }
        if (!file_exists(path)) {
            free(path);
            continue;
        }

        char *text = NULL;
        if (fr_file_read_text(path, &text, err) != FR_OK) {
            free(path);
            cJSON_Delete(current);
            return FR_ERR;
        }
        cJSON *next = NULL;
        int status = plugin->load(plugin->state, text, path, directory, registry, current, &next, err);
        free(text);
        free(path);
        cJSON_Delete(current);
        if (status != FR_OK) return FR_ERR;
        current = next;
    }
    *out = current;
    return FR_OK;
}

int fr_config_load_file(const char *file_path, fr_registry *registry, fr_manifest *out, fr_error *err) {
    memset(out, 0, sizeof *out);

    const fr_config_plugin *plugin = plugin_for(registry, file_path, err);
    if (plugin == NULL) return FR_ERR;

    char *directory = directory_of(file_path);
    if (directory == NULL) {
        fr_error_set(err, "out of memory deriving the directory of \"%s\"", file_path);
        return FR_ERR;
    }

    char *text = NULL;
    if (fr_file_read_text(file_path, &text, err) != FR_OK) {
        free(directory);
        return FR_ERR;
    }

    cJSON *document = NULL;
    int status = plugin->load(plugin->state, text, file_path, directory, registry, NULL, &document, err);
    free(text);
    if (status != FR_OK) {
        free(directory);
        return FR_ERR;
    }

    cJSON *final = NULL;
    if (plugin->overlay) {
        final = document;
    } else if (apply_overlays(directory, registry, document, &final, err) != FR_OK) {
        free(directory);
        return FR_ERR;
    }
    free(directory);
    return fr_manifest_from_document(final, file_path, out, err);
}

int fr_config_find(const char *directory, const fr_registry *registry, char **out_path, fr_error *err) {
    *out_path = NULL;
    char *primary = NULL;
    char *overlay = NULL;

    for (size_t index = 0; index < fr_registry_config_count(registry); index++) {
        const fr_config_plugin *plugin = fr_registry_config_at(registry, index);
        char *path = join(directory, plugin->file_name);
        if (path == NULL) {
            fr_error_set(err, "out of memory joining \"%s\"", plugin->file_name);
            free(primary);
            free(overlay);
            return FR_ERR;
        }
        if (!file_exists(path)) {
            free(path);
            continue;
        }
        if (plugin->overlay) {
            free(overlay);
            overlay = path;
        } else if (primary != NULL) {
            fr_error_set(err, "\"%s\" holds both \"%s\" and \"%s\": keep one",
                         directory, primary, path);
            free(primary);
            free(overlay);
            free(path);
            return FR_ERR;
        } else {
            primary = path;
        }
    }

    if (primary != NULL) {
        free(overlay);
        *out_path = primary;
        return FR_OK;
    }
    if (overlay != NULL) {
        *out_path = overlay;
        return FR_OK;
    }
    char expected[256];
    size_t written = 0;
    expected[0] = '\0';
    for (size_t index = 0; index < fr_registry_config_count(registry); index++) {
        const fr_config_plugin *plugin = fr_registry_config_at(registry, index);
        int added = snprintf(expected + written, sizeof expected - written,
                             written == 0 ? "%s" : ", %s", plugin->file_name);
        if (added < 0 || (size_t) added >= sizeof expected - written) break;
        written += (size_t) added;
    }
    fr_error_set(err, "\"%s\" holds no manifest: expected one of %s", directory, expected);
    return FR_ERR;
}
