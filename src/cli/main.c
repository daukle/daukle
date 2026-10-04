#include "cache/cache.h"
#include "cli/cli.h"
#include "config/config.h"
#include "config/config_lua.h"
#include "config/manifest.h"
#include "config/tomledit.h"
#include "exec/toolreport.h"
#include "lua/lua_verbs.h"
#include "lua/luax.h"
#include "plugin/plugins.h"
#include "plugin/registry.h"
#include "plugin/resolvers.h"
#include "project/derived.h"
#include "project/region.h"
#include "project/sync.h"
#include "project/tasks.h"
#include "util/error.h"
#include "util/version.h"

#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif

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
        *out_path = fr_dup_string(manifest_path);
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
    fr_resolvers_clear();
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

    char *copy = fr_dup_string(modules_arg);
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
static int plugin_update(const char *label, int verbose) {
    fr_error err;

    char *resolved = NULL;
    if (resolve_manifest_path(NULL, &resolved, &err) != FR_OK) {
        report_error(&err, verbose);
        return 1;
    }

    char *directory = manifest_directory(resolved);
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
        report_error(&err, verbose);
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
        report_error(&err, verbose);
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
        printf("daukle: plugin \"%s\" has no fetched artifact to clear\n", label);
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
        size_t plugin_count = fr_plugins_report_declared_count(fr_plugins_report());
        fr_tasks_set_free(&set);
        fr_session_close(&session);
        report_error(&err, verbose);
        if (unknown) {
            char message[256];
            fr_tasks_unknown_message(task_name, plugin_count, message, sizeof message);
            fprintf(stderr, "  %s\n", message);
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

static void confirm_published(const char *goal, void *state) {
    (void) state;
    printf("daukle: %s\n", goal);
}

/* Scaffolding a person runs once, not generation daukle owns, which is why it
   may write at the project root at all: the no-files-at-the-root rule forbids
   daukle WRITING generated files there, and this writes the authored file the
   rule exists to protect. It declares no plugins, because what a project builds
   with is a decision and an empty [modules] is already a working manifest.
   D-70. */
static int run_init(const char *name, int verbose) {
    const char *path = "daukle.toml";
    FILE *existing = fopen(path, "rb");
    if (existing != NULL) {
        fclose(existing);
        fprintf(stderr, "daukle: %s already exists, and init will not overwrite it\n", path);
        return 2;
    }

    /* The only part of this that is not in cli.c, where test_cli can reach it.
       Both spellings of the call are the standard one for their platform and
       neither has a branch of its own. */
    char cwd[1024];
#ifdef _WIN32
    const char *working = _getcwd(cwd, (int) sizeof cwd);
#else
    const char *working = getcwd(cwd, sizeof cwd);
#endif

    const char *project = name != NULL ? name : fr_cli_last_path_segment(working);
    if (project == NULL || project[0] == '\0') project = "my-project";

    char text[1024];
    if (!fr_cli_init_manifest(project, text, sizeof text)) {
        fprintf(stderr, "daukle: the project name is too long to write into %s\n", path);
        return 1;
    }

    fr_error err;
    if (fr_file_write_text(path, text, &err) != FR_OK) {
        report_error(&err, verbose);
        return 1;
    }

    printf("daukle: wrote %s for \"%s\".\n", path, project);
    printf("daukle: add a plugin under [plugins], then run \"daukle sync\".\n");
    return 0;
}

static int run_publish(const char *only, int use_cache, int verbose) {
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

    int matched = 0;
    for (size_t index = 0; index < session.manifest.publish_count; index++) {
        if (only == NULL || strcmp(only, session.manifest.publishes[index].name) == 0) matched++;
    }
    if (matched == 0) {
        fr_tasks_set_free(&set);
        if (only == NULL) {
            fprintf(stderr, "daukle: this project declares no publish destinations\n");
        } else {
            fprintf(stderr, "daukle: this project declares no publish destination \"%s\"\n", only);
        }
        fr_session_close(&session);
        return 2;
    }

    if (fr_tasks_check_publish(&set, &session.manifest, only, &err) != FR_OK) {
        fr_tasks_set_free(&set);
        fr_session_close(&session);
        report_error(&err, verbose);
        return 1;
    }

    fr_sync_report report;
    if (fr_sync_session(&session, 1, &report, &err) != FR_OK) {
        fr_sync_report_free(&report);
        fr_tasks_set_free(&set);
        fr_session_close(&session);
        report_error(&err, verbose);
        return 1;
    }
    fr_sync_report_free(&report);

    if (fr_tasks_run_publish(&set, &session, only, confirm_published, NULL, &err) != FR_OK) {
        fr_tasks_set_free(&set);
        fr_session_close(&session);
        report_error(&err, verbose);
        return 1;
    }

    fr_tasks_set_free(&set);
    fr_session_close(&session);
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
        int runs_something = node->run != NULL
                             || (node->plugin != NULL && node->plugin->run != NULL);
        printf("  runs: %s\n", runs_something ? "a program" : "nothing of its own");
        if (node->run != NULL) printf("  program: %s\n", node->run->tool);
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
            printf("daukle %s\n", fr_self_version());
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
            return plugin_update(options.plugin_label, options.verbose);
        case FR_CLI_CLEAN:
            return clean_derived(options.manifest_path, options.verbose);
        case FR_CLI_TASK:
            return run_task(options.task_name, options.use_cache, options.verbose);
        case FR_CLI_TASKS:
            return list_tasks(options.use_cache, options.verbose);
        case FR_CLI_PUBLISH:
            return run_publish(options.publish_name, options.use_cache, options.verbose);
        case FR_CLI_INIT:
            return run_init(options.init_name, options.verbose);
        case FR_CLI_HELP:
            if (options.help_topic != NULL) {
                fr_cli_print_help(stdout, options.help_topic);
            } else {
                fr_cli_print_usage(stdout);
            }
            return 0;
        case FR_CLI_USAGE:
            break;
    }

    if (options.plugin_unknown_subcommand != NULL) {
        fprintf(stderr, "daukle: unknown plugin subcommand \"%s\"\n", options.plugin_unknown_subcommand);
        return 2;
    }

    if (options.plugin_update_rejected_option != NULL) {
        fprintf(stderr, "daukle: \"plugin update\" cannot take %s: the command is itself a cache"
                        " refresh, so the flag would suppress the one write it exists to make\n",
                options.plugin_update_rejected_option);
        return 2;
    }

    if (options.help_unknown_topic != NULL) {
        fprintf(stderr, "daukle: no command is called \"%s\"\n", options.help_unknown_topic);
        return 2;
    }

    fr_cli_print_usage(stderr);
    return 2;
}
