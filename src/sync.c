#include "sync.h"

#include "cache.h"
#include "config.h"
#include "config_lua.h"
#include "config_toml.h"
#include "error.h"
#include "generate.h"
#include "manifest.h"
#include "plugins.h"
#include "region.h"
#include "registry.h"
#include "resolve.h"
#include "resolvers.h"

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

static char *join_path(const char *dir, const char *file) {
    const char *base = (dir == NULL || dir[0] == '\0') ? "." : dir;
    size_t length = strlen(base) + 1 + strlen(file) + 1;
    char *joined = malloc(length);
    if (joined != NULL) snprintf(joined, length, "%s/%s", base, file);
    return joined;
}

static void wrap_error_with_path(fr_error *err, const char *path) {
    char original[sizeof err->message];
    memcpy(original, err->message, sizeof original);
    fr_error_set(err, "%s: %s", path, original);
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

static int sync_consumer(const fr_consumer *consumer, const fr_manifest *manifest,
                         const char *manifest_path, const char *manifest_dir,
                         const fr_registry *registry, int write,
                         fr_sync_report *report, fr_error *err) {
    char *target_path = join_path(manifest_dir, consumer->file);
    if (target_path == NULL) {
        fr_error_set(err, "out of memory building the target path for consumer \"%s\"", consumer->id);
        return FR_ERR;
    }

    fr_resolved *resolved = NULL;
    size_t count = 0;
    if (fr_resolve_consumer(consumer, manifest, manifest_dir, registry, &resolved, &count, err) != FR_OK) {
        wrap_error_with_path(err, manifest_path);
        free(target_path);
        return FR_ERR;
    }

    char capability[256];
    snprintf(capability, sizeof capability, "daukle.language/%s", consumer->language);
    const fr_language_plugin *language = fr_registry_language(registry, capability);
    if (language == NULL) {
        fr_error_set(err, "consumer \"%s\": no plugin provides language \"%s\"; add it to [plugins] in daukle.toml",
                    consumer->id, consumer->language);
        fr_resolved_free(resolved, count);
        free(target_path);
        return FR_ERR;
    }

    char *original_text = NULL;
    if (fr_file_read_text(target_path, &original_text, err) != FR_OK) {
        wrap_error_with_path(err, target_path);
        fr_resolved_free(resolved, count);
        free(target_path);
        return FR_ERR;
    }

    char *replaced = NULL;
    int applied = language->apply(language->state, consumer, resolved, count,
                                  original_text, &replaced, err);
    fr_resolved_free(resolved, count);
    if (applied != FR_OK) {
        wrap_error_with_path(err, target_path);
        free(original_text);
        free(target_path);
        return FR_ERR;
    }

    int result = FR_OK;
    if (strcmp(original_text, replaced) != 0) {
        if (write) result = fr_file_write_text(target_path, replaced, err);
        if (result != FR_OK) {
            wrap_error_with_path(err, target_path);
        } else if (fr_sync_report_add(report, target_path, err) != FR_OK) {
            result = FR_ERR;
        } else {
            target_path = NULL;
        }
    }

    free(original_text);
    free(replaced);
    free(target_path);
    return result;
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
    if (fr_generate(&session->manifest, session->manifest_path, session->manifest_dir,
                    session->registry, write, report, err) != FR_OK) {
        return FR_ERR;
    }
    for (size_t index = 0; index < session->manifest.consumer_count; index++) {
        if (sync_consumer(&session->manifest.consumers[index], &session->manifest,
                          session->manifest_path, session->manifest_dir,
                          session->registry, write, report, err) != FR_OK) {
            return FR_ERR;
        }
    }
    return FR_OK;
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
