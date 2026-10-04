#include "greatest.h"
#include "cli/cli.h"
#include "config/manifest.h"
#include "project/tasks.h"

static fr_cli_options parse(int argc, const char **argv) {
    fr_cli_options options;
    fr_cli_parse(argc, (char **) argv, &options);
    return options;
}

TEST defaults_the_manifest_path_and_the_cache(void) {
    const char *argv[] = { "daukle", "sync" };
    fr_cli_options options = parse(2, argv);
    ASSERT_EQ(FR_CLI_SYNC, options.command);
    ASSERT(options.manifest_path == NULL);
    ASSERT_EQ(1, options.use_cache);
    PASS();
}

TEST takes_the_manifest_path_after_the_command(void) {
    const char *argv[] = { "daukle", "check", "other/daukle.toml" };
    fr_cli_options options = parse(3, argv);
    ASSERT_EQ(FR_CLI_CHECK, options.command);
    ASSERT_STR_EQ("other/daukle.toml", options.manifest_path);
    PASS();
}

TEST accepts_no_cache_on_either_side_of_the_command(void) {
    const char *trailing[] = { "daukle", "sync", "--no-cache" };
    ASSERT_EQ(0, parse(3, trailing).use_cache);

    const char *leading[] = { "daukle", "--no-cache", "sync" };
    fr_cli_options options = parse(3, leading);
    ASSERT_EQ(FR_CLI_SYNC, options.command);
    ASSERT_EQ(0, options.use_cache);
    PASS();
}

/* A mistyped option read as a positional argument would silently become the
   manifest path, and the run would then fail against a file the user never
   named rather than telling them what they typed wrong. */
TEST rejects_an_unknown_option(void) {
    const char *argv[] = { "daukle", "sync", "--no-chache" };
    ASSERT_EQ(FR_CLI_USAGE, parse(3, argv).command);
    PASS();
}

TEST rejects_a_second_manifest_path(void) {
    const char *argv[] = { "daukle", "sync", "one.json", "two.json" };
    ASSERT_EQ(FR_CLI_USAGE, parse(4, argv).command);
    PASS();
}

/* An unknown first word is now a task name; see an_unknown_first_word_is_a_task. */
TEST no_command_at_all_is_usage(void) {
    const char *bare[] = { "daukle" };
    ASSERT_EQ(FR_CLI_USAGE, parse(1, bare).command);
    PASS();
}

TEST reports_the_version_flag(void) {
    const char *argv[] = { "daukle", "--version" };
    ASSERT_EQ(FR_CLI_VERSION, parse(2, argv).command);
    PASS();
}

TEST parses_an_add_command(void) {
    const char *argv[] = { "daukle", "add", "forebay/basekit@^5.0.0", "--to", "stub", "--modules", "ir,core" };
    fr_cli_options options = parse(7, argv);
    ASSERT_EQ(FR_CLI_ADD, options.command);
    ASSERT_STR_EQ("forebay/basekit@^5.0.0", options.add_spec);
    ASSERT_STR_EQ("stub", options.add_consumer);
    ASSERT_STR_EQ("ir,core", options.add_modules);
    PASS();
}

TEST rejects_an_add_without_a_range(void) {
    const char *argv[] = { "daukle", "add", "forebay/basekit", "--to", "stub" };
    ASSERT_EQ(FR_CLI_USAGE, parse(5, argv).command);
    PASS();
}

TEST rejects_an_add_without_a_consumer(void) {
    const char *argv[] = { "daukle", "add", "forebay/basekit@^5.0.0" };
    ASSERT_EQ(FR_CLI_USAGE, parse(3, argv).command);
    PASS();
}

/* A flag-shaped next token must be read as a missing value, not consumed as
   one: otherwise "--to --modules" would silently set the consumer to the
   literal string "--modules" and produce a confusing downstream failure
   instead of a usage error at the point the mistake was actually made. */
