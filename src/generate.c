#include "generate.h"

#include "derived.h"
#include "error.h"
#include "exec.h"
#include "lua_sandbox.h"
#include "plugins.h"
#include "resolve.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FR_GENERATE_MAX_FILES 64

static void wrap_error_with_path(fr_error *err, const char *path) {
    char original[sizeof err->message];
    memcpy(original, err->message, sizeof original);
    fr_error_set(err, "%s: %s", path, original);
}

static int path_is_safe(const char *path) {
    if (path[0] == '\0') return 0;
    if (fr_lua_sandbox_climbs_out(path)) return 0;
    if (strchr(path, '\\') != NULL) return 0;
    for (const char *cursor = path; *cursor != '\0'; cursor++) {
        if ((unsigned char) *cursor < 0x20) return 0;
    }
    return 1;
}

static int validate(const fr_generated_file *files, size_t count, const char *canonical_root,
                    fr_error *err) {
    if (count > FR_GENERATE_MAX_FILES) {
        fr_error_set(err, "a toolchain may generate at most %d files", FR_GENERATE_MAX_FILES);
        return FR_ERR;
    }
    for (size_t index = 0; index < count; index++) {
        if (!path_is_safe(files[index].path)) {
            fr_error_set(err, "generated path \"%s\" must stay inside the derived directory",
                         files[index].path);
            return FR_ERR;
        }
        if (strlen(files[index].text) > FR_EXEC_CAPTURE_LIMIT) {
            fr_error_set(err, "generated file \"%s\" is larger than the 1 MiB limit",
                         files[index].path);
            return FR_ERR;
        }
        if (canonical_root != NULL && strstr(files[index].text, canonical_root) != NULL) {
            fr_error_set(err, "generated file \"%s\" names the project root; "
                              "use the relative \"root\" instead",
                         files[index].path);
            return FR_ERR;
        }
    }
    return FR_OK;
}

/* fr_derived_report's paths are owned by that report and freed by
   fr_derived_report_free; report gets its own copies so the two frees never
   race over the same allocation. */
static int adopt_derived_paths(fr_sync_report *report, const fr_derived_report *derived_report,
                               fr_error *err) {
    for (size_t index = 0; index < derived_report->count; index++) {
        char *copy = fr_dup_string(derived_report->paths[index]);
        if (copy == NULL) {
            fr_error_set(err, "out of memory recording \"%s\"", derived_report->paths[index]);
            return FR_ERR;
        }
        if (fr_sync_report_add(report, copy, err) != FR_OK) return FR_ERR;
    }
    return FR_OK;
}

static int generate_toolchain(const fr_toolchain *toolchain, const fr_manifest *manifest,
                              const char *manifest_path, const char *manifest_dir,
                              const fr_registry *registry, int write, const char *canonical_root,
                              fr_sync_report *report, fr_error *err) {
    char capability[256];
    snprintf(capability, sizeof capability, "daukle.toolchain/%s", toolchain->name);
    const fr_toolchain_plugin *plugin = fr_registry_toolchain(registry, capability);
    if (plugin == NULL) {
        fr_error_set(err, "toolchain \"%s\": no plugin provides toolchain \"%s\"; "
                          "add it to [plugins] in daukle.toml",
                    toolchain->name, toolchain->name);
        return FR_ERR;
    }

    fr_consumer consumer;
    memset(&consumer, 0, sizeof consumer);
    consumer.id = toolchain->name;
    consumer.language = toolchain->name;
    consumer.dependencies = toolchain->dependencies;
    consumer.dependency_count = toolchain->dependency_count;

    fr_resolved *resolved = NULL;
    size_t resolved_count = 0;
    if (fr_resolve_consumer(&consumer, manifest, manifest_dir, registry, &resolved, &resolved_count,
                            err) != FR_OK) {
        wrap_error_with_path(err, manifest_path);
        return FR_ERR;
    }

    char *derived_dir = NULL;
    if (fr_derived_dir(manifest_dir, toolchain->name, &derived_dir, err) != FR_OK) {
        fr_resolved_free(resolved, resolved_count);
        return FR_ERR;
    }

    fr_generated_file *files = NULL;
    size_t file_count = 0;
    int result = plugin->generate(plugin->state, toolchain, manifest->self.project,
                                  toolchain->version, "../../..", resolved, resolved_count,
                                  &files, &file_count, err);
    fr_resolved_free(resolved, resolved_count);

    if (result == FR_OK) result = validate(files, file_count, canonical_root, err);

    /* Only when a file will actually land, and only when writing: an empty
       table must leave build/daukle/ untouched, and a check pass (write == 0)
       must not create it either. */
    if (result == FR_OK && write && file_count > 0) {
        char *derived_root = NULL;
        if (fr_derived_root(manifest_dir, &derived_root, err) != FR_OK) {
            result = FR_ERR;
        } else {
            result = fr_derived_ensure_root(derived_root, err);
            free(derived_root);
        }
    }

    if (result == FR_OK) {
        fr_derived_report derived_report;
        result = fr_derived_apply(derived_dir, files, file_count, write, &derived_report, err);
        if (result == FR_OK) {
            result = adopt_derived_paths(report, &derived_report, err);
            fr_derived_report_free(&derived_report);
        }
    }

    fr_derived_free_files(files, file_count);
    free(derived_dir);
    return result;
}

int fr_generate(const fr_manifest *manifest, const char *manifest_path, const char *manifest_dir,
                const fr_registry *registry, int write, fr_sync_report *report, fr_error *err) {
    char *canonical_root = NULL;
    fr_error canonical_err;
    if (fr_lua_sandbox_canonical_dir(manifest_dir, &canonical_root, &canonical_err) != FR_OK) {
        canonical_root = NULL;
    }

    int result = FR_OK;
    for (size_t index = 0; index < manifest->toolchain_count; index++) {
        if (generate_toolchain(&manifest->toolchains[index], manifest, manifest_path, manifest_dir,
                               registry, write, canonical_root, report, err) != FR_OK) {
            result = FR_ERR;
            break;
        }
    }

    free(canonical_root);
    return result;
}
