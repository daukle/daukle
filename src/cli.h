#ifndef DAUKLE_CLI_H
#define DAUKLE_CLI_H

#include <stddef.h>

typedef enum {
    FR_CLI_SYNC,
    FR_CLI_CHECK,
    FR_CLI_ADD,
    FR_CLI_CONFIG_PRINT,
    FR_CLI_VERSION,
    FR_CLI_USAGE
} fr_cli_command;

typedef struct {
    fr_cli_command command;
    const char *manifest_path;
    int use_cache;
    int verbose;
    long instruction_limit;
    size_t memory_limit;
    /* Raw "project@range" text; fr_cli_parse only checks for the '@' and leaves
       splitting to the caller, since splitting in place would write into argv. */
    const char *add_spec;
    const char *add_consumer;
    const char *add_modules;
} fr_cli_options;

void fr_cli_parse(int argc, char **argv, fr_cli_options *out);

#endif
