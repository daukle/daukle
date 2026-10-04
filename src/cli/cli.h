#ifndef DAUKLE_CLI_H
#define DAUKLE_CLI_H

#include <stddef.h>
#include <stdio.h>

/* FR_CLI_USAGE must stay LAST. It is the only member no word reaches, so the
   documentation table covers every value below it, and test_cli walks the range
   to prove a new command cannot be added without a help entry. */
typedef enum {
    FR_CLI_SYNC,
    FR_CLI_CHECK,
    FR_CLI_ADD,
    FR_CLI_CONFIG_PRINT,
    FR_CLI_PLUGIN_UPDATE,
    FR_CLI_CLEAN,
    FR_CLI_VERSION,
    FR_CLI_TASK,
    FR_CLI_TASKS,
    FR_CLI_PUBLISH,
    FR_CLI_HELP,
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
    /* NULL means every plugin, the same as omitting the word entirely. */
    const char *plugin_label;
    /* Set only when "plugin" was followed by a word other than "update", so the
       caller can name the mistake rather than print a bare usage line. */
    const char *plugin_unknown_subcommand;
    /* The option "plugin update" was given but can never honour, or NULL.
       That command forces the cache on for its whole duration because it IS
       the refresh, so honouring --no-cache would suppress the one write the
       command exists to make. */
    const char *plugin_update_rejected_option;
    /* The word a run names when it matches no built-in command. A task takes
       no positional manifest path, because two bare words cannot be told
       apart from a task and a path. */
    const char *task_name;
    /* The destination "daukle publish" names, or NULL for every declared one. */
    const char *publish_name;
    /* The command "daukle help" was asked about, or NULL for the whole list. */
    const char *help_topic;
    /* The word "help" was given that names no command, so the caller can say
       which one rather than print the list as though nothing was asked. */
    const char *help_unknown_topic;
} fr_cli_options;

void fr_cli_parse(int argc, char **argv, fr_cli_options *out);

/* One row of the help, and the only place a command is described. The short
   usage and `daukle help <command>` are both rendered from this table, so help
   that disagrees with itself is not expressible. */
typedef struct {
    fr_cli_command command;
    /* The word a user types, or NULL for the two that no word names: a task is
       spelled as whatever the plugins declared, and --version is a flag. */
    const char *name;
    const char *synopsis;
    const char *summary;
    /* NULL where the summary says everything there is to say. */
    const char *detail;
} fr_cli_command_doc;

const fr_cli_command_doc *fr_cli_command_docs(size_t *count);
const fr_cli_command_doc *fr_cli_find_command_doc(const char *name);

/* The short usage, here rather than in main.c so the one claim it makes that
   the parser can contradict is testable: --no-cache is refused by
   `plugin update` and accepted everywhere else, and main.c has no test
   binary to pin that against. */
void fr_cli_print_usage(FILE *out);

/* The detail for one command. Returns 0 when no command goes by that name,
   having printed nothing. */
int fr_cli_print_help(FILE *out, const char *name);

#endif
