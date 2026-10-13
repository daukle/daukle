#include "cli/cli_internal.h"

#include "config/config.h"
#include "config/config_lua.h"
#include "plugin/plugins.h"
#include "plugin/registry.h"
#include "plugin/resolvers.h"
#include "project/region.h"
#include "project/sync.h"

#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The document outlives the entries parsed from it because
   fr_resolver_entry.block borrows from it, and a resolver reached after it was
   freed would read freed memory. */
typedef struct {
    cJSON *document;
    fr_plugin_entry *entries;
    size_t count;
    fr_resolver_entry *resolvers;
    size_t resolver_count;
} manifest_plugins;

static void manifest_plugins_free(manifest_plugins *plugins) {
    fr_plugins_free(plugins->entries, plugins->count);
    fr_resolvers_free(plugins->resolvers, plugins->resolver_count);
    cJSON_Delete(plugins->document);
    memset(plugins, 0, sizeof *plugins);
}

/* Reads only the manifest's own document, never fr_config_load_file: that also
   runs fr_plugins_load, which resolves and executes every declared plugin, the
   opposite of what "plugin update" wants when a plugin's current cache is what
   it is trying to discard. Reading the document is all this needs, and it is
   what keeps a lua manifest readable here: a chunk declaring a plugin only
   registers it, so nothing is fetched from the cache being cleared.
   base_dir and registry come from the caller because the runtime is already
   open on them by the time this runs; a lua format reopens it through
   fr_lua_runtime_begin, which serves one base directory and one registry and
   refuses anything else. Passing the NULL registry this used to pass fails
   with "another registry's plugins are still registered". */
static int read_manifest_plugins(const char *manifest_path, const char *base_dir,
                                 fr_registry *registry, manifest_plugins *out, fr_error *err) {
    memset(out, 0, sizeof *out);

    const fr_config_plugin *plugin = fr_config_plugin_for(registry, manifest_path, err);
    if (plugin == NULL) return FR_ERR;

    char *text = NULL;
    if (fr_file_read_text(manifest_path, &text, err) != FR_OK) return FR_ERR;

    int status = plugin->load(plugin->state, text, manifest_path, base_dir, registry, NULL,
                              &out->document, err);
    free(text);
    if (status != FR_OK) return FR_ERR;

    if (fr_resolvers_parse(out->document, &out->resolvers, &out->resolver_count, err) != FR_OK
        || fr_plugins_parse(out->document, out->resolvers, out->resolver_count, &out->entries,
                            &out->count, err) != FR_OK) {
        manifest_plugins_free(out);
        return FR_ERR;
    }
    return FR_OK;
}

/* "update which plugins?" has no answer without a manifest in scope, so every
   call, labelled or not, starts by finding and reading one the same way the
   other subcommands do. Scoping to this manifest's own [plugins] table (rather
   than the whole, per-user, cross-project cache root) matters: without it, a
   label-less update in one project would silently discard every other
   project's cached plugins too. The scoping rule itself lives in
   fr_plugins_update_cache, shared with, and covered directly by, test_plugins.c,
   since main.c has no test binary of its own.
   A lua runtime is opened here rather than inside the calls that need one:
   fr_plugins_update_cache re-resolves an FR_PLUGIN_RESOLVED entry through
   fr_resolvers_use, a lua-format manifest is executed to be read at all, and
   only this function knows the manifest's own directory that both resolve
   against. It is opened BEFORE the manifest is read because of that second
   reason: a lua format's own reopen must be handed this same registry, and
   the runtime refuses any other. */
int fr_cli_plugin_update(const char *label, int verbose) {
    fr_error err;

    char *resolved = NULL;
    if (fr_cli_resolve_manifest_path(NULL, &resolved, &err) != FR_OK) {
        fr_cli_report_error(&err, verbose);
        return 1;
    }

    char *directory = fr_cli_manifest_directory(resolved);
    if (directory == NULL) {
        free(resolved);
        fprintf(stderr, "daukle: out of memory finding the manifest directory\n");
        return 1;
    }

    fr_registry *registry = NULL;
    int status = fr_build_registry(&registry, &err);
    if (status == FR_OK) status = fr_lua_runtime_begin(directory, registry, &err);
    if (status != FR_OK) {
        if (registry != NULL) fr_registry_destroy(registry);
        free(directory);
        free(resolved);
        fr_cli_report_error(&err, verbose);
        return 1;
    }

    manifest_plugins plugins;
    status = read_manifest_plugins(resolved, directory, registry, &plugins, &err);
    free(resolved);
    free(directory);
    if (status != FR_OK) {
        fr_lua_runtime_shutdown();
        fr_resolvers_clear();
        fr_registry_destroy(registry);
        fr_cli_report_error(&err, verbose);
        return 1;
    }

    size_t removed_count = 0;
    status = fr_plugins_update_cache(plugins.entries, plugins.count, plugins.resolvers,
                                     plugins.resolver_count, label, &removed_count, &err);

    fr_lua_runtime_shutdown();
    fr_resolvers_clear();
    fr_registry_destroy(registry);
    manifest_plugins_free(&plugins);
    if (status != FR_OK) {
        fr_cli_report_error(&err, verbose);
        return 1;
    }

    if (label == NULL) {
        if (removed_count == 0) {
            printf("daukle: no plugin here has a cache to clear\n");
        } else {
            printf("daukle: cleared the cache for %zu plugin%s\n", removed_count,
                   removed_count == 1 ? "" : "s");
        }
    } else if (removed_count == 0) {
        printf("daukle: plugin \"%s\" has no fetched artifact to clear\n", label);
    } else {
        printf("daukle: cleared the cache for \"%s\"\n", label);
    }
    return 0;
}
