#include "tool.h"

#include "error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define PATH_SEPARATOR ';'
static const char *const EXTENSIONS[] = { ".exe", ".com", "" };
static const char *const BATCH_EXTENSIONS[] = { ".bat", ".cmd" };
#else
#include <sys/stat.h>
#include <unistd.h>
#define PATH_SEPARATOR ':'
static const char *const EXTENSIONS[] = { "" };
#endif

static int is_executable_file(const char *path) {
#ifdef _WIN32
    FILE *probe = fopen(path, "rb");
    if (probe == NULL) return 0;
    fclose(probe);
    return 1;
#else
    /* A permission check alone cannot tell a program from a directory: most
       directories pass X_OK too, since it tests search permission there. */
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode) && access(path, X_OK) == 0;
#endif
}

static char *join_candidate(const char *dir, size_t dir_length, const char *name,
                            const char *extension) {
    size_t size = dir_length + 1 + strlen(name) + strlen(extension) + 1;
    char *candidate = malloc(size);
    if (candidate == NULL) return NULL;
    snprintf(candidate, size, "%.*s/%s%s", (int) dir_length, dir, name, extension);
    return candidate;
}

static char *first_existing(const char *dir, size_t dir_length, const char *name,
                            const char *const *extensions, size_t extension_count,
                            int *out_of_memory) {
    for (size_t index = 0; index < extension_count; index++) {
        char *candidate = join_candidate(dir, dir_length, name, extensions[index]);
        if (candidate == NULL) {
            *out_of_memory = 1;
            return NULL;
        }
        if (is_executable_file(candidate)) return candidate;
        free(candidate);
    }
    return NULL;
}

/* A PATH entry may itself be relative, and the two backends would then
   disagree about what it means: POSIX chdir()s into options.cwd before
   execv(), so the program would resolve against the child's new directory,
   while CreateProcess resolves it against daukle's own. */
static int absolute_form(char *candidate, const char *name, char **out_path, fr_error *err) {
#ifdef _WIN32
    char *absolute = _fullpath(NULL, candidate, 0);
#else
    char *absolute = realpath(candidate, NULL);
#endif
    free(candidate);
    if (absolute == NULL) {
        fr_error_set(err, "\"%s\" was found but its absolute path could not be resolved", name);
        return FR_ERR;
    }
    *out_path = absolute;
    return FR_OK;
}

int fr_tool_resolve(const char *name, char **out_path, fr_error *err) {
    *out_path = NULL;

    const char *search = getenv("PATH");
    if (search == NULL) search = "";

    for (const char *entry = search; ; ) {
        const char *end = strchr(entry, PATH_SEPARATOR);
        size_t length = end == NULL ? strlen(entry) : (size_t) (end - entry);
        if (length > 0) {
            int out_of_memory = 0;
            char *found = first_existing(entry, length, name, EXTENSIONS,
                                         sizeof EXTENSIONS / sizeof EXTENSIONS[0], &out_of_memory);
            if (out_of_memory) {
                fr_error_set(err, "out of memory resolving \"%s\"", name);
                return FR_ERR;
            }
            if (found != NULL) return absolute_form(found, name, out_path, err);
#ifdef _WIN32
            /* Refused by name rather than reported as missing: CreateProcess cannot
               start a batch file at all, and doing so would mean handing a command
               string to cmd.exe, which is the shell design section 4.1 forbids. */
            char *batch = first_existing(entry, length, name, BATCH_EXTENSIONS,
                                         sizeof BATCH_EXTENSIONS / sizeof BATCH_EXTENSIONS[0],
                                         &out_of_memory);
            if (out_of_memory) {
                fr_error_set(err, "out of memory resolving \"%s\"", name);
                return FR_ERR;
            }
            if (batch != NULL) {
                fr_error_set(err, "daukle cannot run a batch file, and \"%s\" resolved to %s",
                            name, batch);
                free(batch);
                return FR_ERR;
            }
#endif
        }
        if (end == NULL) break;
        entry = end + 1;
    }

    fr_error_set(err, "\"%s\" is not installed and could not be found on the search path", name);
    return FR_ERR;
}
