#include "cli/cli_internal.h"

#include "cache/cache.h"
#include "config/config.h"
#include "config/config_lua.h"
#include "config/manifest.h"
#include "exec/toolreport.h"
#include "plugin/plugins.h"
#include "plugin/registry.h"
#include "plugin/resolvers.h"
#include "project/sync.h"

#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>

/* What the configuration actually depended on, so a future lockfile has a
   record of it without a second pass over the config surface. */
static void print_env_reads(const cJSON *env_reads) {
    if (env_reads == NULL || env_reads->child == NULL) return;
    printf("daukle: environment reads\n");
    const cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, env_reads) {
        if (cJSON_IsString(entry)) printf("  %s = %s\n", entry->string, entry->valuestring);
    }
}

/* Printed after the manifest itself, exactly what fr_plugins_load recorded on
   this run: label, resolved version or local location, declared verbs and
   digest, so adopting a pin is a copy of the printed line's sha256, not a
   separate lookup. Reads fr_plugins_report before it is cleared, never after. */
static void print_plugin_report(void) {
    const fr_plugin_report *report = fr_plugins_report();
    if (report->count == 0 && report->unused_resolver_count == 0) return;

    printf("daukle: plugins\n");
    for (size_t index = 0; index < report->count; index++) {
        const fr_plugin_report_entry *entry = &report->entries[index];
        if (entry->required_by != NULL) {
            printf("  %s: %s, sha256 %s, required by %s as %s", entry->label, entry->url,
                   entry->sha256, entry->required_by, entry->alias);
        } else if (entry->kind == FR_PLUGIN_RESOLVED) {
            printf("  %s: %s via %s, %s, sha256 %s", entry->label, entry->resolved,
                   entry->resolver, entry->url, entry->sha256);
        } else {
            printf("  %s: %s, sha256 %s", entry->label, entry->resolved, entry->sha256);
        }
        if (entry->uses_count == 0) {
            printf(", uses none");
        } else {
            printf(", uses ");
            for (size_t use_index = 0; use_index < entry->uses_count; use_index++) {
                printf("%s%s", use_index == 0 ? "" : ",", entry->uses[use_index]);
            }
        }
        if (entry->overridden) printf(", overridden");
        printf("\n");
    }

    for (size_t index = 0; index < report->unused_resolver_count; index++) {
        printf("  resolver %s: declared, unused\n", report->unused_resolvers[index]);
    }
}

/* Mirrors print_plugin_report's shape so the two reports read as one
   program's output: a "daukle: " header, then two-space-indented rows. The
   row table is toolreport.c's, filled as a toolchain plugin provisions or
   reuses a tool during this run. An installed row's url field actually holds
   its path (see fr_toolreport_used_installed) and carries no digest, so it
   is rendered without one rather than beside a blank "sha256 " that would
   read as a provisioned row missing its pin. */
static void print_toolreport(void) {
    size_t count = fr_toolreport_row_count();
    if (count == 0) return;

    printf("daukle: tools\n");
    for (size_t index = 0; index < count; index++) {
        const fr_toolreport_row *row = fr_toolreport_row_at(index);
        if (row->provisioned) {
            printf("  %s: %s, sha256 %s\n", row->label, row->url, row->digest);
        } else {
            printf("  %s: %s\n", row->label, row->url);
        }
    }
}

int fr_cli_print_config(const char *manifest_path, int use_cache, int verbose) {
    fr_error err;
    fr_cache_set_enabled(use_cache);

    char *resolved = NULL;
    if (fr_cli_resolve_manifest_path(manifest_path, &resolved, &err) != FR_OK) {
        fr_cli_report_error(&err, verbose);
        return 1;
    }

    fr_registry *registry = NULL;
    if (fr_build_registry(&registry, &err) != FR_OK) {
        fr_cli_report_error(&err, verbose);
        free(resolved);
        return 1;
    }

    fr_manifest manifest;
    int status = fr_config_load_file(resolved, registry, &manifest, &err);
    /* The runtime owns the recorded reads and the shutdown below frees them. */
    cJSON *env_reads = cJSON_Duplicate(fr_lua_env_reads(), 1);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();

    if (status != FR_OK) {
        fr_cli_report_error(&err, verbose);
        cJSON_Delete(env_reads);
        fr_manifest_free(&manifest);
        free(resolved);
        fr_plugins_report_clear();
        fr_resolvers_clear();
        return 1;
    }

    char *printed = cJSON_Print(manifest.document);
    if (printed == NULL) {
        fprintf(stderr, "daukle: out of memory printing \"%s\"\n", resolved);
        cJSON_Delete(env_reads);
        fr_manifest_free(&manifest);
        free(resolved);
        fr_plugins_report_clear();
        fr_resolvers_clear();
        return 1;
    }

    printf("%s\n", printed);
    free(printed);
    print_env_reads(env_reads);
    cJSON_Delete(env_reads);
    print_plugin_report();
    print_toolreport();
    fr_plugins_report_clear();
    fr_resolvers_clear();
    fr_manifest_free(&manifest);
    free(resolved);
    return 0;
}
