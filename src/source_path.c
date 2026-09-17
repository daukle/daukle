#include "source_path.h"

#include "error.h"
#include "jsonx.h"
#include "manifest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *join_manifest_path(const char *base_dir, const char *relative_path) {
    if (base_dir == NULL || base_dir[0] == '\0') base_dir = ".";
    size_t length = strlen(base_dir) + 1 + strlen(relative_path) + 1 + strlen("tiestone.json") + 1;
    char *joined = malloc(length);
    if (joined != NULL) snprintf(joined, length, "%s/%s/tiestone.json", base_dir, relative_path);
    return joined;
}

static int source_path_load(void *state, const char *project, const cJSON *block,
                            const char *base_dir, fr_project *out, fr_error *err) {
    (void) state;
    memset(out, 0, sizeof *out);

    char path[256];
    snprintf(path, sizeof path, "sources.%s", project);
    const char *relative_path = NULL;
    if (fr_json_string(block, "path", path, &relative_path, err) != FR_OK) return FR_ERR;

    char *file_path = join_manifest_path(base_dir, relative_path);
    if (file_path == NULL) {
        fr_error_set(err, "out of memory joining path for \"%s\"", project);
        return FR_ERR;
    }
    int result = fr_project_read(file_path, out, err);
    free(file_path);
    return result;
}

const fr_source_plugin FR_SOURCE_PATH = { "tiestone.source/path", source_path_load, NULL };