TEST rejects_a_flag_shaped_token_as_another_flags_value(void) {
    const char *argv[] = { "daukle", "add", "forebay/basekit@^5.0.0", "--to", "--modules" };
    ASSERT_EQ(FR_CLI_USAGE, parse(5, argv).command);
    PASS();
}

TEST parses_config_print(void) {
    const char *argv[] = { "daukle", "config", "print" };
    ASSERT_EQ(FR_CLI_CONFIG_PRINT, parse(3, argv).command);
    PASS();
}

TEST parses_config_print_with_a_manifest_path(void) {
    const char *argv[] = { "daukle", "config", "print", "other/daukle.toml" };
    fr_cli_options options = parse(4, argv);
    ASSERT_EQ(FR_CLI_CONFIG_PRINT, options.command);
    ASSERT_STR_EQ("other/daukle.toml", options.manifest_path);
    PASS();
}

TEST rejects_config_with_an_unknown_subcommand(void) {
    const char *argv[] = { "daukle", "config", "delete" };
    ASSERT_EQ(FR_CLI_USAGE, parse(3, argv).command);
    PASS();
}

TEST parses_plugin_update_with_a_label(void) {
    const char *argv[] = { "daukle", "plugin", "update", "npm" };
    fr_cli_options options = parse(4, argv);
    ASSERT_EQ(FR_CLI_PLUGIN_UPDATE, options.command);
    ASSERT_STR_EQ("npm", options.plugin_label);
    PASS();
}

TEST parses_plugin_update_without_a_label(void) {
    const char *argv[] = { "daukle", "plugin", "update" };
    fr_cli_options options = parse(3, argv);
    ASSERT_EQ(FR_CLI_PLUGIN_UPDATE, options.command);
    ASSERT(options.plugin_label == NULL);
    PASS();
}

/* FR_CLI_MAX_WORDS is 3, so a fourth positional word is refused by the word
   loop and never reaches a subcommand. These pin that, because raising the
   limit would silently turn both invocations into a label-less update and a
   path-less print. */
TEST rejects_a_second_word_after_a_plugin_label(void) {
    const char *argv[] = { "daukle", "plugin", "update", "npm", "gradle" };
    ASSERT_EQ(FR_CLI_USAGE, parse(5, argv).command);
    PASS();
}

TEST rejects_a_second_word_after_a_config_print_path(void) {
    const char *argv[] = { "daukle", "config", "print", "a.toml", "b.toml" };
    ASSERT_EQ(FR_CLI_USAGE, parse(5, argv).command);
    PASS();
}

TEST rejects_plugin_with_an_unknown_subcommand_naming_it(void) {
    const char *argv[] = { "daukle", "plugin", "delete" };
    fr_cli_options options = parse(3, argv);
    ASSERT_EQ(FR_CLI_USAGE, options.command);
    ASSERT_STR_EQ("delete", options.plugin_unknown_subcommand);
    PASS();
}

/* Refused rather than ignored: fr_plugins_update_cache forces the cache on for
   the command's whole duration, so an accepted --no-cache would be silently
   inert. The second case pins that the flag still reaches every other command,
   because refusing it everywhere would be the easy wrong fix. */
/* The usage line is the third place this rule is written down, after the
   parser and the two cases below, and it was the one that got it wrong: it
   advertised --no-cache as global while the parser refused it on one command.
   Pinned here because main.c, where the line used to live, has no test
   binary at all. */
/* Rendered into a buffer rather than returned as a literal, since the usage
   became a table. The assertions are the ones that were already here: what is
   pinned is the CLAIM, not the layout. */
static char *rendered_usage(char *buffer, size_t size) {
    FILE *sink = tmpfile();
    if (sink == NULL) return NULL;
    fr_cli_print_usage(sink);
    rewind(sink);
    size_t read = fread(buffer, 1, size - 1, sink);
    buffer[read] = '\0';
    fclose(sink);
    return buffer;
}

