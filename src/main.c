#include "cache.h"
#include "cli.h"
#include "config.h"
#include "config_lua.h"
#include "derived.h"
#include "error.h"
#include "lua_verbs.h"
#include "luax.h"
#include "manifest.h"
#include "plugins.h"
#include "region.h"
#include "registry.h"
#include "sync.h"
#include "tasks.h"
#include "tomledit.h"

#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DAUKLE_VERSION "0.1.0"

static void print_lua_log(const char *message) {
    fprintf(stderr, "daukle: %s\n", message);
}

static void print_notice(const char *message) {
    fprintf(stderr, "daukle: %s\n", message);
}

static void report_error(const fr_error *err, int verbose) {
    fprintf(stderr, "daukle: %s\n", err->message);
    if (verbose) {
        const char *traceback = fr_lua_last_traceback();
        if (traceback != NULL) fprintf(stderr, "%s\n", traceback);
    }
}

static char *duplicate_string(const char *text) {
    size_t length = strlen(text) + 1;
    char *copy = malloc(length);
    if (copy != NULL) memcpy(copy, text, length);
    return copy;
}

/* sync.c keeps its own copy of this same small split rather than exposing one. */
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

/* A NULL manifest_path means "search the current directory"; a registry is
   built only for that search and torn down again before returning, per the
   rule that every registry a path builds is destroyed and shut down there. */
static int resolve_manifest_path(const char *manifest_path, char **out_path, fr_error *err) {
    if (manifest_path != NULL) {
        *out_path = duplicate_string(manifest_path);
        if (*out_path == NULL) {
            fr_error_set(err, "out of memory copying the manifest path");
            return FR_ERR;
        }
        return FR_OK;
    }

    fr_registry *registry = NULL;
    if (fr_build_registry(&registry, err) != FR_OK) return FR_ERR;
    int status = fr_config_find(".", registry, out_path, err);
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();
    fr_plugins_report_clear();
    return status;
}

static int run(const char *manifest_path, int write, int use_cache, int verbose) {
    fr_error err;
    fr_sync_report report;
    int status = 0;
    if (fr_sync(manifest_path, write, use_cache, &report, &err) != FR_OK) {
        report_error(&err, verbose);
        if (write && report.count > 0) {
            fprintf(stderr, "daukle: %zu file%s updated before the failure\n",
                    report.count, report.count == 1 ? "" : "s");
        }
        status = 1;
    } else if (write) {
        printf("daukle: %s\n", report.count > 0 ? "updated" : "already in sync");
    } else if (report.count > 0) {
        for (size_t index = 0; index < report.count; index++) {
            fprintf(stderr, "daukle: %s is out of date\n", report.files[index]);
        }
        fprintf(stderr, "daukle: run \"daukle sync\"\n");
        status = 1;
    } else {
        printf("daukle: in sync\n");
    }
    fr_sync_report_free(&report);
    return status;
}

static int run_with_resolved_manifest(const char *manifest_path, int write, int use_cache, int verbose) {
    fr_error err;
    char *resolved = NULL;
    if (resolve_manifest_path(manifest_path, &resolved, &err) != FR_OK) {
        report_error(&err, verbose);
        return 1;
    }
    int status = run(resolved, write, use_cache, verbose);
    free(resolved);
    return status;
}

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
    if (report->count == 0) return;

    printf("daukle: plugins\n");
    for (size_t index = 0; index < report->count; index++) {
        const fr_plugin_report_entry *entry = &report->entries[index];
        printf("  %s: %s, sha256 %s", entry->label, entry->resolved, entry->sha256);
        if (entry->uses_count == 0) {
            printf(", uses none\n");
            continue;
        }
        printf(", uses ");
        for (size_t use_index = 0; use_index < entry->uses_count; use_index++) {
            printf("%s%s", use_index == 0 ? "" : ",", entry->uses[use_index]);
        }
        printf("\n");
    }
}

