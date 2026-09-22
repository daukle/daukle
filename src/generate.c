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

/* canonical_root's own separator form is not the only one a plugin can write:
   CMake output on Windows uses forward slashes even though
   GetFinalPathNameByHandleA returns backslashes. Testing both spellings
   catches either; on POSIX the flipped form never occurs in real text. */
static int text_names_root(const char *text, const char *canonical_root) {
    if (canonical_root == NULL) return 0;
    if (strstr(text, canonical_root) != NULL) return 1;

    size_t length = strlen(canonical_root);
    char flipped[1024];
    if (length >= sizeof flipped) return 0;
    for (size_t index = 0; index < length; index++) {
        char c = canonical_root[index];
        flipped[index] = c == '/' ? '\\' : c == '\\' ? '/' : c;
    }
    flipped[length] = '\0';
    return strstr(text, flipped) != NULL;
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
        if (text_names_root(files[index].text, canonical_root)) {
            fr_error_set(err, "generated file \"%s\" names the project root; "
                              "use the relative \"root\" instead",
                         files[index].path);
            return FR_ERR;
        }
    }
    return FR_OK;
}

static int should_seed_derived_root(int validated, int write, size_t file_count) {
    return validated == FR_OK && write && file_count > 0;
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
        if (fr_sync_report_add(report, copy, err) != FR_OK) {
            free(copy);
            return FR_ERR;
        }
    }
    return FR_OK;
}

static int refuse_if_declared_in_both_modes(const fr_toolchain *toolchain, const fr_manifest *manifest,
                                            fr_error *err) {
    for (size_t index = 0; index < manifest->consumer_count; index++) {
        if (strcmp(manifest->consumers[index].language, toolchain->name) == 0) {
            fr_error_set(err, "\"%s\" is declared as a toolchain and as a consumer's language; "
                              "a tool is in managed mode or in adopted mode, never both",
                        toolchain->name);
            return FR_ERR;
        }
    }
    return FR_OK;
}

static int generate_toolchain(const fr_toolchain *toolchain, const fr_manifest *manifest,
                              const char *manifest_path, const char *manifest_dir,
                              const fr_registry *registry, int write, const char *canonical_root,
                              fr_sync_report *report, fr_error *err) {
    if (refuse_if_declared_in_both_modes(toolchain, manifest, err) != FR_OK) return FR_ERR;

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

    char project_version[64];
    snprintf(project_version, sizeof project_version, "%d.%d.%d", manifest->self.version.major,
            manifest->self.version.minor, manifest->self.version.patch);

    fr_generated_file *files = NULL;
    size_t file_count = 0;
    int result = plugin->generate(plugin->state, toolchain, manifest->self.project,
                                  project_version, "../../..", resolved, resolved_count,
                                  &files, &file_count, err);
    fr_resolved_free(resolved, resolved_count);

    if (result == FR_OK) result = validate(files, file_count, canonical_root, err);

    if (should_seed_derived_root(result, write, file_count)) {
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
    const char *canonical_base = (manifest_dir == NULL || manifest_dir[0] == '\0') ? "." : manifest_dir;

    char *canonical_root = NULL;
    fr_error canonical_err;
    if (fr_lua_sandbox_canonical_dir(canonical_base, &canonical_root, &canonical_err) != FR_OK) {
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