TEST the_usage_line_does_not_claim_no_cache_is_global(void) {
    char buffer[4096];
    const char *usage = rendered_usage(buffer, sizeof buffer);
    ASSERT(usage != NULL);
    ASSERT(strstr(usage, "--no-cache") != NULL);
    ASSERT(strstr(usage, "plugin update") != NULL);
    /* The exception has to be stated beside the flag rather than merely
       somewhere in the line, which "plugin update" alone would satisfy
       because it is also a command the line lists. */
    ASSERT(strstr(usage, "--no-cache, on every command but plugin update") != NULL);
    PASS();
}

/* The point of the table. A command added to the enum without a row here fails
   this rather than shipping undocumented, which is the whole reason the help is
   data and not prose. */
TEST every_command_the_parser_can_produce_is_documented(void) {
    size_t count = 0;
    const fr_cli_command_doc *docs = fr_cli_command_docs(&count);

    for (int command = 0; command < FR_CLI_USAGE; command++) {
        int found = 0;
        for (size_t index = 0; index < count; index++) {
            if ((int) docs[index].command == command) found++;
        }
        static char message[128];
        snprintf(message, sizeof message, "fr_cli_command %d has %d table rows, not 1",
                 command, found);
        ASSERT_EQm(message, 1, found);
    }

    /* The other direction, so a row for a command that no longer exists cannot
       sit here being counted. */
    ASSERT_EQ((size_t) FR_CLI_USAGE, count);
    PASS();
}

TEST every_documented_row_carries_a_synopsis_and_a_summary(void) {
    size_t count = 0;
    const fr_cli_command_doc *docs = fr_cli_command_docs(&count);
    for (size_t index = 0; index < count; index++) {
        ASSERT(docs[index].synopsis != NULL && docs[index].synopsis[0] != '\0');
        ASSERT(docs[index].summary != NULL && docs[index].summary[0] != '\0');
    }
    PASS();
}

/* cli.c names the commands and tasks.c reserves them, and nothing but this
   holds the two lists level. */
TEST every_documented_command_word_is_reserved(void) {
    size_t count = 0;
    const fr_cli_command_doc *docs = fr_cli_command_docs(&count);
    int words = 0;
    for (size_t index = 0; index < count; index++) {
        if (docs[index].name == NULL) continue;
        words++;
        static char message[128];
        snprintf(message, sizeof message, "\"%s\" is a command word and not a reserved task name",
                 docs[index].name);
        ASSERT_EQm(message, 1, fr_tasks_name_is_reserved(docs[index].name));
    }
    /* A table that produced no words would pass the loop having checked nothing. */
    ASSERT(words >= 8);
    PASS();
}

TEST the_usage_names_every_command(void) {
    char buffer[4096];
    const char *usage = rendered_usage(buffer, sizeof buffer);
    ASSERT(usage != NULL);

    size_t count = 0;
    const fr_cli_command_doc *docs = fr_cli_command_docs(&count);
    for (size_t index = 0; index < count; index++) {
        static char message[160];
        snprintf(message, sizeof message, "the usage does not name \"%s\"", docs[index].synopsis);
        ASSERTm(message, strstr(usage, docs[index].synopsis) != NULL);
    }
    PASS();
}

TEST init_with_no_word_takes_the_directory_name(void) {
    const char *argv[] = { "daukle", "init" };
    fr_cli_options options = parse(2, argv);
    ASSERT_EQ(FR_CLI_INIT, options.command);
    ASSERT(options.init_name == NULL);
    PASS();
}

TEST init_takes_a_name(void) {
    const char *argv[] = { "daukle", "init", "my-thing" };
    fr_cli_options options = parse(3, argv);
    ASSERT_EQ(FR_CLI_INIT, options.command);
    ASSERT_STR_EQ("my-thing", options.init_name);
    PASS();
}