static int print_config(const char *manifest_path, int use_cache, int verbose) {
    fr_error err;
    fr_cache_set_enabled(use_cache);

    char *resolved = NULL;
    if (resolve_manifest_path(manifest_path, &resolved, &err) != FR_OK) {
        report_error(&err, verbose);
        return 1;
    }

    fr_registry *registry = NULL;
    if (fr_build_registry(&registry, &err) != FR_OK) {
        report_error(&err, verbose);
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
        report_error(&err, verbose);
        cJSON_Delete(env_reads);
        fr_manifest_free(&manifest);
        free(resolved);
        fr_plugins_report_clear();
        return 1;
    }

    char *printed = cJSON_Print(manifest.document);
    if (printed == NULL) {
        fprintf(stderr, "daukle: out of memory printing \"%s\"\n", resolved);
        cJSON_Delete(env_reads);
        fr_manifest_free(&manifest);
        free(resolved);
        fr_plugins_report_clear();
        return 1;
    }

    printf("%s\n", printed);
    free(printed);
    print_env_reads(env_reads);
    cJSON_Delete(env_reads);
    print_plugin_report();
    fr_plugins_report_clear();
    fr_manifest_free(&manifest);
    free(resolved);
    return 0;
}

static int ends_with(const char *text, const char *suffix) {
    size_t text_length = strlen(text);
    size_t suffix_length = strlen(suffix);
    if (suffix_length > text_length) return 0;
    return strcmp(text + (text_length - suffix_length), suffix) == 0;
}

/* Splits "project@range" in a caller-owned buffer rather than in argv: argv
   entries are not guaranteed to be writable (test doubles pass string
   literals), and manifest_path/add_spec are const char * besides. */
static int split_add_spec(const char *spec, char *buffer, size_t buffer_size,
                          const char **project, const char **range, fr_error *err) {
    size_t length = strlen(spec);
    if (length >= buffer_size) {
        fr_error_set(err, "\"%s\" is too long", spec);
        return FR_ERR;
    }
    memcpy(buffer, spec, length + 1);
    char *at = strchr(buffer, '@');
    if (at == NULL) {
        fr_error_set(err, "\"%s\" needs a @range, for example \"%s@^1.0.0\"", spec, spec);
        return FR_ERR;
    }
    *at = '\0';
    *project = buffer;
    *range = at + 1;
    return FR_OK;
}

/* modules[0] is the head of the single allocation backing every entry, so
   freeing it and then the array releases everything split_modules built. */
static void free_modules(char **modules) {
    if (modules == NULL) return;
    free(modules[0]);
    free(modules);
}

static int split_modules(const char *modules_arg, char ***out_modules, size_t *out_count, fr_error *err) {
    *out_modules = NULL;
    *out_count = 0;
    if (modules_arg == NULL) return FR_OK;

    char *copy = duplicate_string(modules_arg);
    if (copy == NULL) {
        fr_error_set(err, "out of memory splitting the module list");
        return FR_ERR;
    }

    size_t count = 1;
    for (const char *cursor = copy; *cursor != '\0'; cursor++) {
        if (*cursor == ',') count++;
    }

    char **modules = malloc(count * sizeof *modules);
    if (modules == NULL) {
        free(copy);
        fr_error_set(err, "out of memory splitting the module list");
        return FR_ERR;
    }

    size_t position = 0;
    modules[position++] = copy;
    for (char *cursor = copy; *cursor != '\0'; cursor++) {
        if (*cursor == ',') {
            *cursor = '\0';
            modules[position++] = cursor + 1;
        }
    }

    for (size_t index = 0; index < count; index++) {
        if (modules[index][0] != '\0') continue;
        free_modules(modules);
        fr_error_set(err, "\"%s\" has an empty module name; list them as \"a,b\"", modules_arg);
        return FR_ERR;
    }

    *out_modules = modules;
    *out_count = count;
    return FR_OK;
}

