#include "cli/cli.h"

#include "config/manifest.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
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
    out->plugin_update_rejected_option = NULL;
    out->task_name = NULL;
    out->publish_name = NULL;
    out->init_name = NULL;
    out->help_topic = NULL;
    out->help_unknown_topic = NULL;

    const char *words[FR_CLI_MAX_WORDS];
    size_t word_count = 0;

    for (int index = 1; index < argc; index++) {
        const char *argument = argv[index];
        if (strcmp(argument, "--no-cache") == 0) {
            out->use_cache = 0;
        } else if (strcmp(argument, "--version") == 0) {
            out->command = FR_CLI_VERSION;
            return;
        } else if (strcmp(argument, "--help") == 0) {
            out->command = FR_CLI_HELP;
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
        if (!out->use_cache) {
            out->plugin_update_rejected_option = "--no-cache";
            return;
        }
        out->command = FR_CLI_PLUGIN_UPDATE;
        if (word_count == 3) out->plugin_label = words[2];
    } else if (strcmp(command, "clean") == 0) {
        if (word_count > 2) return;
        out->command = FR_CLI_CLEAN;
        if (word_count == 2) out->manifest_path = words[1];
    } else if (strcmp(command, "tasks") == 0) {
        if (word_count > 1) return;
        out->command = FR_CLI_TASKS;
    } else if (strcmp(command, "publish") == 0) {
        if (word_count > 2) return;
        out->command = FR_CLI_PUBLISH;
        if (word_count == 2) out->publish_name = words[1];
    } else if (strcmp(command, "init") == 0) {
        if (word_count > 2) return;
        out->command = FR_CLI_INIT;
        if (word_count == 2) out->init_name = words[1];
    } else if (strcmp(command, "help") == 0) {
        if (word_count > 2) return;
        out->command = FR_CLI_HELP;
        if (word_count == 1) return;
        /* An unknown topic is named rather than silently answered with the
           whole list, which reads as though the question was understood. */
        if (fr_cli_find_command_doc(words[1]) == NULL) {
            out->command = FR_CLI_USAGE;
            out->help_unknown_topic = words[1];
            return;
        }
        out->help_topic = words[1];
    } else {
        if (word_count != 1) return;
        out->command = FR_CLI_TASK;
        out->task_name = command;
    }
}

/* Every command is described here and nowhere else. The short usage and
   `daukle help <command>` are both rendered from this, which is what stops the
   two disagreeing: the single usage line this replaced advertised --no-cache as
   global while the parser refused it on one command, and nothing noticed for
   as long as the claim lived in prose. */
static const fr_cli_command_doc COMMAND_DOCS[] = {
    { FR_CLI_SYNC, "sync", "sync [manifest]",
      "bring the project into line with its manifest",
      "Resolves every dependency, provisions what the plugins ask for and writes the generated\n"
      "files into the derived directory. Nothing is written to the project root." },
    { FR_CLI_CHECK, "check", "check [manifest]",
      "report whether the project is in sync, changing nothing",
      "Exits non-zero when a sync would do work. A fresh clone of a generating toolchain is\n"
      "legitimately out of sync, so this is not a precondition for sync, it is the assertion\n"
      "that one has already run." },
    { FR_CLI_ADD, "add", "add <project>@<range> --to <consumer> [--modules a,b]",
      "record a dependency in the manifest",
      "The range is a requirement rather than a version: what it resolves to is decided at sync\n"
      "time and written to the lock, not here." },
    { FR_CLI_CONFIG_PRINT, "config", "config print [manifest]",
      "print the configuration as daukle resolved it", NULL },
    { FR_CLI_PLUGIN_UPDATE, "plugin", "plugin update [label]",
      "refetch plugins, ignoring the cache",
      "Without a label this refreshes every plugin. It is the one command that refuses\n"
      "--no-cache, because the command IS the refresh." },
    { FR_CLI_CLEAN, "clean", "clean [manifest]",
      "remove what sync generated", NULL },
    { FR_CLI_TASKS, "tasks", "tasks",
      "list the tasks the plugins declare", NULL },
    { FR_CLI_PUBLISH, "publish", "publish [name]",
      "publish the destinations the manifest declares",
      "Without a name this publishes every declared destination." },
    { FR_CLI_INIT, "init", "init [name]",
      "write a starter daukle.toml in this directory",
      "Refuses to touch an existing daukle.toml. The name defaults to this directory's, and the\n"
      "manifest it writes declares no plugins: what the project builds with is the next edit.\n"
      "\n"
      "It does NOT install the wrapper, and cannot: the release URL that would need belongs to\n"
      "the wrapper scripts, not to daukle. See wrapper/ABOUT.md." },
    { FR_CLI_TASK, NULL, "<task>",
      "run a task by the name `tasks` lists",
      "A task takes no manifest path, because two bare words cannot be told apart from a task\n"
      "and a path." },
    { FR_CLI_VERSION, NULL, "--version",
      "print the version and exit", NULL },
    { FR_CLI_HELP, "help", "help [command]",
      "print this, or what one command does", NULL },
};

