#include "cache.h"

#include "error.h"
#include "region.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <sys/stat.h>
#endif

static int CACHE_ENABLED = 1;

void fr_cache_set_enabled(int enabled) {
    CACHE_ENABLED = enabled;
}

int fr_cache_enabled(void) {
    return CACHE_ENABLED;
}

/* Windows silently strips a trailing dot from a path component, so
   "basekit" and "basekit." would otherwise alias to the same directory. */
static int has_trailing_dot_component(const char *text) {
    size_t length = strlen(text);
    if (length == 0) return 0;
    if (text[length - 1] == '.') return 1;
    const char *slash = strchr(text, '/');
    return slash != NULL && slash != text && slash[-1] == '.';
}

static int is_safe_component(const char *text, int allow_one_slash) {
    if (text == NULL || text[0] == '\0') return 0;
    if (text[0] == '/' || text[0] == '\\') return 0;
    if (strstr(text, "..") != NULL) return 0;
    if (strchr(text, '\\') != NULL) return 0;
    if (strchr(text, ':') != NULL) return 0;
    if (has_trailing_dot_component(text)) return 0;

    const char *slash = strchr(text, '/');
    if (slash == NULL) return 1;
    if (!allow_one_slash) return 0;
    return strchr(slash + 1, '/') == NULL && slash[1] != '\0';
}

static int check_root_fit(int written, size_t out_size, fr_error *err) {
    if (written < 0 || (size_t) written >= out_size) {
        fr_error_set(err, "cache root path is too long");
        return FR_ERR;
    }
    return FR_OK;
}

static int cache_root_dir(char *out, size_t out_size, fr_error *err) {
    const char *override = getenv("DAUKLE_CACHE_DIR");
    if (override != NULL && override[0] != '\0') {
        return check_root_fit(snprintf(out, out_size, "%s", override), out_size, err);
    }

#ifdef _WIN32
    const char *local_appdata = getenv("LOCALAPPDATA");
    if (local_appdata != NULL && local_appdata[0] != '\0') {
        return check_root_fit(snprintf(out, out_size, "%s/daukle/cache", local_appdata), out_size, err);
    }
#else
    const char *xdg_cache_home = getenv("XDG_CACHE_HOME");
    if (xdg_cache_home != NULL && xdg_cache_home[0] != '\0') {
        return check_root_fit(snprintf(out, out_size, "%s/daukle", xdg_cache_home), out_size, err);
    }
    const char *home = getenv("HOME");
    if (home != NULL && home[0] != '\0') {
        return check_root_fit(snprintf(out, out_size, "%s/.cache/daukle", home), out_size, err);
    }
#endif

    fr_error_set(err, "no cache directory available: set DAUKLE_CACHE_DIR");
    return FR_ERR;
}

/* Hashed rather than spelled out, because an artifact identity is a url and
   carries characters no filesystem accepts in a path component, at a length no
   filesystem bounds. FNV-1a. */
static void artifact_component(const char *artifact, char out[17]) {
    unsigned long long hash = 14695981039346656037ULL;
    for (const unsigned char *cursor = (const unsigned char *) artifact; *cursor != '\0'; cursor++) {
        hash ^= (unsigned long long) *cursor;
        hash *= 1099511628211ULL;
    }
    snprintf(out, 17, "%016llx", hash);
}

int fr_cache_root(char *out, size_t out_size, fr_error *err) {
    return cache_root_dir(out, out_size, err);
}