static int add_dependency(const fr_cli_options *options) {
    fr_error err;
    char *resolved = NULL;
    if (resolve_manifest_path(options->manifest_path, &resolved, &err) != FR_OK) {
        report_error(&err, options->verbose);
        return 1;
    }

    if (!ends_with(resolved, ".toml")) {
        fprintf(stderr, "daukle: add edits daukle.toml; this project uses \"%s\"\n", resolved);
        free(resolved);
        return 1;
    }

    char spec_buffer[512];
    const char *project = NULL;
    const char *range = NULL;
    if (split_add_spec(options->add_spec, spec_buffer, sizeof spec_buffer, &project, &range, &err) != FR_OK) {
        report_error(&err, options->verbose);
        free(resolved);
        return 1;
    }

    char **modules = NULL;
    size_t module_count = 0;
    if (split_modules(options->add_modules, &modules, &module_count, &err) != FR_OK) {
        report_error(&err, options->verbose);
        free(resolved);
        return 1;
    }

    char *original_text = NULL;
    if (fr_file_read_text(resolved, &original_text, &err) != FR_OK) {
        report_error(&err, options->verbose);
        free_modules(modules);
        free(resolved);
        return 1;
    }

    char *updated_text = NULL;
    int status = fr_toml_edit_set_dependency(original_text, options->add_consumer, project, range,
                                             (const char *const *) modules, module_count,
                                             &updated_text, &err);
    free(original_text);
    free_modules(modules);

    if (status != FR_OK) {
        report_error(&err, options->verbose);
        free(resolved);
        return 1;
    }

    status = fr_file_write_text(resolved, updated_text, &err);
    free(updated_text);
    if (status != FR_OK) {
        report_error(&err, options->verbose);
        free(resolved);
        return 1;
    }

    printf("daukle: added %s@%s to %s\n", project, range, options->add_consumer);
    free(resolved);
    return 0;
}

/* Reads only the manifest's own document, never fr_config_load_file: that also
   runs fr_plugins_load, which resolves and executes every declared plugin, the
   opposite of what "plugin update" wants when a plugin's current cache is what
   it is trying to discard. The load call therefore passes no registry, and an
   overlay format is refused outright, since executing one is precisely what
   dispatching on its extension would do. */
static int read_manifest_plugins(const char *manifest_path, fr_plugin_entry **out_entries,
                                 size_t *out_count, fr_error *err) {
    fr_registry *registry = NULL;
    if (fr_build_registry(&registry, err) != FR_OK) return FR_ERR;

    const fr_config_plugin *plugin = fr_config_plugin_for(registry, manifest_path, err);
    if (plugin == NULL) {
        fr_registry_destroy(registry);
        return FR_ERR;
    }
    if (plugin->overlay) {
        fr_error_set(err, "\"%s\" is an overlay format; it cannot be read on its own",
                     manifest_path);
        fr_registry_destroy(registry);
        return FR_ERR;
    }

    char *text = NULL;
    if (fr_file_read_text(manifest_path, &text, err) != FR_OK) {
        fr_registry_destroy(registry);
        return FR_ERR;
    }

    cJSON *document = NULL;
    int status = plugin->load(plugin->state, text, manifest_path, ".", NULL, NULL, &document, err);
    free(text);
    fr_registry_destroy(registry);
    if (status != FR_OK) return FR_ERR;

    status = fr_plugins_parse(document, out_entries, out_count, err);
    cJSON_Delete(document);
    return status;
}

/* "update which plugins?" has no answer without a manifest in scope, so every
   call, labelled or not, starts by finding and reading one the same way the
   other subcommands do. Scoping to this manifest's own [plugins] table (rather
   than the whole, per-user, cross-project cache root) matters: without it, a
   label-less update in one project would silently discard every other
   project's cached plugins too. The scoping rule itself lives in
   fr_plugins_update_cache, shared with, and covered directly by, test_plugins.c,
   since main.c has no test binary of its own. */
static int plugin_update(const char *label, int use_cache, int verbose) {
    fr_error err;
    fr_cache_set_enabled(use_cache);

    char *resolved = NULL;
    if (resolve_manifest_path(NULL, &resolved, &err) != FR_OK) {
        report_error(&err, verbose);
        return 1;
    }

    fr_plugin_entry *entries = NULL;
    size_t count = 0;
    int status = read_manifest_plugins(resolved, &entries, &count, &err);
    free(resolved);
    if (status != FR_OK) {
        report_error(&err, verbose);
        return 1;
    }

    size_t removed_count = 0;
    status = fr_plugins_update_cache(entries, count, label, &removed_count, &err);
    fr_plugins_free(entries, count);
    if (status != FR_OK) {
        report_error(&err, verbose);
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
        printf("daukle: plugin \"%s\" loads from a local file; it has no cache to clear\n", label);
    } else {
        printf("daukle: cleared the cache for \"%s\"\n", label);
    }
    return 0;
}

