#include "plugin/plugins_internal.h"

#include "cJSON.h"
#include "config/config_lua.h"
#include "lua/lua_verbs.h"
#include "plugin/plugin_deps.h"
#include "plugin/plugin_modules.h"
#include "util/error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *origin;
    const char *resolved;
    const char *digest;
    fr_plugin_source *source;
} acquired_artifact;

/* Every alias the named modules declare, so an override is refused only when
   NONE of them declares its alias. */
typedef struct {
    char names[FR_PLUGIN_MAX_MODULES * FR_PLUGIN_MAX_REQUIRES][FR_PLUGIN_MAX_ALIAS];
    size_t count;
} declared_aliases;

static int alias_is_declared(const fr_plugin_declaration *declaration, const char *alias) {
    for (size_t index = 0; index < declaration->requires_count; index++) {
        if (strcmp(declaration->requires[index].alias, alias) == 0) return 1;
    }
    return 0;
}

/* One [plugins] line carries one requires table however many modules it names,
   so each module is handed the overrides for its own aliases only. NULL when
   the line names one chunk: then the table is passed whole and refused as
   before. The returned object holds references and is freed with cJSON_Delete. */
static cJSON *overrides_for_module(const cJSON *overrides,
                                   const fr_plugin_declaration *declaration) {
    cJSON *scoped = cJSON_CreateObject();
    if (scoped == NULL) return NULL;
    const cJSON *override = NULL;
    cJSON_ArrayForEach(override, overrides) {
        if (!alias_is_declared(declaration, override->string)) continue;
        cJSON_AddItemReferenceToObject(scoped, override->string, (cJSON *) override);
    }
    return scoped;
}

static void remember_aliases(declared_aliases *aliases, const fr_plugin_declaration *declaration) {
    for (size_t index = 0; index < declaration->requires_count; index++) {
        if (aliases->count == sizeof aliases->names / sizeof aliases->names[0]) return;
        snprintf(aliases->names[aliases->count++], FR_PLUGIN_MAX_ALIAS, "%s",
                 declaration->requires[index].alias);
    }
}

static int refuse_an_override_no_module_declares(const fr_plugin_entry *entry,
                                                 const declared_aliases *aliases,
                                                 fr_error *err) {
    const cJSON *override = NULL;
    cJSON_ArrayForEach(override, entry->overrides) {
        int declared = 0;
        for (size_t index = 0; index < aliases->count && !declared; index++) {
            declared = strcmp(aliases->names[index], override->string) == 0;
        }
        if (!declared) {
            fr_error_set(err, "plugin \"%s\": requires[\"%s\"] overrides an alias none of the"
                              " modules it names declares", entry->label, override->string);
            return FR_ERR;
        }
    }
    return FR_OK;
}

/* The source only knows its own files, so the [plugins] label that names
   the module is added here, where a reader can find the line to fix. */
static int named_module_entry(const fr_plugin_entry *entry, const acquired_artifact *artifact,
                              const char *module, const char **out_text, size_t *out_length,
                              fr_error *err) {
    if (fr_plugin_source_module_entry(artifact->source, module, out_text, out_length, err)
        == FR_OK) {
        return FR_OK;
    }
    char reason[sizeof err->message];
    snprintf(reason, sizeof reason, "%s", err->message);
    fr_error_set(err, "plugin \"%s\": %s", entry->label, reason);
    return FR_ERR;
}

/* One entry chunk of an acquired artifact, run with the grant its OWN
   declaration names. A module reports under "<label>/<module>", so config
   print shows one row per grant. */