TEST init_takes_no_second_name(void) {
    const char *argv[] = { "daukle", "init", "one", "two" };
    ASSERT_EQ(FR_CLI_USAGE, parse(4, argv).command);
    PASS();
}

/* Both separators, because a Windows working directory comes back with
   backslashes and a POSIX shell on the same machine hands back forward ones. */
TEST the_last_path_segment_is_taken_on_either_separator(void) {
    ASSERT_STR_EQ("thing", fr_cli_last_path_segment("/home/me/thing"));
    ASSERT_STR_EQ("thing", fr_cli_last_path_segment("C:\\Users\\me\\thing"));
    ASSERT_STR_EQ("thing", fr_cli_last_path_segment("C:/Users/me\\thing"));
    ASSERT_STR_EQ("thing", fr_cli_last_path_segment("thing"));
    ASSERT(fr_cli_last_path_segment("/home/me/") == NULL);
    ASSERT(fr_cli_last_path_segment("") == NULL);
    ASSERT(fr_cli_last_path_segment(NULL) == NULL);
    PASS();
}

/* The shape only. That daukle ACCEPTS what init writes is a stronger claim and
   needs the toml reader, so it is pinned by test_e2e against a real sync.
   FR_SCHEMA is the parser constant, so the two cannot drift. */
TEST init_writes_the_schema_the_parser_requires(void) {
    char text[1024];
    ASSERT(fr_cli_init_manifest("me/thing", text, sizeof text));
    ASSERT(strstr(text, "project = \"me/thing\"") != NULL);
    ASSERT(strstr(text, "[modules]") != NULL);

    char expected_schema[32];
    snprintf(expected_schema, sizeof expected_schema, "schema = %d", FR_SCHEMA);
    ASSERT(strstr(text, expected_schema) != NULL);
    PASS();
}

TEST init_refuses_a_name_it_cannot_render(void) {
    char text[1024];
    ASSERT_FALSE(fr_cli_init_manifest("", text, sizeof text));
    ASSERT_FALSE(fr_cli_init_manifest(NULL, text, sizeof text));
    char tiny[8];
    ASSERT_FALSE(fr_cli_init_manifest("a-name-far-too-long-for-this", tiny, sizeof tiny));
    PASS();
}

TEST help_with_no_word_is_the_whole_list(void) {
    const char *argv[] = { "daukle", "help" };
    fr_cli_options options = parse(2, argv);
    ASSERT_EQ(FR_CLI_HELP, options.command);
    ASSERT(options.help_topic == NULL);
    PASS();
}

TEST the_help_flag_is_the_help_command(void) {
    const char *argv[] = { "daukle", "--help" };
    fr_cli_options options = parse(2, argv);
    ASSERT_EQ(FR_CLI_HELP, options.command);
    ASSERT(options.help_topic == NULL);
    PASS();
}

TEST help_takes_a_command_word(void) {
    const char *argv[] = { "daukle", "help", "sync" };
    fr_cli_options options = parse(3, argv);
    ASSERT_EQ(FR_CLI_HELP, options.command);
    ASSERT_STR_EQ("sync", options.help_topic);
    PASS();
}

/* Answering an unknown word with the whole list reads as though the question
   was understood, so it is a usage error that names the word instead. */
TEST help_names_a_word_that_is_no_command(void) {
    const char *argv[] = { "daukle", "help", "frobnicate" };
    fr_cli_options options = parse(3, argv);
    ASSERT_EQ(FR_CLI_USAGE, options.command);
    ASSERT_STR_EQ("frobnicate", options.help_unknown_topic);
    ASSERT(options.help_topic == NULL);
    PASS();
}

TEST help_for_a_command_prints_its_synopsis(void) {
    FILE *sink = tmpfile();
    ASSERT(sink != NULL);
    ASSERT_EQ(1, fr_cli_print_help(sink, "plugin"));
    rewind(sink);
    char buffer[2048];
    size_t read = fread(buffer, 1, sizeof buffer - 1, sink);
    buffer[read] = '\0';
    fclose(sink);
    ASSERT(strstr(buffer, "plugin update [label]") != NULL);
    ASSERT(strstr(buffer, "--no-cache") != NULL);
    PASS();
}

