#include "migrate/migrate_internal.h"

#include "cache/cache.h"
#include "config/config_lua.h"
#include "migrate/merge.h"
#include "plugin/plugins.h"
#include "plugin/registry.h"
#include "plugin/resolvers.h"
#include "project/sync.h"
#include "util/error.h"
#include "util/strbuf.h"

#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif

#define FR_MIGRATE_WHERE_SIZE 512

static int out_of_memory(fr_error *err, const char *doing) {
    fr_error_set(err, "out of memory %s", doing);
    return FR_ERR;
}

int fr_migrate_core_blocker(fr_migrate_state *state, const char *construct, const char *where,
                            const char *why, fr_error *err) {
    return fr_blockers_add(&state->blockers, "core", construct, where, why, NULL, err);
}

int fr_migrate_consider(fr_migrate_state *state, const fr_catalog_plugin *plugin, fr_error *err) {
    for (size_t index = 0; index < state->considered_count; index++) {
        if (strcmp(state->considered[index]->id, plugin->id) == 0) return FR_OK;
    }
    const fr_catalog_plugin **grown =
        realloc(state->considered, (state->considered_count + 1) * sizeof *grown);
    if (grown == NULL) return out_of_memory(err, "recording the plugins to pin");
    state->considered = grown;
    state->considered[state->considered_count++] = plugin;
    if (plugin->asset_url != NULL) return FR_OK;
    return fr_migrate_core_blocker(state, "plugin release", plugin->id,
                                   "the catalog names no published release, so there is nothing"
                                   " to pin", err);
}

static void list_manifests(const fr_registry *registry, fr_strbuf *found, size_t *count) {
    for (size_t index = 0; index < fr_registry_config_count(registry); index++) {
        const char *name = fr_registry_config_at(registry, index)->file_name;
        if (!fr_cache_file_exists(name)) continue;
        fr_strbuf_append_format(found, "%s%s", *count == 0 ? "" : " and ", name);
        (*count)++;
    }
}

/* Every registered manifest name is probed rather than asking fr_config_find,
   whose FR_ERR means both "none here" and "two here", and a directory holding
   two manifests must be refused as firmly as one holding one. */
static int refuse_a_manifest(fr_error *err) {
    fr_registry *registry = NULL;
    if (fr_build_registry(&registry, err) != FR_OK) return FR_ERR;
    fr_strbuf found;
    fr_strbuf_init(&found);
    size_t count = 0;
    list_manifests(registry, &found, &count);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    fr_plugins_report_clear();
    fr_resolvers_clear();
    char *names = fr_strbuf_release(&found);
    if (names == NULL) return out_of_memory(err, "looking for an existing manifest");
    int status = FR_OK;
    if (count > 0) {
        fr_error_set(err, "this directory already holds %s; migrate converts a project with no"
                          " daukle manifest", names);
        status = FR_ERR;
    }
    free(names);
    return status;
}

static int load_catalogs(fr_migrate_state *state, fr_error *err) {
    if (state->options->catalog_count == 0) {
        fr_error_set(err, "migrate was given no catalog to detect with: pass --catalog <location>"
                          " or set DAUKLE_CATALOGS. The default list is the \"catalogs\" key of"
                          " the wrapper.toml each daukle release publishes.");
        return FR_ERR;
    }
    if (fr_catalog_load(state->options->catalogs, state->options->catalog_count, &state->catalog,
                        err) != FR_OK) {
        return FR_ERR;
    }
    for (size_t index = 0; index < state->catalog.warning_count; index++) {
        fprintf(stderr, "daukle migrate: %s\n", state->catalog.warnings[index]);
    }
    return FR_OK;
}

static int detect_plugins(fr_migrate_state *state, fr_error *err) {
    if (fr_detect(".", &state->catalog, state->options->with, state->options->with_count,
                  &state->detections, err) != FR_OK) {
        return FR_ERR;
    }
    if (state->detections.count == 0) {
        fr_error_set(err, "no catalog plugin recognised this project; pass --with <id> to name one");
        return FR_ERR;
    }
    for (size_t index = 0; index < state->detections.count; index++) {
        if (fr_migrate_consider(state, state->detections.items[index].plugin, err) != FR_OK) {
            return FR_ERR;
        }
    }
    state->detected_count = state->considered_count;
    return FR_OK;
}

