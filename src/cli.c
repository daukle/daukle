#include "cli.h"

#include <string.h>

static const char *DEFAULT_MANIFEST = "tiestone.json";

/* An unrecognised option is a usage error rather than a positional argument:
   read as one, a mistyped "--no-chache" would become the manifest path and the
   run would fail against a file the user never named. */
void fr_cli_parse(int argc, char **argv, fr_cli_options *out) {
    out->command = FR_CLI_USAGE;
    out->manifest_path = DEFAULT_MANIFEST;
    out->use_cache = 1;

    const char *command = NULL;
    const char *manifest_path = NULL;

    for (int index = 1; index < argc; index++) {
        const char *argument = argv[index];
        if (strcmp(argument, "--no-cache") == 0) {
            out->use_cache = 0;
        } else if (strcmp(argument, "--version") == 0) {
            out->command = FR_CLI_VERSION;
            return;
        } else if (argument[0] == '-') {
            return;
        } else if (command == NULL) {
            command = argument;
        } else if (manifest_path == NULL) {
            manifest_path = argument;
        } else {
            return;
        }
    }

    if (command == NULL) return;
    if (strcmp(command, "sync") == 0) out->command = FR_CLI_SYNC;
    else if (strcmp(command, "check") == 0) out->command = FR_CLI_CHECK;
    else return;

    if (manifest_path != NULL) out->manifest_path = manifest_path;
}
