#include "migrate/detect_internal.h"

#include "util/error.h"

#include "lauxlib.h"
#include "lualib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int fr_detect_name_matches(const char *pattern, const char *name) {
    const char *star_pattern = NULL;
    const char *star_name = NULL;
    while (*name != '\0') {
        if (*pattern == '*') {
            star_pattern = pattern++;
            star_name = name;
        } else if (*pattern == '?' || *pattern == *name) {
            pattern++;
            name++;
        } else if (star_pattern != NULL) {
            pattern = star_pattern + 1;
            name = ++star_name;
        } else {
            return 0;
        }
    }
    while (*pattern == '*') pattern++;
    return *pattern == '\0';
}

char *fr_detect_copy(const char *text, size_t length) {
    char *copy = malloc(length + 1);
    if (copy == NULL) return NULL;
    memcpy(copy, text, length);
    copy[length] = '\0';
    return copy;
}

char *fr_detect_join(const char *base, const char *name) {
    if (name[0] == '\0') return fr_detect_copy(base, strlen(base));
    if (base[0] == '\0') return fr_detect_copy(name, strlen(name));
    size_t length = strlen(base) + 1 + strlen(name) + 1;
    char *joined = malloc(length);
    if (joined == NULL) return NULL;
    snprintf(joined, length, "%s/%s", base, name);
    return joined;
}

int fr_detect_is_with(const fr_detect_run *run, const fr_catalog_plugin *plugin) {
    for (size_t index = 0; index < run->with_count; index++) {
        if (strcmp(run->with[index], plugin->id) == 0) return 1;
    }
    return 0;
}

int fr_detections_add(fr_detections *detections, const fr_catalog_plugin *plugin, const char *dir,
                      fr_error *err) {
    for (size_t index = 0; index < detections->count; index++) {
        if (detections->items[index].plugin == plugin && strcmp(detections->items[index].dir, dir) == 0) {
            return FR_OK;
        }
    }
    fr_detection *grown = realloc(detections->items, (detections->count + 1) * sizeof *grown);
    if (grown == NULL) {
        fr_error_set(err, "out of memory while recording the detection of %s", plugin->id);
        return FR_ERR;
    }
    detections->items = grown;
    char *copy = fr_detect_copy(dir, strlen(dir));
    if (copy == NULL) {
        fr_error_set(err, "out of memory while recording the detection of %s", plugin->id);
        return FR_ERR;
    }
    grown[detections->count].plugin = plugin;
    grown[detections->count].dir = copy;
    detections->count++;
    return FR_OK;
}

void fr_detections_free(fr_detections *detections) {
    for (size_t index = 0; index < detections->count; index++) free(detections->items[index].dir);
    free(detections->items);
    detections->items = NULL;
    detections->count = 0;
}

static int is_strict_ancestor(const char *ancestor, const char *dir) {
    if (ancestor[0] == '\0') return dir[0] != '\0';
    size_t length = strlen(ancestor);
    return strncmp(ancestor, dir, length) == 0 && dir[length] == '/';
}

static int has_outer_detection(const fr_detections *detections, size_t index) {
    const fr_detection *inner = &detections->items[index];
    for (size_t other = 0; other < detections->count; other++) {
        const fr_detection *outer = &detections->items[other];
        if (outer->plugin == inner->plugin && is_strict_ancestor(outer->dir, inner->dir)) return 1;
    }
    return 0;
}

static int collapse_to_outermost(fr_detections *detections, fr_error *err) {
    size_t total = detections->count;
    int *drop = calloc(total > 0 ? total : 1, sizeof *drop);
    if (drop == NULL) {
        fr_error_set(err, "out of memory while collapsing detections");
        return FR_ERR;
    }
    for (size_t index = 0; index < total; index++) drop[index] = has_outer_detection(detections, index);
    size_t kept = 0;
    for (size_t index = 0; index < total; index++) {
        if (drop[index]) {
            free(detections->items[index].dir);
        } else {
            detections->items[kept++] = detections->items[index];
        }
    }
    detections->count = kept;
    free(drop);
    return FR_OK;
}

static int compare_detections(const void *left, const void *right) {
    const fr_detection *first = left;
    const fr_detection *second = right;
    if (first->plugin != second->plugin) return first->plugin < second->plugin ? -1 : 1;
    return strcmp(first->dir, second->dir);
}

static int check_with_ids(const fr_catalog *catalog, const char *const *with, size_t with_count,
                          fr_error *err) {
    for (size_t index = 0; index < with_count; index++) {
        if (fr_catalog_find(catalog, with[index]) != NULL) continue;
        fr_error_set(err, "--with names %s, which no catalog lists", with[index]);
        return FR_ERR;
    }
    return FR_OK;
}

static int add_with_at_root(const fr_catalog *catalog, const char *const *with, size_t with_count,
                            fr_detections *found, fr_error *err) {
    for (size_t index = 0; index < with_count; index++) {
        const fr_catalog_plugin *plugin = fr_catalog_find(catalog, with[index]);
        if (fr_detections_add(found, plugin, "", err) != FR_OK) return FR_ERR;
    }
    return FR_OK;
}

/* @implNote Opened with luaL_newstate and no capped allocator on purpose: it
   runs no plugin code, only string.find. */
static lua_State *open_pattern_state(void) {
    lua_State *state = luaL_newstate();
    if (state == NULL) return NULL;
    luaL_requiref(state, LUA_STRLIBNAME, luaopen_string, 1);
    lua_pop(state, 1);
    return state;
}

int fr_detect(const char *root, const fr_catalog *catalog, const char *const *with,
              size_t with_count, fr_detections *out, fr_error *err) {
    out->items = NULL;
    out->count = 0;
    if (check_with_ids(catalog, with, with_count, err) != FR_OK) return FR_ERR;

    fr_detect_run run = { catalog, with, with_count, NULL, out, 0 };
    run.patterns = open_pattern_state();
    if (run.patterns == NULL) {
        fr_error_set(err, "out of memory while opening the detection pattern matcher");
        return FR_ERR;
    }
    int rc = fr_detect_walk(&run, root, err);
    lua_close(run.patterns);
    if (rc == FR_OK) rc = collapse_to_outermost(out, err);
    if (rc == FR_OK) rc = add_with_at_root(catalog, with, with_count, out, err);
    if (rc != FR_OK) {
        fr_detections_free(out);
        return FR_ERR;
    }
    qsort(out->items, out->count, sizeof *out->items, compare_detections);
    return FR_OK;
}
