#ifndef DAUKLE_DETECT_INTERNAL_H
#define DAUKLE_DETECT_INTERNAL_H

#include "migrate/detect.h"

#include "lua.h"

typedef struct {
    const fr_catalog *catalog;
    const char *const *with;
    size_t with_count;
    lua_State *patterns;
    fr_detections *found;
    size_t directories;
} fr_detect_run;

typedef struct {
    char **files;
    size_t file_count;
    char **subdirectories;
    size_t subdirectory_count;
    int failed;
} fr_detect_listing;

/* A heap copy of the first length bytes of text, or NULL when out of memory. */
char *fr_detect_copy(const char *text, size_t length);

/* Returns a new string "<base>/<name>", or a copy of name when base is empty. */
char *fr_detect_join(const char *base, const char *name);

int fr_detect_is_with(const fr_detect_run *run, const fr_catalog_plugin *plugin);

/* Records plugin at dir unless it is already recorded there. */
int fr_detections_add(fr_detections *detections, const fr_catalog_plugin *plugin, const char *dir,
                      fr_error *err);

/* Evaluates every rule that admits relative_dir against its listing, settles
   the claims on each file and records the winners in run->found. directory is
   the same place as a path that can be opened. */
int fr_detect_directory(fr_detect_run *run, const char *directory, const char *relative_dir,
                        const fr_detect_listing *listing, fr_error *err);

/* Visits root and, depth-first, every directory below it. */
int fr_detect_walk(fr_detect_run *run, const char *root, fr_error *err);

#endif
