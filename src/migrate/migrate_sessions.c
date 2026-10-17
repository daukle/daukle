#include "migrate/migrate_internal.h"

#include "config/config_dispatch_migrate.h"
#include "config/config_lua.h"
#include "lua/lua_verbs.h"
#include "plugin/plugins.h"
#include "plugin/registry.h"
#include "plugin/resolvers.h"
#include "project/derived.h"
#include "project/sync.h"
#include "project/toolchain_root.h"
#include "util/error.h"

#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FR_MIGRATE_INSTANCE_SIZE 256
#define FR_MIGRATE_TEXT_SIZE 512

static int out_of_memory(fr_error *err, const char *doing) {
    fr_error_set(err, "out of memory %s", doing);
    return FR_ERR;
}

static cJSON *plugins_document(const fr_migrate_state *state, size_t count, fr_error *err) {
    cJSON *document = cJSON_CreateObject();
    cJSON *plugins = document != NULL ? cJSON_AddObjectToObject(document, "plugins") : NULL;
    if (plugins == NULL) {
        cJSON_Delete(document);
        out_of_memory(err, "building the plugins migrate loads");
        return NULL;
    }
    for (size_t index = 0; index < count; index++) {
        const fr_catalog_plugin *plugin = state->considered[index];
        if (plugin->asset_url == NULL) continue;
        cJSON *entry = cJSON_AddObjectToObject(plugins, plugin->id);
        if (entry == NULL || cJSON_AddStringToObject(entry, "url", plugin->asset_url) == NULL
            || cJSON_AddStringToObject(entry, "sha256", plugin->asset_sha256) == NULL) {
            cJSON_Delete(document);
            out_of_memory(err, "building the plugins migrate loads");
            return NULL;
        }
    }
    return document;
}

static void close_session(fr_registry *registry) {
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    fr_plugins_report_clear();
    fr_resolvers_clear();
}

/* daukle.manifest is "" for the length of the load: there is no manifest yet,
   and a chunk testing its name with string.match must not raise on nil. */
static int open_session(const fr_migrate_state *state, size_t count, fr_registry **out,
                        fr_error *err) {
    *out = NULL;
    cJSON *document = plugins_document(state, count, err);
    if (document == NULL) return FR_ERR;
    if (fr_build_registry(out, err) != FR_OK) {
        cJSON_Delete(document);
        return FR_ERR;
    }
    fr_lua_verbs_set_manifest_name("");
    int status = fr_plugins_load(*out, document, ".", err);
    fr_lua_verbs_set_manifest_name(NULL);
    cJSON_Delete(document);
    if (status != FR_OK) {
        close_session(*out);
        *out = NULL;
    }
    return status;
}

static int leaf_is_usable(const char *text) {
    if (!((text[0] >= 'a' && text[0] <= 'z') || (text[0] >= '0' && text[0] <= '9'))) return 0;
    for (const char *cursor = text; *cursor != '\0'; cursor++) {
        char c = *cursor;
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_'
              || c == '-')) {
            return 0;
        }
    }
    return 1;
}

static int instance_is_taken(const fr_migrate_state *state, const char *instance) {
    for (size_t index = 0; index < state->kept_count; index++) {
        if (strcmp(state->kept[index].instance, instance) == 0) return 1;
    }
    return 0;
}

/* The root instance keeps the toolchain's own name, so a single-toolchain
   project migrates to the manifest it would have been written as by hand. */
static int name_instance(fr_migrate_state *state, const char *toolchain, const char *dir,
                         char *out, fr_error *err) {
    int written = dir == NULL
        ? snprintf(out, FR_MIGRATE_INSTANCE_SIZE, "%s", toolchain)
        : snprintf(out, FR_MIGRATE_INSTANCE_SIZE, "%s-%s", toolchain, dir);
    for (char *cursor = out; *cursor != '\0'; cursor++) {
        if (*cursor == '/') *cursor = '-';
    }
    const char *where = dir != NULL ? dir : ".";
    char why[FR_MIGRATE_TEXT_SIZE];
    if (written < 0 || written >= FR_MIGRATE_INSTANCE_SIZE || !leaf_is_usable(out)) {
        snprintf(why, sizeof why, "the instance name \"%s\" is not a usable task name: it must be"
                                  " lowercase letters, digits, '.', '_' and '-'", out);
        return fr_migrate_core_blocker(state, "toolchain instance", where, why, err) == FR_OK
            ? 0 : -1;
    }
    if (instance_is_taken(state, out)) {
        snprintf(why, sizeof why, "the instance name \"%s\" is already another directory's", out);
        return fr_migrate_core_blocker(state, "toolchain instance", where, why, err) == FR_OK
            ? 0 : -1;
    }
    return 1;
}

