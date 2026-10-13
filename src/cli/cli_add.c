#include "cli/cli_internal.h"

#include "cli/cli.h"
#include "config/tomledit.h"
#include "plugin/plugins.h"
#include "project/region.h"
#include "util/error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

int fr_cli_add_dependency(const fr_cli_options *options) {
    fr_error err;
    char *resolved = NULL;
    if (fr_cli_resolve_manifest_path(options->manifest_path, &resolved, &err) != FR_OK) {
        fr_cli_report_error(&err, options->verbose);
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
        fr_cli_report_error(&err, options->verbose);
        free(resolved);
        return 1;
    }

    char **modules = NULL;
    size_t module_count = 0;
    if (split_modules(options->add_modules, &modules, &module_count, &err) != FR_OK) {
        fr_cli_report_error(&err, options->verbose);
        free(resolved);
        return 1;
    }

    char *original_text = NULL;
    if (fr_file_read_text(resolved, &original_text, &err) != FR_OK) {
        fr_cli_report_error(&err, options->verbose);
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
        fr_cli_report_error(&err, options->verbose);
        free(resolved);
        return 1;
    }

    status = fr_file_write_text(resolved, updated_text, &err);
    free(updated_text);
    if (status != FR_OK) {
        fr_cli_report_error(&err, options->verbose);
        free(resolved);
        return 1;
    }

    printf("daukle: added %s@%s to %s\n", project, range, options->add_consumer);
    free(resolved);
    return 0;
}
