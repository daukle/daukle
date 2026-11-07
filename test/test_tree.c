#include "greatest.h"

#include "project/region.h"
#include "util/tree.h"
#include "support.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef _WIN32
#include <unistd.h>
#endif

#define SEGMENT "a_directory_name_long_enough_that_a_few_levels_pass_max_path"
#define LEVELS 7

static char scratch[512];
static char outside[512];

static int path_exists(const char *path) {
    struct stat info;
    return stat(path, &info) == 0;
}

static char *read_file(const char *path) {
    char *text = NULL;
    fr_error err;
    if (fr_file_read_text(path, &text, &err) != FR_OK) return NULL;
    return text;
}

/* A junction on Windows, since an unprivileged process there can create a
   directory link only as a junction, and a symlink everywhere else. Success is
   read through the link, because mklink can fail without saying so. */
static int create_directory_link(const char *link_path, const char *target_path) {
#ifdef _WIN32
    char command[2048];
    snprintf(command, sizeof command, "cmd /c mklink /J \"%s\" \"%s\" >nul 2>&1",
             link_path, target_path);
    system(command);
#else
    int created = symlink(target_path, link_path);
    (void) created;
#endif
    char probe[900];
    snprintf(probe, sizeof probe, "%s/marker.txt", link_path);
    char *text = read_file(probe);
    int worked = text != NULL;
    free(text);
    return worked;
}

/* Deepens scratch/SEGMENT by one level through two renames, so every path this
   test itself touches stays short while the tree it builds does not: creating
   the deep levels directly would need the long-path support under test. */
static int nest_one_level(void) {
    char deepest[700], holder[700], moved[800];
    snprintf(deepest, sizeof deepest, "%s/%s", scratch, SEGMENT);
    snprintf(holder, sizeof holder, "%s/holder", scratch);
    snprintf(moved, sizeof moved, "%s/%s", holder, SEGMENT);
    if (fr_test_make_directory(holder) != 0) return 0;
    if (rename(deepest, moved) != 0) return 0;
    return rename(holder, deepest) == 0;
}

TEST a_tree_deeper_than_max_path_is_removed_without_following_its_link(void) {
    snprintf(scratch, sizeof scratch, "%s/daukle_test_tree_deep_%d", fr_test_temp_base(),
             fr_test_process_id());
    snprintf(outside, sizeof outside, "%s/daukle_test_tree_outside_%d", fr_test_temp_base(),
             fr_test_process_id());
    fr_test_remove_tree(scratch);
    fr_test_remove_tree(outside);
    fr_test_make_directory(scratch);
    fr_test_make_directory(outside);

    char marker[700], level[700], leaf[800], link[800];
    snprintf(marker, sizeof marker, "%s/marker.txt", outside);
    snprintf(level, sizeof level, "%s/%s", scratch, SEGMENT);
    snprintf(leaf, sizeof leaf, "%s/leaf.txt", level);
    snprintf(link, sizeof link, "%s/link", level);
    fr_error err;
    ASSERT_EQ(FR_OK, fr_file_write_text(marker, "precious\n", &err));
    ASSERT_EQ(0, fr_test_make_directory(level));
    ASSERT_EQ(FR_OK, fr_file_write_text(leaf, "leaf\n", &err));
    if (!create_directory_link(link, outside)) {
        fr_test_remove_tree(scratch);
        fr_test_remove_tree(outside);
        SKIPm("could not create a junction/symlink pointing outside the tree on this machine");
    }
    for (int built = 1; built < LEVELS; built++) ASSERTm("nesting a level failed", nest_one_level());
    size_t deepest_length = strlen(scratch) + LEVELS * (strlen(SEGMENT) + 1) + strlen("/link");
    ASSERTm("the tree is not deeper than MAX_PATH", deepest_length > 260);

    fr_remove_tree(scratch);

    int tree_gone = !path_exists(scratch);
    char *survivor = read_file(marker);
    int marker_survived = survivor != NULL && strcmp(survivor, "precious\n") == 0;
    free(survivor);
    if (tree_gone) fr_test_remove_tree(outside);

    ASSERTm("the tree deeper than MAX_PATH survived fr_remove_tree", tree_gone);
    ASSERTm("the link target lost its content", marker_survived);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();
    RUN_TEST(a_tree_deeper_than_max_path_is_removed_without_following_its_link);
    GREATEST_MAIN_END();
}