static char *copy_or_null(const char *text, int *failed) {
    if (text == NULL) return NULL;
    char *copy = fr_dup_string(text);
    if (copy == NULL) *failed = 1;
    return copy;
}

static int keep_answer(fr_migrate_state *state, const fr_migrator_plugin *migrator,
                       const char *label, const char *instance, const char *dir, fr_error *err) {
    fr_migrate_kept *grown = realloc(state->kept, (state->kept_count + 1) * sizeof *grown);
    if (grown == NULL) return out_of_memory(err, "keeping a migrator's answer");
    state->kept = grown;
    fr_migrate_kept *kept = &state->kept[state->kept_count];
    memset(kept, 0, sizeof *kept);
    state->kept_count++;
    int failed = 0;
    kept->label = copy_or_null(label, &failed);
    kept->tool = copy_or_null(migrator->tool, &failed);
    kept->toolchain = copy_or_null(migrator->toolchain, &failed);
    kept->instance = copy_or_null(instance, &failed);
    kept->dir = copy_or_null(dir, &failed);
    if (failed) return out_of_memory(err, "keeping a migrator's answer");

    fr_toolchain toolchain;
    memset(&toolchain, 0, sizeof toolchain);
    toolchain.name = kept->instance;
    toolchain.plugin = kept->toolchain;
    toolchain.dir = kept->dir;
    if (fr_toolchain_root(&toolchain, &kept->root, err) != FR_OK) return FR_ERR;
    return fr_derived_dir_relative(kept->instance, &kept->derived_dir_relative, err);
}

static fr_migrate_call call_for(const fr_migrate_kept *kept, const cJSON *proof) {
    fr_migrate_call call = { ".", kept->tool, kept->instance, kept->dir, kept->root,
                             kept->derived_dir_relative, proof };
    return call;
}

static int run_migrator(fr_migrate_state *state, const fr_registry *registry,
                        const fr_detection *detection, fr_error *err) {
    const char *label = detection->plugin->id;
    const char *dir = detection->dir[0] == '\0' ? NULL : detection->dir;
    const fr_migrator_plugin *migrator = fr_registry_migrator_of(registry, label);
    if (migrator == NULL) {
        char why[FR_MIGRATE_TEXT_SIZE];
        snprintf(why, sizeof why, "%s matched this project but declares no migrator", label);
        return fr_migrate_core_blocker(state, "migrator", dir != NULL ? dir : ".", why, err);
    }
    char instance[FR_MIGRATE_INSTANCE_SIZE];
    int named = name_instance(state, migrator->toolchain, dir, instance, err);
    if (named <= 0) return named == 0 ? FR_OK : FR_ERR;
    if (keep_answer(state, migrator, label, instance, dir, err) != FR_OK) return FR_ERR;

    fr_migrate_kept *kept = &state->kept[state->kept_count - 1];
    fr_migrate_call call = call_for(kept, NULL);
    if (fr_lua_dispatch_migrator_run(migrator, &call, &kept->result, err) != FR_OK) return FR_ERR;
    return fr_migrate_check_run_result(kept->result, label, &state->blockers, err);
}

int fr_migrate_session_a(fr_migrate_state *state, fr_error *err) {
    fr_registry *registry = NULL;
    if (open_session(state, state->detected_count, &registry, err) != FR_OK) return FR_ERR;
    int status = FR_OK;
    for (size_t index = 0; status == FR_OK && index < state->detections.count; index++) {
        const fr_detection *detection = &state->detections.items[index];
        if (detection->plugin->asset_url == NULL) continue;
        status = run_migrator(state, registry, detection, err);
    }
    close_session(registry);
    return status;
}

