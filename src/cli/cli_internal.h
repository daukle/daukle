#ifndef DAUKLE_CLI_INTERNAL_H
#define DAUKLE_CLI_INTERNAL_H

#include "cli/cli.h"
#include "util/types.h"

void fr_cli_report_error(const fr_error *err, int verbose);
int fr_cli_resolve_manifest_path(const char *manifest_path, char **out_path, fr_error *err);
int fr_cli_print_config(const char *manifest_path, int use_cache, int verbose);

int fr_cli_add_dependency(const fr_cli_options *options);

char *fr_cli_manifest_directory(const char *manifest_path);
int fr_cli_plugin_update(const char *label, int verbose);

int fr_cli_run(const char *manifest_path, int write, int use_cache, int verbose);
int fr_cli_run_task(const char *task_name, int use_cache, int verbose);
int fr_cli_run_publish(const char *only, int use_cache, int verbose);
int fr_cli_list_tasks(int use_cache, int verbose);

#endif