int fr_cache_path(const char *project, const char *version, const char *artifact,
                  char **out_path, fr_error *err) {
    *out_path = NULL;
    if (!is_safe_component(project, 1)) {
        fr_error_set(err, "project \"%s\" is not a safe cache path component", project);
        return FR_ERR;
    }
    if (!is_safe_component(version, 0)) {
        fr_error_set(err, "version \"%s\" is not a safe cache path component", version);
        return FR_ERR;
    }
    if (artifact == NULL || artifact[0] == '\0') {
        fr_error_set(err, "cache entry for \"%s\" names no artifact", project);
        return FR_ERR;
    }

    char root[1024];
    if (cache_root_dir(root, sizeof root, err) != FR_OK) return FR_ERR;

    char artifact_id[17];
    artifact_component(artifact, artifact_id);

    size_t length = strlen(root) + 1 + strlen(project) + 1 + strlen(version) + 1
                  + strlen(artifact_id) + 1 + strlen("daukle.json") + 1;
    char *path = malloc(length);
    if (path == NULL) {
        fr_error_set(err, "out of memory building cache path");
        return FR_ERR;
    }
    snprintf(path, length, "%s/%s/%s/%s/daukle.json", root, project, version, artifact_id);

    *out_path = path;
    return FR_OK;
}

int fr_cache_read(const char *project, const char *version, const char *artifact,
                  char **out_text, fr_error *err) {
    *out_text = NULL;
    if (!CACHE_ENABLED) return FR_OK;

    char *path = NULL;
    if (fr_cache_path(project, version, artifact, &path, err) != FR_OK) return FR_ERR;

    FILE *probe = fopen(path, "rb");
    if (probe == NULL) { free(path); return FR_OK; }
    fclose(probe);

    int result = fr_file_read_text(path, out_text, err);
    free(path);
    return result;
}

static void make_directory(const char *path) {
#ifdef _WIN32
    if (_mkdir(path) == 0 || errno == EEXIST) return;
#else
    if (mkdir(path, 0777) == 0 || errno == EEXIST) return;
#endif
}

/* Walks every '/' in the full cache-entry path, creating each ancestor
   directory in turn (root included). Failures are ignored here: a directory
   that could not be created simply makes the later fopen fail too, and that
   failure is swallowed by the caller, never the caller's caller. */
static void make_parent_directories(char *path) {
    for (char *slash = strchr(path, '/'); slash != NULL; slash = strchr(slash + 1, '/')) {
        *slash = '\0';
        make_directory(path);
        *slash = '/';
    }
}

static int rename_into_place(const char *temp_path, const char *final_path) {
#ifdef _WIN32
    return MoveFileExA(temp_path, final_path, MOVEFILE_REPLACE_EXISTING) ? FR_OK : FR_ERR;
#else
    return rename(temp_path, final_path) == 0 ? FR_OK : FR_ERR;
#endif
}

/* Writes to a sibling "<path>.tmp" first and only renames it over the final
   path once the full text is confirmed on disk, so a short write (full disk,
   killed process) never leaves a truncated file where a reader looks. Shared
   by fr_cache_write's own project/version/artifact layout and by any other
   cache shape that needs the same guarantee (see fr_cache_root); every step
   fails without touching the destination, so a caller that ignores the
   result is left with either the old file or no file, never a partial one. */
int fr_cache_write_atomic(char *path, const char *text, size_t length) {
    make_parent_directories(path);

    size_t temp_path_size = strlen(path) + strlen(".tmp") + 1;
    char *temp_path = malloc(temp_path_size);
    if (temp_path == NULL) return FR_ERR;
    snprintf(temp_path, temp_path_size, "%s.tmp", path);

    FILE *file = fopen(temp_path, "wb");
    if (file == NULL) { free(temp_path); return FR_ERR; }

    size_t written = fwrite(text, 1, length, file);
    int close_result = fclose(file);

    int result = FR_OK;
    if (written != length || close_result != 0) {
        remove(temp_path);
        result = FR_ERR;
    } else if (rename_into_place(temp_path, path) != FR_OK) {
        remove(temp_path);
        result = FR_ERR;
    }

    free(temp_path);
    return result;
}

/* Every step fails silently: a cache write must never fail the caller. */
void fr_cache_write(const char *project, const char *version, const char *artifact,
                    const char *text, size_t length) {
    if (!CACHE_ENABLED) return;

    fr_error err;
    char *path = NULL;
    if (fr_cache_path(project, version, artifact, &path, &err) != FR_OK) return;

    fr_cache_write_atomic(path, text, length);
    free(path);
}
