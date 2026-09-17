#ifndef DAUKLE_CLI_H
#define DAUKLE_CLI_H

typedef enum {
    FR_CLI_SYNC,
    FR_CLI_CHECK,
    FR_CLI_VERSION,
    FR_CLI_USAGE
} fr_cli_command;

typedef struct {
    fr_cli_command command;
    const char *manifest_path;
    int use_cache;
} fr_cli_options;

void fr_cli_parse(int argc, char **argv, fr_cli_options *out);

#endif