TEST help_for_an_unknown_command_prints_nothing(void) {
    FILE *sink = tmpfile();
    ASSERT(sink != NULL);
    ASSERT_EQ(0, fr_cli_print_help(sink, "frobnicate"));
    rewind(sink);
    char buffer[64];
    ASSERT_EQ((size_t) 0, fread(buffer, 1, sizeof buffer, sink));
    fclose(sink);
    PASS();
}

TEST rejects_no_cache_on_plugin_update_naming_the_option(void) {
    const char *argv[] = { "daukle", "plugin", "update", "--no-cache" };
    fr_cli_options options = parse(4, argv);
    ASSERT_EQ(FR_CLI_USAGE, options.command);
    ASSERT_STR_EQ("--no-cache", options.plugin_update_rejected_option);
    PASS();
}

TEST keeps_no_cache_on_the_commands_that_honour_it(void) {
    const char *argv[] = { "daukle", "sync", "--no-cache" };
    fr_cli_options options = parse(3, argv);
    ASSERT_EQ(FR_CLI_SYNC, options.command);
    ASSERT_EQ(0, options.use_cache);
    ASSERT(options.plugin_update_rejected_option == NULL);
    PASS();
}

TEST leaves_the_manifest_path_unset_so_the_directory_is_searched(void) {
    const char *argv[] = { "daukle", "sync" };
    fr_cli_options options = parse(2, argv);
    ASSERT_EQ(FR_CLI_SYNC, options.command);
    ASSERT(options.manifest_path == NULL);
    PASS();
}

TEST reads_both_lua_limits(void) {
    const char *argv[] = { "daukle", "sync", "--lua-instruction-limit", "1000",
                           "--lua-memory-limit", "2000000" };
    fr_cli_options options = parse(6, argv);
    ASSERT_EQ(FR_CLI_SYNC, options.command);
    ASSERT_EQ(1000, options.instruction_limit);
    ASSERT_EQ(2000000, (long) options.memory_limit);
    PASS();
}

/* Zero is what both limits use to mean "keep the default", and it is also what
   an unparseable argument used to produce, so the typo vanished silently. */
TEST rejects_a_limit_that_is_not_a_number(void) {
    const char *memory[] = { "daukle", "sync", "--lua-memory-limit", "banana" };
    ASSERT_EQ(FR_CLI_USAGE, parse(4, memory).command);

    const char *instructions[] = { "daukle", "sync", "--lua-instruction-limit", "10x" };
    ASSERT_EQ(FR_CLI_USAGE, parse(4, instructions).command);

    const char *zero[] = { "daukle", "sync", "--lua-memory-limit", "0" };
    ASSERT_EQ(FR_CLI_USAGE, parse(4, zero).command);
    PASS();
}

TEST clean_is_parsed(void) {
    const char *argv[] = { "daukle", "clean" };
    fr_cli_options options = parse(2, argv);
    ASSERT_EQ(FR_CLI_CLEAN, options.command);
    PASS();
}

TEST clean_takes_an_optional_manifest(void) {
    const char *argv[] = { "daukle", "clean", "some/daukle.toml" };
    fr_cli_options options = parse(3, argv);
    ASSERT_EQ(FR_CLI_CLEAN, options.command);
    ASSERT_STR_EQ("some/daukle.toml", options.manifest_path);
    PASS();
}

TEST an_unknown_first_word_is_a_task(void) {
    const char *argv[] = { "daukle", "build" };
    fr_cli_options options = parse(2, argv);
    ASSERT_EQ(FR_CLI_TASK, options.command);
    ASSERT_STR_EQ("build", options.task_name);
    PASS();
}

