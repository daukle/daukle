#include "greatest.h"
#include "cli.h"

static fr_cli_options parse(int argc, const char **argv) {
    fr_cli_options options;
    fr_cli_parse(argc, (char **) argv, &options);
    return options;
}

TEST defaults_the_manifest_path_and_the_cache(void) {
    const char *argv[] = { "tiestone", "sync" };
    fr_cli_options options = parse(2, argv);
    ASSERT_EQ(FR_CLI_SYNC, options.command);
    ASSERT_STR_EQ("tiestone.json", options.manifest_path);
    ASSERT_EQ(1, options.use_cache);
    PASS();
}

TEST takes_the_manifest_path_after_the_command(void) {
    const char *argv[] = { "tiestone", "check", "other/tiestone.json" };
    fr_cli_options options = parse(3, argv);
    ASSERT_EQ(FR_CLI_CHECK, options.command);
    ASSERT_STR_EQ("other/tiestone.json", options.manifest_path);
    PASS();
}

TEST accepts_no_cache_on_either_side_of_the_command(void) {
    const char *trailing[] = { "tiestone", "sync", "--no-cache" };
    ASSERT_EQ(0, parse(3, trailing).use_cache);

    const char *leading[] = { "tiestone", "--no-cache", "sync" };
    fr_cli_options options = parse(3, leading);
    ASSERT_EQ(FR_CLI_SYNC, options.command);
    ASSERT_EQ(0, options.use_cache);
    PASS();
}

/* A mistyped option read as a positional argument would silently become the
   manifest path, and the run would then fail against a file the user never
   named rather than telling them what they typed wrong. */
TEST rejects_an_unknown_option(void) {
    const char *argv[] = { "tiestone", "sync", "--no-chache" };
    ASSERT_EQ(FR_CLI_USAGE, parse(3, argv).command);
    PASS();
}

TEST rejects_a_second_manifest_path(void) {
    const char *argv[] = { "tiestone", "sync", "one.json", "two.json" };
    ASSERT_EQ(FR_CLI_USAGE, parse(4, argv).command);
    PASS();
}

TEST rejects_an_unknown_command_and_no_command_at_all(void) {
    const char *unknown[] = { "tiestone", "publish" };
    ASSERT_EQ(FR_CLI_USAGE, parse(2, unknown).command);

    const char *bare[] = { "tiestone" };
    ASSERT_EQ(FR_CLI_USAGE, parse(1, bare).command);
    PASS();
}

TEST reports_the_version_flag(void) {
    const char *argv[] = { "tiestone", "--version" };
    ASSERT_EQ(FR_CLI_VERSION, parse(2, argv).command);
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
    GREATEST_MAIN_END();
}
