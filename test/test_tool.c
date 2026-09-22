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

GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    fr_test_prepend_to_path_dir_of(argv[0]);
    GREATEST_MAIN_BEGIN();
    RUN_TEST(a_tool_on_the_search_path_resolves_to_an_absolute_path);
    RUN_TEST(a_tool_that_is_not_installed_fails_naming_it);
    GREATEST_MAIN_END();
}
