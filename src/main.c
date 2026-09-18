#include "cli.h"
#include "config.h"
#include "config_lua.h"
#include "error.h"
#include "luax.h"
#include "manifest.h"
#include "region.h"
#include "registry.h"
#include "sync.h"
#include "tomledit.h"

#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DAUKLE_VERSION "0.1.0"

static void print_lua_log(const char *message) {
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

static int print_config(const char *manifest_path, int verbose) {
    fr_error err;
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
    fr_registry_destroy(registry);
    fr_lua_runtime_shutdown();

    if (status != FR_OK) {
        report_error(&err, verbose);
        fr_manifest_free(&manifest);
        free(resolved);
        return 1;
    }

    char *printed = cJSON_Print(manifest.document);
    if (printed == NULL) {
        fprintf(stderr, "daukle: out of memory printing \"%s\"\n", resolved);
        fr_manifest_free(&manifest);
        free(resolved);
        return 1;
    }

    printf("%s\n", printed);
    free(printed);
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

int main(int argc, char **argv) {
    fr_cli_options options;
    fr_cli_parse(argc, argv, &options);
    fr_lua_set_log_sink(print_lua_log);
    fr_lua_set_limits(options.instruction_limit, options.memory_limit);

    switch (options.command) {
        case FR_CLI_VERSION:
            printf("daukle %s\n", DAUKLE_VERSION);
            return 0;
        case FR_CLI_SYNC:
            return run_with_resolved_manifest(options.manifest_path, 1, options.use_cache, options.verbose);
        case FR_CLI_CHECK:
            return run_with_resolved_manifest(options.manifest_path, 0, options.use_cache, options.verbose);
        case FR_CLI_CONFIG_PRINT:
            return print_config(options.manifest_path, options.verbose);
        case FR_CLI_ADD:
            return add_dependency(&options);
        case FR_CLI_USAGE:
            break;
    }

    fprintf(stderr, "usage: daukle [--version | sync [manifest] | check [manifest]"
                    " | add <project>@<range> --to <consumer> [--modules a,b]"
                    " | config print] [--no-cache] [--verbose]\n");
    return 2;
}