TEST a_built_in_command_wins_over_a_task_of_the_same_name(void) {
    const char *argv[] = { "daukle", "clean" };
    fr_cli_options options = parse(2, argv);
    ASSERT_EQ(FR_CLI_CLEAN, options.command);
    PASS();
}

TEST a_task_takes_no_second_word(void) {
    const char *argv[] = { "daukle", "build", "daukle.toml" };
    ASSERT_EQ(FR_CLI_USAGE, parse(3, argv).command);
    PASS();
}

TEST tasks_is_a_command_not_a_task(void) {
    char *argv[] = { "daukle", "tasks" };
    fr_cli_options options;
    fr_cli_parse(2, argv, &options);
    ASSERT_EQ(FR_CLI_TASKS, options.command);
    ASSERT(options.task_name == NULL);
    PASS();
}

/* fr_tasks_name_is_reserved's own list and the strcmp chain above it exist
   only because nothing ties them together; a name added to one and
   forgotten in the other would exist, list, and never run without either
   side noticing. Driving fr_cli_parse with each of the words the chain
   above dispatches on, with whatever follows it needs to reach its own
   command, pins both halves of that contract at once: the command must not
   be FR_CLI_TASK (a task of the same name would never run), and the same
   word must be reserved (a task of that name would never be offered
   either). */
TEST every_built_in_command_word_is_also_a_reserved_task_name(void) {
    const char *sync_argv[] = { "daukle", "sync" };
    fr_cli_options sync_options = parse(2, sync_argv);
    ASSERT_EQ(FR_CLI_SYNC, sync_options.command);
    ASSERT(fr_tasks_name_is_reserved("sync"));

    const char *check_argv[] = { "daukle", "check" };
    fr_cli_options check_options = parse(2, check_argv);
    ASSERT_EQ(FR_CLI_CHECK, check_options.command);
    ASSERT(fr_tasks_name_is_reserved("check"));

    const char *add_argv[] = { "daukle", "add", "me/app@1.0.0", "--to", "consumer" };
    fr_cli_options add_options = parse(5, add_argv);
    ASSERT_EQ(FR_CLI_ADD, add_options.command);
    ASSERT(fr_tasks_name_is_reserved("add"));

    const char *config_argv[] = { "daukle", "config", "print" };
    fr_cli_options config_options = parse(3, config_argv);
    ASSERT_EQ(FR_CLI_CONFIG_PRINT, config_options.command);
    ASSERT(fr_tasks_name_is_reserved("config"));

    const char *plugin_argv[] = { "daukle", "plugin", "update" };
    fr_cli_options plugin_options = parse(3, plugin_argv);
    ASSERT_EQ(FR_CLI_PLUGIN_UPDATE, plugin_options.command);
    ASSERT(fr_tasks_name_is_reserved("plugin"));

    const char *clean_argv[] = { "daukle", "clean" };
    fr_cli_options clean_options = parse(2, clean_argv);
    ASSERT_EQ(FR_CLI_CLEAN, clean_options.command);
    ASSERT(fr_tasks_name_is_reserved("clean"));

    const char *tasks_argv[] = { "daukle", "tasks" };
    fr_cli_options tasks_options = parse(2, tasks_argv);
    ASSERT_EQ(FR_CLI_TASKS, tasks_options.command);
    ASSERT(fr_tasks_name_is_reserved("tasks"));

    PASS();
}

TEST publish_with_no_word_means_every_destination(void) {
    const char *argv[] = { "daukle", "publish" };
    fr_cli_options options;
    fr_cli_parse(2, (char **) argv, &options);
    ASSERT_EQ(FR_CLI_PUBLISH, options.command);
    ASSERT(options.publish_name == NULL);
    PASS();
}

TEST publish_with_a_word_names_one_destination(void) {
    const char *argv[] = { "daukle", "publish", "github" };
    fr_cli_options options;
    fr_cli_parse(3, (char **) argv, &options);
    ASSERT_EQ(FR_CLI_PUBLISH, options.command);
    ASSERT_STR_EQ("github", options.publish_name);
    PASS();
}