static int clean_derived(const char *manifest_path, int verbose) {
    fr_error err;
    char *resolved = NULL;
    if (resolve_manifest_path(manifest_path, &resolved, &err) != FR_OK) {
        report_error(&err, verbose);
        return 1;
    }

    char *directory = manifest_directory(resolved);
    free(resolved);
    if (directory == NULL) {
        fprintf(stderr, "daukle: out of memory finding the manifest directory\n");
        return 1;
    }

    int status = fr_derived_clean(directory, &err);
    free(directory);
    if (status != FR_OK) {
        report_error(&err, verbose);
        return 1;
    }

    printf("daukle: cleaned\n");
    return 0;
}

static int run_task(const char *task_name, int use_cache, int verbose) {
    fr_error err;
    char *resolved = NULL;
    if (resolve_manifest_path(NULL, &resolved, &err) != FR_OK) {
        report_error(&err, verbose);
        return 1;
    }

    fr_session session;
    int opened = fr_session_open(resolved, use_cache, &session, &err);
    free(resolved);
    if (opened != FR_OK) {
        report_error(&err, verbose);
        return 1;
    }

    fr_task_set set;
    if (fr_tasks_collect(session.registry, &session.manifest, &set, &err) != FR_OK) {
        fr_session_close(&session);
        report_error(&err, verbose);
        return 1;
    }

    fr_task_plan plan;
    if (fr_tasks_plan(&set, task_name, &plan, &err) != FR_OK) {
        int unknown = fr_tasks_find(&set, task_name) == NULL;
        size_t plugin_count = fr_plugins_report()->count;
        fr_tasks_set_free(&set);
        fr_session_close(&session);
        report_error(&err, verbose);
        if (unknown) {
            if (plugin_count == 0) {
                fprintf(stderr, "  this project declares no plugins, so it has no tasks\n");
            } else {
                fprintf(stderr, "  tasks come from plugins; this project declares %zu\n", plugin_count);
            }
            fprintf(stderr, "  run \"daukle tasks\" to see what they provide\n");
            return 2;
        }
        return 1;
    }

    /* The plan is known good before anything is written: a task that does not
       exist must never trigger the write a real one would have caused. */
    fr_sync_report report;
    if (fr_sync_session(&session, 1, &report, &err) != FR_OK) {
        fr_sync_report_free(&report);
        fr_tasks_plan_free(&plan);
        fr_tasks_set_free(&set);
        fr_session_close(&session);
        report_error(&err, verbose);
        return 1;
    }
    fr_sync_report_free(&report);

    int status = fr_tasks_run(&plan, &session, &err);
    fr_tasks_plan_free(&plan);
    fr_tasks_set_free(&set);
    fr_session_close(&session);
    if (status != FR_OK) {
        report_error(&err, verbose);
        return 1;
    }
    printf("daukle: %s\n", task_name);
    return 0;
}

#define FR_TASKS_JOINER_CAPACITY 32

/* label reads in the direction that kind actually runs: FR_TASK_JOIN_PART_OF
   is what name pulls in (they run before name), FR_TASK_JOIN_DEPENDS_ON is
   what pulls name in (name runs before them). Printing the true total past
   FR_TASKS_JOINER_CAPACITY, rather than staying silent about it, is what
   keeps a truncated list from being mistaken for a complete one. */
static void print_task_joiners(const fr_task_set *set, const char *name,
                               fr_task_join_kind kind, const char *label) {
    const char *joiners[FR_TASKS_JOINER_CAPACITY];
    size_t total = fr_tasks_joiners(set, name, kind, joiners, FR_TASKS_JOINER_CAPACITY);
    size_t shown = total < FR_TASKS_JOINER_CAPACITY ? total : FR_TASKS_JOINER_CAPACITY;
    for (size_t index = 0; index < shown; index++) {
        printf("  %s: %s\n", label, joiners[index]);
    }
    if (total > shown) {
        printf("  ... and %zu more not shown\n", total - shown);
    }
}

