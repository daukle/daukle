#include "project/toolchain_root.h"

#include "project/derived.h"
#include "util/error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int fr_toolchain_root(const fr_toolchain *toolchain, char **out_root, fr_error *err) {
    const char *dir = toolchain->dir;
    size_t length = strlen(FR_DERIVED_ROOT_RELATIVE) + (dir != NULL ? 1 + strlen(dir) : 0) + 1;
    char *root = malloc(length);
    if (root == NULL) {
        fr_error_set(err, "out of memory building the root of toolchain \"%s\"", toolchain->name);
        return FR_ERR;
    }
    if (dir != NULL) {
        snprintf(root, length, "%s/%s", FR_DERIVED_ROOT_RELATIVE, dir);
    } else {
        snprintf(root, length, "%s", FR_DERIVED_ROOT_RELATIVE);
    }
    *out_root = root;
    return FR_OK;
}
