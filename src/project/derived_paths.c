#include "project/derived.h"

#include "lua/lua_sandbox.h"
#include "util/error.h"
#include "util/tree.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

/* The one spelling of the toolchain-agnostic derived directory's name, so
   fr_derived_root's filesystem path and fr_derived_dir_relative's sandbox-
   relative path cannot drift apart. */
#define FR_DERIVED_ROOT_NAME "build/daukle"
#define FR_GENERATED_ROOT_NAME "daukle"

static int validate_toolchain_name(const char *toolchain, fr_error *err) {
    if (toolchain[0] == '\0' || fr_lua_sandbox_climbs_out(toolchain)
        || strchr(toolchain, '/') != NULL || strchr(toolchain, '\\') != NULL) {
        fr_error_set(err, "toolchain name \"%s\" cannot be a directory name", toolchain);
        return FR_ERR;
    }
    return FR_OK;
}

int fr_derived_root(const char *manifest_dir, char **out_dir, fr_error *err) {
    *out_dir = NULL;
    const char *base = (manifest_dir == NULL || manifest_dir[0] == '\0') ? "." : manifest_dir;

    size_t length = strlen(base) + 1 + strlen(FR_DERIVED_ROOT_NAME) + 1;
    char *path = malloc(length);
    if (path == NULL) {
        fr_error_set(err, "out of memory building the derived root");
        return FR_ERR;
    }
    snprintf(path, length, "%s/%s", base, FR_DERIVED_ROOT_NAME);

    *out_dir = path;
    return FR_OK;
}

int fr_derived_dir(const char *manifest_dir, const char *toolchain, char **out_dir, fr_error *err) {
    *out_dir = NULL;
    if (validate_toolchain_name(toolchain, err) != FR_OK) return FR_ERR;

    char *root = NULL;
    if (fr_derived_root(manifest_dir, &root, err) != FR_OK) return FR_ERR;

    size_t length = strlen(root) + 1 + strlen(toolchain) + 1;
    char *path = malloc(length);
    if (path == NULL) {
        free(root);
        fr_error_set(err, "out of memory building the derived directory for \"%s\"", toolchain);
        return FR_ERR;
    }
    snprintf(path, length, "%s/%s", root, toolchain);
    free(root);

    *out_dir = path;
    return FR_OK;
}

int fr_derived_dir_relative(const char *toolchain, char **out_dir, fr_error *err) {
    *out_dir = NULL;
    if (validate_toolchain_name(toolchain, err) != FR_OK) return FR_ERR;

    size_t length = strlen(FR_DERIVED_ROOT_NAME) + 1 + strlen(toolchain) + 1;
    char *path = malloc(length);
    if (path == NULL) {
        fr_error_set(err, "out of memory building the derived directory for \"%s\"", toolchain);
        return FR_ERR;
    }
    snprintf(path, length, "%s/%s", FR_DERIVED_ROOT_NAME, toolchain);

    *out_dir = path;
    return FR_OK;
}

int fr_generated_dir_relative(const char *toolchain, char **out_dir, fr_error *err) {
    *out_dir = NULL;
    if (validate_toolchain_name(toolchain, err) != FR_OK) return FR_ERR;

    size_t length = strlen(FR_GENERATED_ROOT_NAME) + 1 + strlen(toolchain) + 1;
    char *path = malloc(length);
    if (path == NULL) {
        fr_error_set(err, "out of memory building the generated directory for \"%s\"",
                     toolchain);
        return FR_ERR;
    }
    snprintf(path, length, "%s/%s", FR_GENERATED_ROOT_NAME, toolchain);

    *out_dir = path;
    return FR_OK;
}

#ifdef _WIN32
static int derived_root_exists(const char *path) {
    DWORD attributes = GetFileAttributesA(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

static int derived_root_is_link(const char *path) {
    DWORD attributes = GetFileAttributesA(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}
#else
static int derived_root_exists(const char *path) {
    struct stat info;
    return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

static int derived_root_is_link(const char *path) {
    struct stat info;
    return lstat(path, &info) == 0 && S_ISLNK(info.st_mode);
}
#endif

int fr_derived_root_is_contained(const char *canonical_base, const char *canonical_root) {
    size_t base_length = strlen(canonical_base);
    if (strncmp(canonical_base, canonical_root, base_length) != 0) return 0;
    char boundary = canonical_root[base_length];
    return boundary == '/' || boundary == '\\';
}

/* The files a migration moved aside live nowhere else until "migrate --done",
   so deleting the derived root would delete the user's only copy of them. */
static int holds_unfinished_migration(const char *root, fr_error *err) {
    size_t length = strlen(root) + sizeof "/" FR_DERIVED_MIGRATE_OLD_NAME;
    char *old = malloc(length);
    if (old == NULL) {
        fr_error_set(err, "out of memory checking for an unfinished migration");
        return 1;
    }
    snprintf(old, length, "%s/%s", root, FR_DERIVED_MIGRATE_OLD_NAME);
    int unfinished = derived_root_exists(old);
    free(old);
    if (unfinished) {
        fr_error_set(err, "build/daukle/migrate/old holds the files a migration replaced; run"
                          " \"daukle migrate --done\" to delete them, or move them out, before"
                          " cleaning");
    }
    return unfinished;
}

int fr_derived_clean(const char *manifest_dir, fr_error *err) {
    char *root = NULL;
    if (fr_derived_root(manifest_dir, &root, err) != FR_OK) return FR_ERR;

    if (!derived_root_exists(root)) {
        free(root);
        return FR_OK;
    }

    const char *base = (manifest_dir == NULL || manifest_dir[0] == '\0') ? "." : manifest_dir;

    if (holds_unfinished_migration(root, err)) {
        free(root);
        return FR_ERR;
    }

    if (derived_root_is_link(root)) {
        fr_error_set(err, "the derived directory for \"%s\" is a link; refusing to delete anything",
                     base);
        free(root);
        return FR_ERR;
    }

    char *canonical_base = NULL;
    if (fr_lua_sandbox_canonical_dir(base, &canonical_base, err) != FR_OK) {
        free(root);
        return FR_ERR;
    }

    char *canonical_root = NULL;
    int status = fr_lua_sandbox_canonical_dir(root, &canonical_root, err);
    if (status == FR_OK && !fr_derived_root_is_contained(canonical_base, canonical_root)) {
        fr_error_set(err, "the derived directory for \"%s\" escapes it; refusing to delete anything",
                     base);
        status = FR_ERR;
    }
    free(canonical_base);
    free(canonical_root);
    if (status != FR_OK) {
        free(root);
        return FR_ERR;
    }

    fr_remove_tree(root);
    int still_there = derived_root_exists(root);
    if (still_there) fr_error_set(err, "could not fully remove \"%s\"", root);
    free(root);
    return still_there ? FR_ERR : FR_OK;
}
