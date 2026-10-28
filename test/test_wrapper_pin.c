#include "greatest.h"

#include "cli/wrapper_pin.h"
#include "project/region.h"
#include "support.h"
#include "util/executable.h"
#include "util/types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *RELEASED_PIN =
    "# The pinned daukle this project builds with.\n"
    "version = \"0.13.0\"\n"
    "\n"
    "catalogs = [\"https://raw.githubusercontent.com/daukle/manifest/main/plugins.json\"]\n"
    "\n"
    "[assets.\"linux/x86_64\"]\n"
    "asset = \"daukle-linux-x86_64\"\n"
    "sha256 = \"ba196c9f1274cb44f932e6c71007ae862809638e2a836ac537df9cc6c4eb6bb9\"\n";

TEST reads_the_version_of_a_released_pin(void) {
    char version[64];
    ASSERT(fr_wrapper_pin_version(RELEASED_PIN, version, sizeof version));
    ASSERT_STR_EQ("0.13.0", version);
    PASS();
}

TEST reads_an_empty_version_as_empty(void) {
    char version[64] = "untouched";
    ASSERT(fr_wrapper_pin_version("version = \"\"\n[assets.\"a/b\"]\n", version, sizeof version));
    ASSERT_STR_EQ("", version);
    PASS();
}

TEST text_with_no_version_has_none(void) {
    char version[64];
    ASSERT_FALSE(fr_wrapper_pin_version("catalogs = []\n", version, sizeof version));
    ASSERT_FALSE(fr_wrapper_pin_version("", version, sizeof version));
    PASS();
}

TEST a_version_inside_a_section_is_not_the_pins(void) {
    char version[64];
    ASSERT_FALSE(fr_wrapper_pin_version("[assets.\"linux/x86_64\"]\nversion = \"9.9.9\"\n", version,
                                        sizeof version));
    PASS();
}

TEST a_key_that_only_starts_with_version_is_not_it(void) {
    char version[64];
    ASSERT_FALSE(fr_wrapper_pin_version("versioning = \"1\"\n", version, sizeof version));
    PASS();
}

TEST a_version_too_long_for_the_buffer_is_refused(void) {
    char version[4];
    ASSERT_FALSE(fr_wrapper_pin_version("version = \"0.13.0\"\n", version, sizeof version));
    PASS();
}

static char base[512];

static void make_base(const char *name) {
    snprintf(base, sizeof base, "%s/daukle-wrapper-pin-%s-%d", fr_test_temp_base(), name,
             fr_test_process_id());
    fr_test_remove_tree(base);
    fr_test_make_directory(base);
}

static void write_pin_at(const char *relative) {
    char target[1024];
    snprintf(target, sizeof target, "%s/%s", base, relative);
    fr_error err;
    fr_file_write_text(target, RELEASED_PIN, &err);
}

TEST the_override_wins_over_the_executable_directory(void) {
    make_base("override");
    char bin[600];
    snprintf(bin, sizeof bin, "%s/bin", base);
    fr_test_make_directory(bin);
    write_pin_at("bin/wrapper.toml");
    write_pin_at("chosen.toml");
    char chosen[600];
    snprintf(chosen, sizeof chosen, "%s/chosen.toml", base);

    char *found = NULL;
    char *searched = NULL;
    ASSERT(fr_wrapper_pin_find(chosen, bin, &found, &searched));
    ASSERT_STR_EQ(chosen, found);
    ASSERT(searched == NULL);
    free(found);
    fr_test_remove_tree(base);
    PASS();
}

TEST the_pin_beside_the_executable_is_found(void) {
    make_base("beside");
    write_pin_at("wrapper.toml");
    char *found = NULL;
    char *searched = NULL;
    ASSERT(fr_wrapper_pin_find(NULL, base, &found, &searched));
    ASSERT(strstr(found, "/wrapper.toml") != NULL);
    ASSERT(strstr(found, "share") == NULL);
    free(found);
    fr_test_remove_tree(base);
    PASS();
}

TEST the_shared_pin_of_an_fhs_layout_is_found(void) {
    make_base("shared");
    char directory[700];
    snprintf(directory, sizeof directory, "%s/bin", base);
    fr_test_make_directory(directory);
    snprintf(directory, sizeof directory, "%s/share", base);
    fr_test_make_directory(directory);
    snprintf(directory, sizeof directory, "%s/share/daukle", base);
    fr_test_make_directory(directory);
    write_pin_at("share/daukle/wrapper.toml");
    snprintf(directory, sizeof directory, "%s/bin", base);

    char *found = NULL;
    char *searched = NULL;
    ASSERT(fr_wrapper_pin_find("", directory, &found, &searched));
    ASSERT(strstr(found, "../share/daukle/wrapper.toml") != NULL);
    free(found);
    fr_test_remove_tree(base);
    PASS();
}

TEST no_pin_lists_every_location_searched(void) {
    make_base("absent");
    char missing[600];
    snprintf(missing, sizeof missing, "%s/missing.toml", base);
    char *found = NULL;
    char *searched = NULL;
    ASSERT_FALSE(fr_wrapper_pin_find(missing, base, &found, &searched));
    ASSERT(found == NULL);
    ASSERT(strstr(searched, "missing.toml\n") != NULL);
    ASSERT(strstr(searched, "/wrapper.toml\n") != NULL);
    ASSERT(strstr(searched, "/../share/daukle/wrapper.toml\n") != NULL);
    free(searched);
    fr_test_remove_tree(base);
    PASS();
}

TEST the_test_binary_knows_its_own_directory(void) {
    char *directory = fr_executable_directory();
    ASSERT(directory != NULL);
    ASSERT(directory[0] != '\0');
    char last = directory[strlen(directory) - 1];
    ASSERT(last != '/' && last != '\\');
    free(directory);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(reads_the_version_of_a_released_pin);
    RUN_TEST(reads_an_empty_version_as_empty);
    RUN_TEST(text_with_no_version_has_none);
    RUN_TEST(a_version_inside_a_section_is_not_the_pins);
    RUN_TEST(a_key_that_only_starts_with_version_is_not_it);
    RUN_TEST(a_version_too_long_for_the_buffer_is_refused);
    RUN_TEST(the_override_wins_over_the_executable_directory);
    RUN_TEST(the_pin_beside_the_executable_is_found);
    RUN_TEST(the_shared_pin_of_an_fhs_layout_is_found);
    RUN_TEST(no_pin_lists_every_location_searched);
    RUN_TEST(the_test_binary_knows_its_own_directory);
    GREATEST_MAIN_END();
}
