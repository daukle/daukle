#include "cli.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* Enough for "config print <manifest>", the deepest command this CLI has; a
   run with more positional words than this is a usage error regardless. */
#define FR_CLI_MAX_WORDS 3

/* A flag-shaped next token is treated as a missing value rather than consumed:
   "add foo@1.0 --to --modules" must not set add_consumer to the literal
   string "--modules". */
static int has_value_argument(int index, int argc, char **argv) {
    return index + 1 < argc && argv[index + 1][0] != '-';
}

/* Zero means "keep the built-in default" in both limits, and it is also what
   strtoul returns for text it cannot read, so an unparseable
   "--lua-memory-limit banana" would otherwise be accepted and ignored. */
static int parse_positive_number(const char *text, unsigned long *out) {
    char *end = NULL;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (end == text || *end != '\0' || errno == ERANGE || value == 0) return 0;
    *out = value;
    return 1;
}

/* An unrecognised option is a usage error rather than a positional argument:
   read as one, a mistyped "--no-chache" would become the manifest path and the
   run would fail against a file the user never named. */
void fr_cli_parse(int argc, char **argv, fr_cli_options *out) {
    out->command = FR_CLI_USAGE;
    out->manifest_path = NULL;
    out->use_cache = 1;
    out->verbose = 0;
    out->instruction_limit = 0;
    out->memory_limit = 0;
    out->add_spec = NULL;
    out->add_consumer = NULL;
    out->add_modules = NULL;
    out->plugin_label = NULL;
    out->plugin_unknown_subcommand = NULL;

    const char *words[FR_CLI_MAX_WORDS];
    size_t word_count = 0;

    for (int index = 1; index < argc; index++) {
        const char *argument = argv[index];
        if (strcmp(argument, "--no-cache") == 0) {
            out->use_cache = 0;
        } else if (strcmp(argument, "--version") == 0) {
            out->command = FR_CLI_VERSION;
            return;
        } else if (strcmp(argument, "--verbose") == 0) {
            out->verbose = 1;
        } else if (strcmp(argument, "--to") == 0 && has_value_argument(index, argc, argv)) {
            out->add_consumer = argv[++index];
        } else if (strcmp(argument, "--modules") == 0 && has_value_argument(index, argc, argv)) {
            out->add_modules = argv[++index];
        } else if (strcmp(argument, "--lua-instruction-limit") == 0 && has_value_argument(index, argc, argv)) {
            unsigned long value = 0;
            if (!parse_positive_number(argv[++index], &value) || value > (unsigned long) LONG_MAX) return;
            out->instruction_limit = (long) value;
        } else if (strcmp(argument, "--lua-memory-limit") == 0 && has_value_argument(index, argc, argv)) {
            unsigned long value = 0;
            if (!parse_positive_number(argv[++index], &value)) return;
            out->memory_limit = (size_t) value;
        } else if (argument[0] == '-') {
            return;
        } else if (word_count < FR_CLI_MAX_WORDS) {
            words[word_count++] = argument;
        } else {
            return;
        }
    }

    if (word_count == 0) return;
    const char *command = words[0];

    if (strcmp(command, "sync") == 0 || strcmp(command, "check") == 0) {
        if (word_count > 2) return;
        out->command = strcmp(command, "sync") == 0 ? FR_CLI_SYNC : FR_CLI_CHECK;
        if (word_count == 2) out->manifest_path = words[1];
    } else if (strcmp(command, "add") == 0) {
        if (word_count != 2 || out->add_consumer == NULL) return;
        if (strchr(words[1], '@') == NULL) return;
        out->add_spec = words[1];
        out->command = FR_CLI_ADD;
    } else if (strcmp(command, "config") == 0) {
        if (word_count < 2 || strcmp(words[1], "print") != 0) return;
        out->command = FR_CLI_CONFIG_PRINT;
        if (word_count == 3) out->manifest_path = words[2];
    } else if (strcmp(command, "plugin") == 0) {
        if (word_count < 2) return;
        if (strcmp(words[1], "update") != 0) {
            out->plugin_unknown_subcommand = words[1];
            return;
        }
        out->command = FR_CLI_PLUGIN_UPDATE;
        if (word_count == 3) out->plugin_label = words[2];
    }
}