static int list_tasks(int use_cache, int verbose) {
    fr_error err;
    char *resolved = NULL;
    if (resolve_manifest_path(NULL, &resolved, &err) != FR_OK) {
        report_error(&err, verbose);
        return 1;
    }
    fr_session session;
    int opened = fr_session_open(resolved, use_cache, &session, &err);
    free(resolved);
    if (opened != FR_OK) {
        report_error(&err, verbose);
        return 1;
    }

    fr_task_set set;
    if (fr_tasks_collect(session.registry, &session.manifest, &set, &err) != FR_OK) {
        fr_session_close(&session);
        report_error(&err, verbose);
        return 1;
    }

    if (set.count == 0) {
        printf("daukle: this project has no tasks\n");
    } else {
        printf("daukle: %zu task%s\n", set.count, set.count == 1 ? "" : "s");
    }
    for (size_t index = 0; index < set.count; index++) {
        if (index > 0) printf("\n");
        const fr_task_node *node = &set.nodes[index];
        printf("%s%s\n", node->name, node->plugin == NULL ? " (from the manifest)" : "");
        printf("  runs: %s\n", (node->plugin != NULL && node->plugin->run != NULL)
                                   ? "a program" : "nothing of its own");
        for (size_t edge = 0; edge < node->depends_on_count; edge++) {
            printf("  after: %s\n", node->depends_on[edge]);
        }
        for (size_t edge = 0; edge < node->extra_depends_on_count; edge++) {
            printf("  after: %s (from the manifest)\n", node->extra_depends_on[edge]);
        }
        if (node->part_of != NULL) printf("  part of: %s\n", node->part_of);
        if (node->extra_part_of != NULL) printf("  part of: %s (from the manifest)\n", node->extra_part_of);

        print_task_joiners(&set, node->name, FR_TASK_JOIN_PART_OF, "pulls in");
        print_task_joiners(&set, node->name, FR_TASK_JOIN_DEPENDS_ON, "needed by");
    }

    fr_tasks_set_free(&set);
    fr_session_close(&session);
    return 0;
}

int main(int argc, char **argv) {
    fr_cli_options options;
    fr_cli_parse(argc, argv, &options);
    fr_lua_set_log_sink(print_lua_log);
    fr_derived_set_notice_sink(print_notice);
    fr_lua_set_limits(options.instruction_limit, options.memory_limit);
    fr_lua_verbs_set_verbose(options.verbose);

    switch (options.command) {
        case FR_CLI_VERSION:
            printf("daukle %s\n", DAUKLE_VERSION);
            return 0;
        case FR_CLI_SYNC:
            return run_with_resolved_manifest(options.manifest_path, 1, options.use_cache, options.verbose);
        case FR_CLI_CHECK:
            return run_with_resolved_manifest(options.manifest_path, 0, options.use_cache, options.verbose);
        case FR_CLI_CONFIG_PRINT:
            return print_config(options.manifest_path, options.use_cache, options.verbose);
        case FR_CLI_ADD:
            return add_dependency(&options);
        case FR_CLI_PLUGIN_UPDATE:
            return plugin_update(options.plugin_label, options.use_cache, options.verbose);
        case FR_CLI_CLEAN:
            return clean_derived(options.manifest_path, options.verbose);
        case FR_CLI_TASK:
            return run_task(options.task_name, options.use_cache, options.verbose);
        case FR_CLI_TASKS:
            return list_tasks(options.use_cache, options.verbose);
        case FR_CLI_USAGE:
            break;
    }

    if (options.plugin_unknown_subcommand != NULL) {
        fprintf(stderr, "daukle: unknown plugin subcommand \"%s\"\n", options.plugin_unknown_subcommand);
        return 2;
    }

    fprintf(stderr, "usage: daukle [--version | sync [manifest] | check [manifest]"
                    " | add <project>@<range> --to <consumer> [--modules a,b]"
                    " | config print | plugin update [label] | clean [manifest]"
                    " | tasks | <task>]"
                    " [--no-cache] [--verbose]\n");
    return 2;
}