static int load_chunk(const fr_plugin_entry *entry, const char *module,
                      const acquired_artifact *artifact, const fr_resolver_entry *resolvers,
                      size_t resolver_count, declared_aliases *aliases, fr_error *err) {
    const char *chunk = NULL;
    size_t chunk_length = 0;
    int status = module == NULL
        ? fr_plugin_source_entry(artifact->source, &chunk, &chunk_length, err)
        : named_module_entry(entry, artifact, module, &chunk, &chunk_length, err);
    if (status != FR_OK) return FR_ERR;

    int is_main = module == NULL || strcmp(module, FR_PLUGIN_MAIN_MODULE) == 0;
    char label[256];
    char origin[1024];
    snprintf(label, sizeof label, is_main ? "%s" : "%s/%s", entry->label, module);
    snprintf(origin, sizeof origin, is_main ? "%s" : "%s [modules/%s.lua]", artifact->origin,
             module);
    fr_plugin_entry reported = *entry;
    reported.label = label;

    lua_State *state = fr_lua_runtime_state();
    fr_plugin_declaration declaration;
    memset(&declaration, 0, sizeof declaration);
    declaration.kind = "plugin";
    declaration.label = label;
    status = fr_plugins_declaration_read(state, chunk, chunk_length, origin, &declaration, err);

    fr_plugin_deps *deps = NULL;
    if (status == FR_OK) {
        cJSON *scoped = aliases != NULL && entry->overrides != NULL
            ? overrides_for_module(entry->overrides, &declaration) : NULL;
        status = fr_plugin_deps_acquire(&declaration, scoped != NULL ? scoped : entry->overrides,
                                        resolvers, resolver_count, &deps, err);
        cJSON_Delete(scoped);
    }
    if (status == FR_OK) {
        fr_lua_verbs_set_declared_env((const char *const *) declaration.env,
                                      declaration.env_count);
        fr_lua_declare_set_required_aliases(declaration.requires, declaration.requires_count);
        status = fr_lua_plugin_load(chunk, chunk_length, origin,
                                    (const char *const *) declaration.uses,
                                    declaration.uses_count, artifact->source, deps, err);
        fr_lua_declare_set_required_aliases(NULL, 0);
        fr_lua_verbs_set_declared_env(NULL, 0);
    }
    if (status == FR_OK) {
        status = fr_plugins_report_append(&reported, artifact->origin, artifact->resolved,
                                          artifact->digest, &declaration, err);
    }
    if (status == FR_OK) status = fr_plugins_report_append_dependencies(deps, err);

    fr_plugin_deps_close(deps);
    fr_plugins_free_declaration(&declaration);
    return status;
}

/* Reads each named module's declaration before any of them loads, so a
   misspelt override is refused by name rather than surfacing as a fetch of
   the url it was meant to replace. */
static int collect_declared_aliases(const fr_plugin_entry *entry,
                                    const acquired_artifact *artifact,
                                    declared_aliases *aliases, fr_error *err) {
    for (size_t index = 0; index < entry->module_count; index++) {
        const char *chunk = NULL;
        size_t chunk_length = 0;
        if (named_module_entry(entry, artifact, entry->modules[index], &chunk, &chunk_length, err)
            != FR_OK) {
            return FR_ERR;
        }
        fr_plugin_declaration declaration;
        memset(&declaration, 0, sizeof declaration);
        declaration.kind = "plugin";
        declaration.label = entry->label;
        int status = fr_plugins_declaration_read(fr_lua_runtime_state(), chunk, chunk_length,
                                                 artifact->origin, &declaration, err);
        if (status == FR_OK) remember_aliases(aliases, &declaration);
        fr_plugins_free_declaration(&declaration);
        if (status != FR_OK) return FR_ERR;
    }
    return refuse_an_override_no_module_declares(entry, aliases, err);
}

int fr_plugins_load_entry(const fr_plugin_entry *entry, const fr_resolver_entry *resolvers,
                          size_t resolver_count, fr_error *err) {
    char *origin = NULL;
    char *text = NULL;
    char *resolved = NULL;
    size_t length = 0;
    char digest[65];
    fr_plugin_source *source = NULL;
    if (fr_plugins_acquire_source(entry, resolvers, resolver_count, &text, &length, &origin,
                                  &resolved, digest, &source, err)
        != FR_OK) {
        return FR_ERR;
    }

    acquired_artifact artifact = { origin, resolved, digest, source };
    declared_aliases *aliases = entry->module_count > 1 ? calloc(1, sizeof *aliases) : NULL;
    int status = FR_OK;
    if (entry->module_count > 1 && aliases == NULL) {
        fr_error_set(err, "plugin \"%s\": out of memory", entry->label);
        status = FR_ERR;
    }
    if (status == FR_OK && aliases != NULL && entry->overrides != NULL) {
        status = collect_declared_aliases(entry, &artifact, aliases, err);
    }
    if (status == FR_OK && entry->module_count == 0) {
        status = load_chunk(entry, NULL, &artifact, resolvers, resolver_count, NULL, err);
    }
    for (size_t index = 0; status == FR_OK && index < entry->module_count; index++) {
        status = load_chunk(entry, entry->modules[index], &artifact, resolvers, resolver_count,
                            aliases, err);
    }

    free(aliases);
    fr_plugin_source_close(source);
    free(text);
    free(origin);
    free(resolved);
    return status;
}
