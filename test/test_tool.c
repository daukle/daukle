#include "greatest.h"

#include "support.h"
#include "tool.h"

#include <stdlib.h>
#include <string.h>

static int path_is_absolute(const char *path) {
#ifdef _WIN32
    int drive_letter = (path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z');
    if (drive_letter && path[1] == ':' && (path[2] == '\\' || path[2] == '/')) return 1;
    return path[0] == '\\' && path[1] == '\\';
#else
    return path[0] == '/';
#endif
}

TEST a_tool_on_the_search_path_resolves_to_an_absolute_path(void) {
    char *path = NULL;
    fr_error err;
    /* The test binary's own directory is put on the path by the harness below,
       so this resolves the one executable guaranteed to exist everywhere. */
    int status = fr_tool_resolve("test_tool", &path, &err);
    int path_is_set = path != NULL;
    int is_absolute = path_is_set && path_is_absolute(path);
    free(path);

    ASSERT_EQ(FR_OK, status);
    ASSERT(path_is_set);
    ASSERT(is_absolute);
    PASS();
}

/* The directory the test binary lives in, so a test can step into it and name
   it relatively. Recorded by the harness below. */
static char binary_dir[1024];

static char *dup_or_empty(const char *text);

/* A relative PATH entry is what makes canonicalisation load bearing: POSIX
   chdir()s into options.cwd before execv, so a relative program would resolve
   against the child's new directory while CreateProcess resolves it against
   daukle's own, and one call would run two different programs. */
TEST a_relative_path_entry_still_resolves_to_an_absolute_path(void) {
    char original_dir[1024];
    int saved_dir = fr_test_get_working_directory(original_dir, sizeof original_dir);
    int stepped_in = saved_dir && binary_dir[0] != '\0'
                     && fr_test_set_working_directory(binary_dir);

    char *old_path = dup_or_empty(getenv("PATH"));
    if (stepped_in) fr_test_set_env("PATH", ".");

    char *path = NULL;
    fr_error err;
    int status = stepped_in ? fr_tool_resolve("test_tool", &path, &err) : FR_ERR;
    int is_absolute = status == FR_OK && path != NULL && path_is_absolute(path);

    fr_test_set_env("PATH", old_path);
    free(old_path);
    free(path);
    if (saved_dir) fr_test_set_working_directory(original_dir);

    ASSERT(stepped_in);
    ASSERT_EQ(FR_OK, status);
    ASSERT(is_absolute);
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
    int made_temp_dir = fr_test_make_directory(temp_dir) == 0;

    char fake_tool_path[600];
#ifdef _WIN32
    snprintf(fake_tool_path, sizeof fake_tool_path, "%s/daukle-fake-tool.exe", temp_dir);
#else
    snprintf(fake_tool_path, sizeof fake_tool_path, "%s/daukle-fake-tool", temp_dir);
#endif
    int made_fake_tool = fr_test_make_directory(fake_tool_path) == 0;

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
    int status = fr_tool_resolve("daukle-fake-tool", &path, &err);
    char message[sizeof err.message];
    snprintf(message, sizeof message, "%s", err.message);

    /* Cleanup runs before any assertion so a failure never leaves PATH, which is process-global, pointed at temp_dir. */
    fr_test_set_env("PATH", old_path);
    free(old_path);
    free(path);
    fr_test_remove_tree(temp_dir);

    ASSERT(made_temp_dir);
    ASSERT(made_fake_tool);
    ASSERT_EQ(FR_ERR, status);
    ASSERT(strstr(message, "is not installed") != NULL);
    PASS();
}

static int write_empty_file(const char *path) {
    FILE *file = fopen(path, "wb");
    if (file == NULL) return 0;
    fclose(file);
    return 1;
}

/* A .bat sitting on PATH is the case "is not installed" would send someone
   hunting for an install that is right there, so it is refused by name. */
TEST a_batch_file_on_the_search_path_is_refused_as_one(void) {
    char temp_dir[512];
    snprintf(temp_dir, sizeof temp_dir, "%s/daukle_test_batch_%d", fr_test_temp_base(),
             fr_test_process_id());
    int made_temp_dir = fr_test_make_directory(temp_dir) == 0;

    char batch_path[600];
    snprintf(batch_path, sizeof batch_path, "%s/daukle-fake-batch.bat", temp_dir);
    int made_batch = write_empty_file(batch_path);

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
    int status = fr_tool_resolve("daukle-fake-batch", &path, &err);
    char message[sizeof err.message];
    snprintf(message, sizeof message, "%s", err.message);

    fr_test_set_env("PATH", old_path);
    free(old_path);
    free(path);
    fr_test_remove_tree(temp_dir);

    ASSERT(made_temp_dir);
    ASSERT(made_batch);
    ASSERT_EQ(FR_ERR, status);
#ifdef _WIN32
    ASSERT(strstr(message, "cannot run a batch file") != NULL);
    ASSERT(strstr(message, "is not installed") == NULL);
#else
    /* POSIX never treated ".bat" as an extension of the name, so there the
       file simply is not the tool and the ordinary message is the right one. */
    ASSERT(strstr(message, "is not installed") != NULL);
#endif
    PASS();
}

GREATEST_MAIN_DEFS();

static void record_binary_dir(const char *argv_zero) {
    const char *last_slash = strrchr(argv_zero, '/');
    const char *last_backslash = strrchr(argv_zero, '\\');
    const char *end_of_dir = last_slash;
    if (last_backslash != NULL && (end_of_dir == NULL || last_backslash > end_of_dir)) {
        end_of_dir = last_backslash;
    }
    if (end_of_dir == NULL) return;
    size_t length = (size_t) (end_of_dir - argv_zero);
    if (length >= sizeof binary_dir) return;
    memcpy(binary_dir, argv_zero, length);
    binary_dir[length] = '\0';
}

int main(int argc, char **argv) {
    record_binary_dir(argv[0]);
    fr_test_prepend_to_path_dir_of(argv[0]);
    GREATEST_MAIN_BEGIN();
    RUN_TEST(a_tool_on_the_search_path_resolves_to_an_absolute_path);
    RUN_TEST(a_relative_path_entry_still_resolves_to_an_absolute_path);
    RUN_TEST(a_tool_that_is_not_installed_fails_naming_it);
    RUN_TEST(a_directory_shadowing_a_tool_name_does_not_resolve_as_it);
    RUN_TEST(a_batch_file_on_the_search_path_is_refused_as_one);
    GREATEST_MAIN_END();
}
