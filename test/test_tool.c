#include "greatest.h"

#include "support.h"
#include "tool.h"

#include <stdlib.h>
#include <string.h>

TEST a_tool_on_the_search_path_resolves_to_an_absolute_path(void) {
    char *path = NULL;
    fr_error err;
    /* The test binary's own directory is put on the path by the harness below,
       so this resolves the one executable guaranteed to exist everywhere. */
    ASSERT_EQ(FR_OK, fr_tool_resolve("test_tool", &path, &err));
    ASSERT(path != NULL);
    ASSERT(strstr(path, "test_tool") != NULL);
    free(path);
    PASS();
}

TEST a_tool_that_is_not_installed_fails_naming_it(void) {
    char *path = NULL;
    fr_error err;
    ASSERT_EQ(FR_ERR, fr_tool_resolve("daukle-no-such-tool", &path, &err));
    ASSERT(strstr(err.message, "daukle-no-such-tool") != NULL);
    ASSERT(strstr(err.message, "is not installed") != NULL);
    PASS();
}

static char *dup_or_empty(const char *text) {
    const char *source = text == NULL ? "" : text;
    size_t size = strlen(source) + 1;
    char *copy = malloc(size);
    if (copy != NULL) memcpy(copy, source, size);
    return copy;
}

/* Regresses a real bug: a permission check alone cannot tell a program from
   a directory, so a directory named after the tool used to resolve as it on
   POSIX. Windows was never affected, fopen already rejects a directory there,
   so this only proves anything on the POSIX runners. */
TEST a_directory_shadowing_a_tool_name_does_not_resolve_as_it(void) {
    char temp_dir[512];
    snprintf(temp_dir, sizeof temp_dir, "%s/daukle_test_tool_%d", fr_test_temp_base(),
             fr_test_process_id());
    ASSERT_EQ(0, fr_test_make_directory(temp_dir));

    char fake_tool_path[600];
#ifdef _WIN32
    snprintf(fake_tool_path, sizeof fake_tool_path, "%s/daukle-fake-tool.exe", temp_dir);
#else
    snprintf(fake_tool_path, sizeof fake_tool_path, "%s/daukle-fake-tool", temp_dir);
#endif
    ASSERT_EQ(0, fr_test_make_directory(fake_tool_path));

    char *old_path = dup_or_empty(getenv("PATH"));
    char new_path[4096];
#ifdef _WIN32
    snprintf(new_path, sizeof new_path, "%s;%s", temp_dir, old_path);
#else
    snprintf(new_path, sizeof new_path, "%s:%s", temp_dir, old_path);
#endif
    fr_test_set_env("PATH", new_path);

    char *path = NULL;
    fr_error err;
    ASSERT_EQ(FR_ERR, fr_tool_resolve("daukle-fake-tool", &path, &err));
    ASSERT(strstr(err.message, "is not installed") != NULL);

    fr_test_set_env("PATH", old_path);
    free(old_path);
    fr_test_remove_tree(temp_dir);
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    fr_test_prepend_to_path_dir_of(argv[0]);
    GREATEST_MAIN_BEGIN();
    RUN_TEST(a_tool_on_the_search_path_resolves_to_an_absolute_path);
    RUN_TEST(a_tool_that_is_not_installed_fails_naming_it);
    RUN_TEST(a_directory_shadowing_a_tool_name_does_not_resolve_as_it);
    GREATEST_MAIN_END();
}
