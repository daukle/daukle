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
    const char *argv[] = { "daukle", "check", "other/daukle.json" };
    fr_cli_options options = parse(3, argv);
    ASSERT_EQ(FR_CLI_CHECK, options.command);
    ASSERT_STR_EQ("other/daukle.json", options.manifest_path);
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

TEST rejects_an_unknown_command_and_no_command_at_all(void) {
    const char *unknown[] = { "daukle", "publish" };
    ASSERT_EQ(FR_CLI_USAGE, parse(2, unknown).command);

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

TEST leaves_the_manifest_path_unset_so_the_directory_is_searched(void) {
    const char *argv[] = { "daukle", "sync" };
    fr_cli_options options = parse(2, argv);
    ASSERT_EQ(FR_CLI_SYNC, options.command);
    ASSERT(options.manifest_path == NULL);
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
    RUN_TEST(rejects_an_unknown_command_and_no_command_at_all);
    RUN_TEST(reports_the_version_flag);
    RUN_TEST(parses_an_add_command);
    RUN_TEST(rejects_an_add_without_a_range);
    RUN_TEST(rejects_an_add_without_a_consumer);
    RUN_TEST(rejects_a_flag_shaped_token_as_another_flags_value);
    RUN_TEST(parses_config_print);
    RUN_TEST(parses_config_print_with_a_manifest_path);
    RUN_TEST(rejects_config_with_an_unknown_subcommand);
    RUN_TEST(leaves_the_manifest_path_unset_so_the_directory_is_searched);
    GREATEST_MAIN_END();
}
