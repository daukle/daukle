#include "migrate/detect_internal.h"

#include "util/error.h"
#include "util/tree.h"

#include <stdlib.h>
#include <string.h>

#define DERIVED_ROOT_RELATIVE_PATH "build/daukle"

static int push_name(char ***names, size_t *count, const char *name) {
    char **grown = realloc(*names, (*count + 1) * sizeof **names);
    if (grown == NULL) return FR_ERR;
    *names = grown;
    char *copy = fr_detect_copy(name, strlen(name));
    if (copy == NULL) return FR_ERR;
    grown[(*count)++] = copy;
    return FR_OK;
}

static void collect_entry(const char *name, int is_directory, int is_link, void *context) {
    fr_detect_listing *listing = context;
    if (listing->failed) return;
    int rc;
    if (!is_directory) {
        rc = push_name(&listing->files, &listing->file_count, name);
    } else if (is_link || name[0] == '.') {
        return;
    } else {
        rc = push_name(&listing->subdirectories, &listing->subdirectory_count, name);
    }
    if (rc != FR_OK) listing->failed = 1;
}

static void free_names(char **names, size_t count) {
    for (size_t index = 0; index < count; index++) free(names[index]);
    free(names);
}

static void free_listing(fr_detect_listing *listing) {
    free_names(listing->files, listing->file_count);
    free_names(listing->subdirectories, listing->subdirectory_count);
}

static int walk_directory(fr_detect_run *run, const char *root, const char *relative_dir, int depth,
                          fr_error *err);

static int walk_children(fr_detect_run *run, const char *root, const char *relative_dir, int depth,
                         const fr_detect_listing *listing, fr_error *err) {
    for (size_t index = 0; index < listing->subdirectory_count; index++) {
        char *child = fr_detect_join(relative_dir, listing->subdirectories[index]);
        if (child == NULL) {
            fr_error_set(err, "out of memory while walking the project for detection");
            return FR_ERR;
        }
        int rc = strcmp(child, DERIVED_ROOT_RELATIVE_PATH) == 0
                     ? FR_OK
                     : walk_directory(run, root, child, depth + 1, err);
        free(child);
        if (rc != FR_OK) return rc;
    }
    return FR_OK;
}

static int walk_directory(fr_detect_run *run, const char *root, const char *relative_dir, int depth,
                          fr_error *err) {
    if (++run->directories > FR_DETECT_MAX_DIRECTORIES) {
        fr_error_set(err,
                     "migrate stopped walking after %d directories; the project is larger than "
                     "detection reads",
                     FR_DETECT_MAX_DIRECTORIES);
        return FR_ERR;
    }
    char *directory = fr_detect_join(root, relative_dir);
    if (directory == NULL) {
        fr_error_set(err, "out of memory while walking the project for detection");
        return FR_ERR;
    }
    fr_detect_listing listing = { 0 };
    fr_list_directory(directory[0] == '\0' ? "." : directory, collect_entry, &listing);
    int rc = FR_OK;
    if (listing.failed) {
        fr_error_set(err, "out of memory while listing %s for detection", directory);
        rc = FR_ERR;
    }
    if (rc == FR_OK) rc = fr_detect_directory(run, directory, relative_dir, &listing, err);
    if (rc == FR_OK && depth < FR_DETECT_MAX_DEPTH) {
        rc = walk_children(run, root, relative_dir, depth, &listing, err);
    }
    free_listing(&listing);
    free(directory);
    return rc;
}

int fr_detect_walk(fr_detect_run *run, const char *root, fr_error *err) {
    return walk_directory(run, root, "", 0, err);
}
