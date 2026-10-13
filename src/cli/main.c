#include "cli/cli_internal.h"

#include "cli/cli.h"
#include "config/config.h"
#include "config/config_lua.h"
#include "lua/lua_verbs.h"
#include "lua/luax.h"
#include "plugin/plugins.h"
#include "plugin/registry.h"
#include "plugin/resolvers.h"
#include "project/derived.h"
#include "project/region.h"
#include "project/sync.h"
#include "util/error.h"
#include "util/version.h"

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

void fr_cli_report_error(const fr_error *err, int verbose) {
    fprintf(stderr, "daukle: %s\n", err->message);
    if (verbose) {
        const char *traceback = fr_lua_last_traceback();
        if (traceback != NULL) fprintf(stderr, "%s\n", traceback);
    }
}

/* sync.c keeps its own copy of this same small split rather than exposing one. */
char *fr_cli_manifest_directory(const char *manifest_path) {
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
int fr_cli_resolve_manifest_path(const char *manifest_path, char **out_path, fr_error *err) {
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

int fr_cli_run(const char *manifest_path, int write, int use_cache, int verbose) {
    fr_error err;
    fr_sync_report report;
    int status = 0;
    if (fr_sync(manifest_path, write, use_cache, &report, &err) != FR_OK) {
        fr_cli_report_error(&err, verbose);
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
    if (fr_cli_resolve_manifest_path(manifest_path, &resolved, &err) != FR_OK) {
        fr_cli_report_error(&err, verbose);
        return 1;
    }
    int status = fr_cli_run(resolved, write, use_cache, verbose);
    free(resolved);
    return status;
}

static int clean_derived(const char *manifest_path, int verbose) {
    fr_error err;
    char *resolved = NULL;
    if (fr_cli_resolve_manifest_path(manifest_path, &resolved, &err) != FR_OK) {
        fr_cli_report_error(&err, verbose);
        return 1;
    }

    char *directory = fr_cli_manifest_directory(resolved);
    free(resolved);
    if (directory == NULL) {
        fprintf(stderr, "daukle: out of memory finding the manifest directory\n");
        return 1;
    }

    int status = fr_derived_clean(directory, &err);
    free(directory);
    if (status != FR_OK) {
        fr_cli_report_error(&err, verbose);
        return 1;
    }

    printf("daukle: cleaned\n");
    return 0;
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
        fr_cli_report_error(&err, verbose);
        return 1;
    }

    printf("daukle: wrote %s for \"%s\".\n", path, project);
    printf("daukle: add a plugin under [plugins], then run \"daukle sync\".\n");
    return 0;
}

int main(int argc, char **argv) {
    fr_cli_options options;
    fr_cli_parse(argc, argv, &options);
    fr_lua_set_log_sink(print_lua_log);
    fr_derived_set_notice_sink(print_notice);
    fr_lua_set_limits(options.instruction_limit, options.memory_limit);
    fr_lua_verbs_set_verbose(options.verbose);
    fr_lua_verbs_set_resolving(options.resolving);

    switch (options.command) {
        case FR_CLI_VERSION:
            printf("daukle %s\n", fr_self_version());
            return 0;
        case FR_CLI_SYNC:
            return run_with_resolved_manifest(options.manifest_path, 1, options.use_cache, options.verbose);
        case FR_CLI_CHECK:
            return run_with_resolved_manifest(options.manifest_path, 0, options.use_cache, options.verbose);
        case FR_CLI_CONFIG_PRINT:
            return fr_cli_print_config(options.manifest_path, options.use_cache, options.verbose);
        case FR_CLI_ADD:
            return fr_cli_add_dependency(&options);
        case FR_CLI_PLUGIN_UPDATE:
            return fr_cli_plugin_update(options.plugin_label, options.verbose);
        case FR_CLI_CLEAN:
            return clean_derived(options.manifest_path, options.verbose);
        case FR_CLI_TASK:
            return fr_cli_run_task(options.task_name, options.use_cache, options.verbose);
        case FR_CLI_TASKS:
            return fr_cli_list_tasks(options.use_cache, options.verbose);
        case FR_CLI_PUBLISH:
            return fr_cli_run_publish(options.publish_name, options.use_cache, options.verbose);
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
