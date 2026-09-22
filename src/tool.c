#include "tool.h"

#include "error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define PATH_SEPARATOR ';'
static const char *const EXTENSIONS[] = { ".exe", ".com", ".bat", ".cmd", "" };
#else
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
    /* fopen succeeds on a directory here, and ignores the execute bit. */
    return access(path, X_OK) == 0;
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

int fr_tool_resolve(const char *name, char **out_path, fr_error *err) {
    *out_path = NULL;

    const char *search = getenv("PATH");
    if (search == NULL) search = "";

    for (const char *entry = search; ; ) {
        const char *end = strchr(entry, PATH_SEPARATOR);
        size_t length = end == NULL ? strlen(entry) : (size_t) (end - entry);
        if (length > 0) {
            for (size_t index = 0; index < sizeof EXTENSIONS / sizeof EXTENSIONS[0]; index++) {
                char *candidate = join_candidate(entry, length, name, EXTENSIONS[index]);
                if (candidate == NULL) {
                    fr_error_set(err, "out of memory resolving \"%s\"", name);
                    return FR_ERR;
                }
                if (is_executable_file(candidate)) {
                    *out_path = candidate;
                    return FR_OK;
                }
                free(candidate);
            }
        }
        if (end == NULL) break;
        entry = end + 1;
    }

    fr_error_set(err, "\"%s\" is not installed and could not be found on the search path", name);
    return FR_ERR;
}