/* Here rather than in main.c because main.c has no test binary, and the two
   things that can actually be wrong are which segment is taken and what the
   manifest says. Both separators are looked for: a Windows working directory
   uses backslashes and a POSIX shell on Windows hands back forward ones. */
const char *fr_cli_last_path_segment(const char *path) {
    if (path == NULL) return NULL;
    const char *name = path;
    for (const char *scan = path; *scan != '\0'; scan++) {
        if (*scan == '/' || *scan == '\\') name = scan + 1;
    }
    return *name == '\0' ? NULL : name;
}

int fr_cli_init_manifest(const char *project, char *out, size_t size) {
    if (project == NULL || project[0] == '\0') return 0;
    int written = snprintf(out, size,
                           "schema = %d\n"
                           "project = \"%s\"\n"
                           "version = \"0.1.0\"\n"
                           "\n"
                           "[modules]\n",
                           FR_SCHEMA, project);
    return written > 0 && (size_t) written < size;
}

const fr_cli_command_doc *fr_cli_command_docs(size_t *count) {
    *count = sizeof COMMAND_DOCS / sizeof COMMAND_DOCS[0];
    return COMMAND_DOCS;
}

const fr_cli_command_doc *fr_cli_find_command_doc(const char *name) {
    if (name == NULL) return NULL;
    size_t count = 0;
    const fr_cli_command_doc *docs = fr_cli_command_docs(&count);
    for (size_t index = 0; index < count; index++) {
        if (docs[index].name != NULL && strcmp(docs[index].name, name) == 0) return &docs[index];
    }
    return NULL;
}

/* Measured rather than chosen, so a longer synopsis cannot quietly push the
   summaries out of their column. One that outgrows the width gets its summary
   on the next line instead of a ragged row. */
#define FR_CLI_SUMMARY_COLUMN 46

static void print_row(FILE *out, const char *label, const char *text, int width) {
    if ((int) strlen(label) >= width) {
        fprintf(out, "  %s\n  %*s%s\n", label, width, "", text);
    } else {
        fprintf(out, "  %-*s%s\n", width, label, text);
    }
}

void fr_cli_print_usage(FILE *out) {
    size_t count = 0;
    const fr_cli_command_doc *docs = fr_cli_command_docs(&count);

    int width = 0;
    for (size_t index = 0; index < count; index++) {
        int length = (int) strlen(docs[index].synopsis) + 2;
        if (length > width && length <= FR_CLI_SUMMARY_COLUMN) width = length;
    }

    fprintf(out, "usage: daukle <command> [options]\n\n");
    for (size_t index = 0; index < count; index++) {
        print_row(out, docs[index].synopsis, docs[index].summary, width);
    }

    /* --no-cache is spelled as "every command but one" rather than as a global
       flag, because the parser refuses it on `plugin update`. */
    fprintf(out, "\noptions:\n");
    print_row(out, "--verbose", "say what is being done", width);
    print_row(out, "--no-cache, on every command but plugin update", "refetch rather than reuse",
              width);
    fprintf(out, "\n\"daukle help <command>\" prints what one command does.\n");
}

int fr_cli_print_help(FILE *out, const char *name) {
    const fr_cli_command_doc *doc = fr_cli_find_command_doc(name);
    if (doc == NULL) return 0;

    fprintf(out, "daukle %s\n  %s\n", doc->synopsis, doc->summary);
    if (doc->detail != NULL) fprintf(out, "\n%s\n", doc->detail);
    return 1;
}