static cJSON *foreign_object(const fr_migrate_kept *kept, const cJSON *foreign) {
    cJSON *object = cJSON_CreateObject();
    if (object == NULL) return NULL;
    const char *kind = cJSON_GetObjectItemCaseSensitive(foreign, "kind")->valuestring;
    const char *id = cJSON_GetObjectItemCaseSensitive(foreign, "id")->valuestring;
    int complete = cJSON_AddStringToObject(object, "tool", kept->tool) != NULL
        && cJSON_AddStringToObject(object, "kind", kind) != NULL
        && cJSON_AddStringToObject(object, "id", id) != NULL
        && cJSON_AddStringToObject(object, "instance", kept->instance) != NULL;
    const cJSON *config = cJSON_GetObjectItemCaseSensitive(foreign, "config");
    if (complete && config != NULL) {
        cJSON *copy = cJSON_Duplicate(config, 1);
        complete = copy != NULL && cJSON_AddItemToObject(object, "config", copy);
        if (!complete) cJSON_Delete(copy);
    }
    if (!complete) {
        cJSON_Delete(object);
        return NULL;
    }
    return object;
}

static int translate(fr_migrate_state *state, const fr_registry *registry,
                     fr_migrate_foreign *record, fr_error *err) {
    const fr_migrate_kept *kept = &state->kept[record->kept_index];
    const char *id = cJSON_GetObjectItemCaseSensitive(record->foreign, "id")->valuestring;
    char where[FR_MIGRATE_TEXT_SIZE];
    snprintf(where, sizeof where, "%s %s", kept->tool, id);
    const fr_translator_plugin *translator = fr_registry_translator(registry, kept->tool, id);
    if (translator == NULL) {
        char why[FR_MIGRATE_TEXT_SIZE];
        snprintf(why, sizeof why, "%s claims %s %s in its catalog but declares no translator for it",
                 record->replacer->id, kept->tool, id);
        return fr_migrate_core_blocker(state, "translator", where, why, err);
    }
    cJSON *foreign = foreign_object(kept, record->foreign);
    if (foreign == NULL) return out_of_memory(err, "building a translator's foreign plugin");
    int status = fr_lua_dispatch_translator(translator, foreign, &record->translation, err);
    cJSON_Delete(foreign);
    if (status != FR_OK) return FR_ERR;
    char source[FR_MIGRATE_TEXT_SIZE];
    snprintf(source, sizeof source, "translator %s/%s", kept->tool, id);
    return fr_migrate_check_translation(record->translation, source, &state->blockers, err);
}

static int build_old(const fr_registry *registry, const fr_migrate_kept *kept, fr_error *err) {
    const fr_migrator_plugin *migrator = fr_registry_migrator_of(registry, kept->label);
    if (migrator == NULL) {
        fr_error_set(err, "the old build of %s could not run: its migrator did not load again",
                     kept->label);
        return FR_ERR;
    }
    fr_migrate_call call = call_for(kept, cJSON_GetObjectItemCaseSensitive(kept->result, "proof"));
    if (fr_lua_dispatch_migrator_build(migrator, &call, err) == FR_OK) return FR_OK;
    char inner[sizeof err->message];
    memcpy(inner, err->message, sizeof inner);
    fr_error_set(err, "the old build of %s failed: %s", kept->label, inner);
    return FR_ERR;
}

static int translate_assemble_and_build(fr_migrate_state *state, const fr_registry *registry,
                                        fr_error *err) {
    for (size_t index = 0; index < state->foreign_count; index++) {
        fr_migrate_foreign *record = &state->foreign[index];
        if (record->replacer->asset_url == NULL) continue;
        if (translate(state, registry, record, err) != FR_OK) return FR_ERR;
    }
    if (fr_migrate_assemble(state, err) != FR_OK) return FR_ERR;
    if (state->blockers.count > 0) return FR_OK;
    for (size_t index = 0; index < state->kept_count; index++) {
        if (build_old(registry, &state->kept[index], err) != FR_OK) return FR_ERR;
    }
    return FR_OK;
}

int fr_migrate_session_b(fr_migrate_state *state, fr_error *err) {
    fr_registry *registry = NULL;
    if (open_session(state, state->considered_count, &registry, err) != FR_OK) return FR_ERR;
    int status = translate_assemble_and_build(state, registry, err);
    close_session(registry);
    return status;
}
