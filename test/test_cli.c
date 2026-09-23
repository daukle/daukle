#include "greatest.h"
#include "cli.h"

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
    GREATEST_MAIN_END();
}
