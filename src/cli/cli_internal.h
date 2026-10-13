#ifndef DAUKLE_CLI_INTERNAL_H
#define DAUKLE_CLI_INTERNAL_H

#include "util/types.h"

void fr_cli_report_error(const fr_error *err, int verbose);
int fr_cli_resolve_manifest_path(const char *manifest_path, char **out_path, fr_error *err);
int fr_cli_print_config(const char *manifest_path, int use_cache, int verbose);

#endif
