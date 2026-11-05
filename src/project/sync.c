#include "project/sync.h"

#include "cache/cache.h"
#include "config/config.h"
#include "config/config_lua.h"
#include "config/config_toml.h"
#include "config/manifest.h"
#include "plugin/plugins.h"
#include "plugin/registry.h"
#include "plugin/resolvers.h"
#include "project/generate.h"
#include "util/error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *manifest_directory(const char *manifest_path) {
    const char *last_slash = strrchr(manifest_path, '/');
    const char *last_backslash = strrchr(manifest_path, '\\');
    const char *last = last_slash;
    if (last_backslash != NULL && (last == NULL || last_backslash > last)) last = last_backslash;

    size_t length = last != NULL ? (size_t) (last - manifest_path) : 0;
    char *dir = malloc(length + 1);
    if (dir != NULL) {
        memcpy(dir, manifest_path, length);
        dir[length] = '\0';
    }
    return dir;
}

int fr_sync_report_add(fr_sync_report *report, char *path, fr_error *err) {
    for (size_t index = 0; index < report->count; index++) {
        if (strcmp(report->files[index], path) == 0) {
            free(path);
            return FR_OK;
        }
    }

    char **files = realloc(report->files, (report->count + 1) * sizeof *files);
    if (files == NULL) {
        fr_error_set(err, "out of memory recording \"%s\"", path);
        return FR_ERR;
    }
    report->files = files;
    report->files[report->count++] = path;
    return FR_OK;
}

int fr_build_registry(fr_registry **out, fr_error *err) {
    fr_registry *registry = fr_registry_create();
    if (registry == NULL) {
        fr_error_set(err, "out of memory creating the plugin registry");
        return FR_ERR;
    }
    if (fr_registry_add_config(registry, &FR_CONFIG_TOML, err) != FR_OK) {
        fr_registry_destroy(registry);
        return FR_ERR;
    }
    if (fr_registry_add_config(registry, &FR_CONFIG_LUA, err) != FR_OK) {
        fr_registry_destroy(registry);
        return FR_ERR;
    }
    *out = registry;
    return FR_OK;
}

int fr_session_open(const char *manifest_path, int use_cache, fr_session *out, fr_error *err) {
    memset(out, 0, sizeof *out);
    fr_cache_set_enabled(use_cache);

    if (fr_build_registry(&out->registry, err) != FR_OK) return FR_ERR;

    if (fr_config_load_file(manifest_path, out->registry, &out->manifest, err) != FR_OK) {
        fr_session_close(out);
        return FR_ERR;
    }
    out->loaded = 1;

    out->manifest_dir = manifest_directory(manifest_path);
    out->manifest_path = fr_dup_string(manifest_path);
    if (out->manifest_dir == NULL || out->manifest_path == NULL) {
        fr_error_set(err, "out of memory deriving the manifest directory");
        fr_session_close(out);
        return FR_ERR;
    }
    return FR_OK;
}

void fr_session_close(fr_session *session) {
    if (session == NULL) return;
    fr_registry_destroy(session->registry);
    fr_lua_runtime_shutdown();
    fr_plugins_report_clear();
    fr_resolvers_clear();
    free(session->manifest_dir);
    free(session->manifest_path);
    if (session->loaded) fr_manifest_free(&session->manifest);
    memset(session, 0, sizeof *session);
}

int fr_sync_session(const fr_session *session, int write, fr_sync_report *report, fr_error *err) {
    memset(report, 0, sizeof *report);
    return fr_generate(&session->manifest, session->manifest_path, session->manifest_dir,
                       session->registry, write, report, err);
}

int fr_sync(const char *manifest_path, int write, int use_cache, fr_sync_report *report, fr_error *err) {
    memset(report, 0, sizeof *report);
    fr_session session;
    if (fr_session_open(manifest_path, use_cache, &session, err) != FR_OK) return FR_ERR;
    int result = fr_sync_session(&session, write, report, err);
    fr_session_close(&session);
    return result;
}

void fr_sync_report_free(fr_sync_report *report) {
    if (report == NULL) return;
    for (size_t index = 0; index < report->count; index++) free(report->files[index]);
    free(report->files);
    report->files = NULL;
    report->count = 0;
}