static int replaces_holds(const fr_catalog_plugin *plugin, const char *tool, const char *kind,
                          const char *id) {
    const cJSON *by_tool = plugin->replaces != NULL
        ? cJSON_GetObjectItemCaseSensitive(plugin->replaces, tool) : NULL;
    const cJSON *ids = by_tool != NULL ? cJSON_GetObjectItemCaseSensitive(by_tool, kind) : NULL;
    const cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, ids) {
        if (cJSON_IsString(entry) && strcmp(entry->valuestring, id) == 0) return 1;
    }
    return 0;
}

static int record_foreign(fr_migrate_state *state, size_t kept_index, const cJSON *foreign,
                          const fr_catalog_plugin *replacer, fr_error *err) {
    fr_migrate_foreign *grown =
        realloc(state->foreign, (state->foreign_count + 1) * sizeof *grown);
    if (grown == NULL) return out_of_memory(err, "recording a foreign plugin");
    state->foreign = grown;
    fr_migrate_foreign *record = &state->foreign[state->foreign_count++];
    record->kept_index = kept_index;
    record->foreign = foreign;
    record->replacer = replacer;
    record->translation = NULL;
    return fr_migrate_consider(state, replacer, err);
}

/* A foreign plugin claimed by two catalog plugins is a blocker naming both:
   picking the earlier catalog would make the answer depend on the order the
   user listed catalogs in. */
static int claim_foreign(fr_migrate_state *state, size_t kept_index, const cJSON *foreign,
                         fr_error *err) {
    const char *tool = state->kept[kept_index].tool;
    const char *kind = cJSON_GetObjectItemCaseSensitive(foreign, "kind")->valuestring;
    const char *id = cJSON_GetObjectItemCaseSensitive(foreign, "id")->valuestring;
    char where[FR_MIGRATE_WHERE_SIZE];
    snprintf(where, sizeof where, "%s %s %s", tool, kind, id);

    const fr_catalog_plugin *claimant = NULL;
    size_t claimants = 0;
    fr_strbuf names;
    fr_strbuf_init(&names);
    fr_strbuf_append(&names, "claimed by");
    for (size_t index = 0; index < state->catalog.count; index++) {
        const fr_catalog_plugin *plugin = &state->catalog.plugins[index];
        if (!replaces_holds(plugin, tool, kind, id)) continue;
        fr_strbuf_append_format(&names, "%s %s", claimants == 0 ? "" : ",", plugin->id);
        claimant = plugin;
        claimants++;
    }
    fr_strbuf_append(&names, "; a tie between catalogs is never broken by their order");
    char *tie = fr_strbuf_release(&names);
    if (tie == NULL) return out_of_memory(err, "naming the plugins claiming a foreign plugin");

    int status;
    if (claimants == 0) {
        status = fr_migrate_core_blocker(state, "foreign plugin", where,
                                         "no catalog names a replacement for it", err);
    } else if (claimants > 1) {
        status = fr_migrate_core_blocker(state, "foreign plugin", where, tie, err);
    } else {
        status = record_foreign(state, kept_index, foreign, claimant, err);
    }
    free(tie);
    return status;
}

int fr_migrate_collect_foreign(fr_migrate_state *state, fr_error *err) {
    for (size_t index = 0; index < state->kept_count; index++) {
        const cJSON *list = cJSON_GetObjectItemCaseSensitive(state->kept[index].result, "foreign");
        const cJSON *foreign = NULL;
        cJSON_ArrayForEach(foreign, list) {
            if (claim_foreign(state, index, foreign, err) != FR_OK) return FR_ERR;
        }
    }
    return FR_OK;
}

static int merge_fragment(fr_migrate_state *state, const cJSON *answer, const char *source,
                          cJSON *owner_of, fr_error *err) {
    const cJSON *fragment = cJSON_GetObjectItemCaseSensitive(answer, "manifest");
    if (fragment == NULL) return FR_OK;
    return fr_migrate_merge(state->manifest, fragment, source, owner_of, &state->blockers, err);
}

static int merge_answers(fr_migrate_state *state, cJSON *owner_of, fr_error *err) {
    char source[FR_MIGRATE_WHERE_SIZE];
    for (size_t index = 0; index < state->kept_count; index++) {
        snprintf(source, sizeof source, "migrator %s", state->kept[index].label);
        if (merge_fragment(state, state->kept[index].result, source, owner_of, err) != FR_OK) {
            return FR_ERR;
        }
    }
    for (size_t index = 0; index < state->foreign_count; index++) {
        const fr_migrate_foreign *record = &state->foreign[index];
        if (record->translation == NULL) continue;
        snprintf(source, sizeof source, "translator %s/%s", state->kept[record->kept_index].tool,
                 cJSON_GetObjectItemCaseSensitive(record->foreign, "id")->valuestring);
        if (merge_fragment(state, record->translation, source, owner_of, err) != FR_OK) {
            return FR_ERR;
        }
    }
    return FR_OK;
}