TEST publish_with_two_words_is_usage(void) {
    const char *argv[] = { "daukle", "publish", "github", "extra" };
    fr_cli_options options;
    fr_cli_parse(4, (char **) argv, &options);
    ASSERT_EQ(FR_CLI_USAGE, options.command);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(defaults_the_manifest_path_and_the_cache);
    RUN_TEST(takes_the_manifest_path_after_the_command);
    RUN_TEST(accepts_no_cache_on_either_side_of_the_command);
    RUN_TEST(rejects_an_unknown_option);
    RUN_TEST(rejects_a_second_manifest_path);
    RUN_TEST(no_command_at_all_is_usage);
    RUN_TEST(reports_the_version_flag);
    RUN_TEST(parses_an_add_command);
    RUN_TEST(rejects_an_add_without_a_range);
    RUN_TEST(rejects_an_add_without_a_consumer);
    RUN_TEST(rejects_a_flag_shaped_token_as_another_flags_value);
    RUN_TEST(parses_config_print);
    RUN_TEST(parses_config_print_with_a_manifest_path);
    RUN_TEST(rejects_config_with_an_unknown_subcommand);
    RUN_TEST(parses_plugin_update_with_a_label);
    RUN_TEST(parses_plugin_update_without_a_label);
    RUN_TEST(rejects_plugin_with_an_unknown_subcommand_naming_it);
    RUN_TEST(the_usage_line_does_not_claim_no_cache_is_global);
    RUN_TEST(rejects_no_cache_on_plugin_update_naming_the_option);
    RUN_TEST(keeps_no_cache_on_the_commands_that_honour_it);
    RUN_TEST(rejects_a_second_word_after_a_plugin_label);
    RUN_TEST(rejects_a_second_word_after_a_config_print_path);
    RUN_TEST(leaves_the_manifest_path_unset_so_the_directory_is_searched);
    RUN_TEST(reads_both_lua_limits);
    RUN_TEST(rejects_a_limit_that_is_not_a_number);
    RUN_TEST(clean_is_parsed);
    RUN_TEST(clean_takes_an_optional_manifest);
    RUN_TEST(an_unknown_first_word_is_a_task);
    RUN_TEST(a_built_in_command_wins_over_a_task_of_the_same_name);
    RUN_TEST(a_task_takes_no_second_word);
    RUN_TEST(tasks_is_a_command_not_a_task);
    RUN_TEST(every_built_in_command_word_is_also_a_reserved_task_name);
    RUN_TEST(publish_with_no_word_means_every_destination);
    RUN_TEST(publish_with_a_word_names_one_destination);
    RUN_TEST(publish_with_two_words_is_usage);
    RUN_TEST(every_command_the_parser_can_produce_is_documented);
    RUN_TEST(every_documented_row_carries_a_synopsis_and_a_summary);
    RUN_TEST(every_documented_command_word_is_reserved);
    RUN_TEST(the_usage_names_every_command);
    RUN_TEST(init_with_no_word_takes_the_directory_name);
    RUN_TEST(init_takes_a_name);
    RUN_TEST(init_takes_no_second_name);
    RUN_TEST(the_last_path_segment_is_taken_on_either_separator);
    RUN_TEST(init_writes_the_schema_the_parser_requires);
    RUN_TEST(init_refuses_a_name_it_cannot_render);
    RUN_TEST(help_with_no_word_is_the_whole_list);
    RUN_TEST(the_help_flag_is_the_help_command);
    RUN_TEST(help_takes_a_command_word);
    RUN_TEST(help_names_a_word_that_is_no_command);
    RUN_TEST(help_for_a_command_prints_its_synopsis);
    RUN_TEST(help_for_an_unknown_command_prints_nothing);
    GREATEST_MAIN_END();
}