/* The same few lines as fr_cli_last_path_segment, which lives in the executable
   and is not linkable from core. */
static const char *directory_name(char *buffer, size_t size) {
#ifdef _WIN32
    const char *working = _getcwd(buffer, (int) size);
#else
    const char *working = getcwd(buffer, size);
#endif
    if (working == NULL) return "project";
    const char *name = working;
    for (const char *scan = working; *scan != '\0'; scan++) {
        if (*scan == '/' || *scan == '\\') name = scan + 1;
    }
    return *name == '\0' ? "project" : name;
}

static int record_core_keys(fr_migrate_state *state, fr_error *err) {
    for (size_t index = 0; index < state->kept_count; index++) {
        const fr_migrate_kept *kept = &state->kept[index];
        if (fr_migrate_set_instance(state->manifest, kept->instance, kept->toolchain, kept->dir,
                                    err) != FR_OK) {
            return FR_ERR;
        }
    }
    char working[1024];
    if (fr_migrate_fill_defaults(state->manifest, directory_name(working, sizeof working), err)
        != FR_OK) {
        return FR_ERR;
    }
    for (size_t index = 0; index < state->considered_count; index++) {
        const fr_catalog_plugin *plugin = state->considered[index];
        if (plugin->asset_url == NULL) continue;
        if (fr_migrate_add_plugin(state->manifest, plugin->id, plugin->asset_url,
                                  plugin->asset_sha256, err) != FR_OK) {
            return FR_ERR;
        }
    }
    return FR_OK;
}

int fr_migrate_assemble(fr_migrate_state *state, fr_error *err) {
    state->manifest = cJSON_CreateObject();
    cJSON *owner_of = cJSON_CreateObject();
    if (state->manifest == NULL || owner_of == NULL) {
        cJSON_Delete(owner_of);
        return out_of_memory(err, "assembling the manifest");
    }
    int status = merge_answers(state, owner_of, err);
    cJSON_Delete(owner_of);
    if (status != FR_OK) return FR_ERR;
    return record_core_keys(state, err);
}

static void free_state(fr_migrate_state *state) {
    fr_catalog_free(&state->catalog);
    fr_detections_free(&state->detections);
    free(state->considered);
    for (size_t index = 0; index < state->kept_count; index++) {
        fr_migrate_kept *kept = &state->kept[index];
        free(kept->label);
        free(kept->tool);
        free(kept->toolchain);
        free(kept->instance);
        free(kept->dir);
        free(kept->root);
        free(kept->derived_dir_relative);
        cJSON_Delete(kept->result);
    }
    free(state->kept);
    for (size_t index = 0; index < state->foreign_count; index++) {
        cJSON_Delete(state->foreign[index].translation);
    }
    free(state->foreign);
    fr_blockers_free(&state->blockers);
    cJSON_Delete(state->manifest);
}

static int run_pipeline(fr_migrate_state *state, fr_migrate_outcome *out_outcome, fr_error *err) {
    if (refuse_a_manifest(err) != FR_OK) return FR_ERR;
    if (load_catalogs(state, err) != FR_OK) return FR_ERR;
    if (detect_plugins(state, err) != FR_OK) return FR_ERR;
    if (fr_migrate_session_a(state, err) != FR_OK) return FR_ERR;
    if (fr_migrate_collect_foreign(state, err) != FR_OK) return FR_ERR;
    if (fr_migrate_session_b(state, err) != FR_OK) return FR_ERR;
    if (state->blockers.count > 0) {
        fr_blockers_print(&state->blockers, stderr);
        *out_outcome = FR_MIGRATE_BLOCKED;
        return FR_OK;
    }
    return fr_migrate_finish(state, out_outcome, err);
}

int fr_migrate_run(const fr_migrate_options *options, fr_migrate_outcome *out_outcome,
                   fr_error *err) {
    fr_migrate_state state;
    memset(&state, 0, sizeof state);
    state.options = options;
    fr_cache_set_enabled(options->use_cache);
    *out_outcome = FR_MIGRATE_BLOCKED;
    int status = run_pipeline(&state, out_outcome, err);
    free_state(&state);
    return status;
}
