#include "plugins_remote.h"

#include "cJSON.h"
#include "cache.h"
#include "error.h"
#include "http.h"
#include "region.h"
#include "semver.h"

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
#endif

/* Refuses before any caller builds a path from repo, since every path this
   module lays out under the cache root splices it in whole. */
static int check_repo_shape(const char *repo, fr_error *err) {
    const char *slash = strchr(repo, '/');
    if (slash == NULL || slash == repo || slash[1] == '\0') {
        fr_error_set(err, "repo \"%s\" must be \"owner/name\"", repo);
        return FR_ERR;
    }
    if (!fr_cache_component_is_safe(repo, 1)) {
        fr_error_set(err, "repo \"%s\" is not a safe cache path component", repo);
        return FR_ERR;
    }
    return FR_OK;
}

static int split_repo(const char *repo, char **out_owner, char **out_name, fr_error *err) {
    if (check_repo_shape(repo, err) != FR_OK) return FR_ERR;

    const char *slash = strchr(repo, '/');
    *out_owner = dup_prefix(repo, (size_t) (slash - repo));
    *out_name = dup_string(slash + 1);
    if (*out_owner == NULL || *out_name == NULL) {
        free(*out_owner);
        free(*out_name);
        fr_error_set(err, "out of memory splitting repo \"%s\"", repo);
        return FR_ERR;
    }
    return FR_OK;
}

static int version_is_greater(const fr_version *a, const fr_version *b) {
    if (a->major != b->major) return a->major > b->major;
    if (a->minor != b->minor) return a->minor > b->minor;
    return a->patch > b->patch;
}

static void format_version(const fr_version *version, char *out, size_t out_size) {
    snprintf(out, out_size, "%d.%d.%d", version->major, version->minor, version->patch);
}

typedef void (*fr_dir_name_fn)(const char *name, void *state);

/* Only src/plugins_remote.c and test/support.c enumerate directories, and this
   one may not include the test header, so its own small copy stays here. Visits
   directories only, passing each one's bare name (not a full path), which is
   all find_cached_version below needs. fr_plugins_remove_cache's own tree walk
   further down needs files as well as directories, and full child paths to
   recurse with, so it keeps its own, differently-shaped helper rather than
   reusing this one. */
static void for_each_subdirectory(const char *path, fr_dir_name_fn visit, void *state) {
#ifdef _WIN32
    char pattern[1024];
    int written = snprintf(pattern, sizeof pattern, "%s/*", path);
    if (written < 0 || (size_t) written >= sizeof pattern) return;

    WIN32_FIND_DATAA found;
    HANDLE handle = FindFirstFileA(pattern, &found);
    if (handle == INVALID_HANDLE_VALUE) return;
    do {
        if (strcmp(found.cFileName, ".") == 0 || strcmp(found.cFileName, "..") == 0) continue;
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) continue;
        visit(found.cFileName, state);
    } while (FindNextFileA(handle, &found));
    FindClose(handle);
#else
    DIR *dir = opendir(path);
    if (dir == NULL) return;
    const struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char child[1024];
        int written = snprintf(child, sizeof child, "%s/%s", path, entry->d_name);
        if (written < 0 || (size_t) written >= sizeof child) continue;
        struct stat info;
        if (stat(child, &info) != 0 || !S_ISDIR(info.st_mode)) continue;
        visit(entry->d_name, state);
    }
    closedir(dir);
#endif
}

static char *join_path(const char *dir, const char *file_name) {
    size_t length = strlen(dir) + 1 + strlen(file_name) + 1;
    char *path = malloc(length);
    if (path != NULL) snprintf(path, length, "%s/%s", dir, file_name);
    return path;
}

static int build_plugin_cache_dir(const char *owner, const char *name, const char *version,
                                  char **out_dir, fr_error *err) {
    char root[1024];
    if (fr_cache_root(root, sizeof root, err) != FR_OK) return FR_ERR;

    size_t length = strlen(root) + strlen("/plugins/") + strlen(owner) + 1
                  + strlen(name) + 1 + strlen(version) + 1;
    char *dir = malloc(length);
    if (dir == NULL) {
        fr_error_set(err, "out of memory building plugin cache path for \"%s/%s\"", owner, name);
        return FR_ERR;
    }
    snprintf(dir, length, "%s/plugins/%s/%s/%s", root, owner, name, version);
    *out_dir = dir;
    return FR_OK;
}

typedef struct {
    const fr_range *range;
    int have_best;
    fr_version best;
} version_scan_state;

static void consider_cached_version(const char *name, void *state_ptr) {
    version_scan_state *state = state_ptr;
    fr_version version;
    fr_error ignored;
    if (fr_version_parse(name, &version, &ignored) != FR_OK) return;
    if (!fr_range_satisfies(state->range, &version)) return;
    if (!state->have_best || version_is_greater(&version, &state->best)) {
        state->best = version;
        state->have_best = 1;
    }
}

/* Scans the cache before any request is made: a hit here means the caller
   never touches the network at all. Obeys --no-cache the same way
   fr_cache_read does, so a disabled cache never serves a plugin either. */
static int find_cached_version(const char *owner, const char *name, const fr_range *range,
                               fr_version *out_version, int *out_found, fr_error *err) {
    *out_found = 0;
    if (!fr_cache_enabled()) return FR_OK;

    char root[1024];
    if (fr_cache_root(root, sizeof root, err) != FR_OK) return FR_ERR;

    char dir[1024];
    int written = snprintf(dir, sizeof dir, "%s/plugins/%s/%s", root, owner, name);
    if (written < 0 || (size_t) written >= sizeof dir) {
        fr_error_set(err, "plugin cache directory for \"%s/%s\" is too long", owner, name);
        return FR_ERR;
    }

    version_scan_state state;
    state.range = range;
    state.have_best = 0;
    for_each_subdirectory(dir, consider_cached_version, &state);

    *out_found = state.have_best;
    if (state.have_best) *out_version = state.best;
    return FR_OK;
}

/* Best effort, like fr_cache_write: a plugin that fetched fine but failed to
   cache must still load, not fail the whole resolve. Written code, not a
   manifest, so a torn write matters more here than for a cached daukle.json:
   fr_cache_write_atomic's tmp-then-rename means a reader never sees a partial
   plugin.lua that still happens to parse as valid, truncated Lua. */
static void write_plugin_cache(const char *owner, const char *name, const char *version,
                               const char *text, size_t length) {
    if (!fr_cache_enabled()) return;

    fr_error ignored;
    char *dir = NULL;
    if (build_plugin_cache_dir(owner, name, version, &dir, &ignored) != FR_OK) return;

    char *file_path = join_path(dir, "plugin.lua");
    free(dir);
    if (file_path == NULL) return;

    fr_cache_write_atomic(file_path, text, length);
    free(file_path);
}

/* Same header shape source_github.c uses for GITHUB_TOKEN: an Authorization
   bearer header, so daukle speaks to GitHub one way. */
static char *build_github_auth_header(void) {
    const char *token = getenv("GITHUB_TOKEN");
    if (token == NULL || token[0] == '\0') return NULL;

    size_t length = strlen("Bearer ") + strlen(token) + 1;
    char *value = malloc(length);
    if (value != NULL) snprintf(value, length, "Bearer %s", token);
    return value;
}

static int fetch_from_github(const fr_plugin_entry *entry, const char *owner, const char *name,
                             const fr_range *range, char **out_text, char **out_origin,
                             fr_error *err) {
    char url[512];
    int written = snprintf(url, sizeof url, "https://api.github.com/repos/%s/%s/releases",
                           owner, name);
    if (written < 0 || (size_t) written >= sizeof url) {
        fr_error_set(err, "plugin \"%s\": release url is too long", entry->label);
        return FR_ERR;
    }

    char *auth_value = build_github_auth_header();
    fr_http_header header;
    const fr_http_header *headers = NULL;
    size_t header_count = 0;
    if (auth_value != NULL) {
        header.name = "Authorization";
        header.value = auth_value;
        headers = &header;
        header_count = 1;
    }

    char *body = NULL;
    size_t length = 0;
    if (fr_http_get(url, headers, header_count, &body, &length, err) != FR_OK) {
        free(auth_value);
        return FR_ERR;
    }

    cJSON *releases = cJSON_ParseWithLength(body, length);
    free(body);
    if (releases == NULL || !cJSON_IsArray(releases)) {
        fr_error_set(err, "plugin \"%s\": releases response is not a json array", entry->label);
        cJSON_Delete(releases);
        free(auth_value);
        return FR_ERR;
    }

    int have_best = 0;
    fr_version best_version = { 0, 0, 0 };
    cJSON *best_release = NULL;
    cJSON *release = NULL;
    /* A release whose tag is not a version (a doc tag, a marker) is skipped
       rather than treated as an error: not every tag is a release of this plugin. */
    cJSON_ArrayForEach(release, releases) {
        cJSON *tag_item = cJSON_GetObjectItemCaseSensitive(release, "tag_name");
        if (!cJSON_IsString(tag_item) || tag_item->valuestring == NULL) continue;

        fr_version version;
        fr_error ignored;
        if (fr_version_parse(tag_item->valuestring, &version, &ignored) != FR_OK) continue;
        if (!fr_range_satisfies(range, &version)) continue;
        if (!have_best || version_is_greater(&version, &best_version)) {
            best_version = version;
            best_release = release;
            have_best = 1;
        }
    }

    if (!have_best) {
        fr_error_set(err, "plugin \"%s\": no release of \"%s/%s\" satisfies \"%s\"",
                    entry->label, owner, name, entry->version);
        cJSON_Delete(releases);
        free(auth_value);
        return FR_ERR;
    }

    const char *asset_url = NULL;
    cJSON *assets = cJSON_GetObjectItemCaseSensitive(best_release, "assets");
    if (cJSON_IsArray(assets)) {
        cJSON *asset = NULL;
        cJSON_ArrayForEach(asset, assets) {
            cJSON *asset_name = cJSON_GetObjectItemCaseSensitive(asset, "name");
            if (!cJSON_IsString(asset_name) || asset_name->valuestring == NULL) continue;
            if (strcmp(asset_name->valuestring, "plugin.lua") != 0) continue;
            cJSON *download = cJSON_GetObjectItemCaseSensitive(asset, "browser_download_url");
            if (cJSON_IsString(download) && download->valuestring != NULL) asset_url = download->valuestring;
            break;
        }
    }

    if (asset_url == NULL) {
        fr_error_set(err, "plugin \"%s\": release has no plugin.lua asset", entry->label);
        cJSON_Delete(releases);
        free(auth_value);
        return FR_ERR;
    }

    char *plugin_body = NULL;
    size_t plugin_length = 0;
    int status = fr_http_get(asset_url, headers, header_count, &plugin_body, &plugin_length, err);
    free(auth_value);
    if (status != FR_OK) {
        cJSON_Delete(releases);
        return FR_ERR;
    }
    cJSON_Delete(releases);

    char *text = malloc(plugin_length + 1);
    if (text == NULL) {
        free(plugin_body);
        fr_error_set(err, "plugin \"%s\": out of memory reading plugin body", entry->label);
        return FR_ERR;
    }
    memcpy(text, plugin_body, plugin_length);
    text[plugin_length] = '\0';
    free(plugin_body);

    char version_text[64];
    format_version(&best_version, version_text, sizeof version_text);
    write_plugin_cache(owner, name, version_text, text, plugin_length);

    char *dir = NULL;
    if (build_plugin_cache_dir(owner, name, version_text, &dir, err) != FR_OK) {
        free(text);
        return FR_ERR;
    }
    char *origin = join_path(dir, "plugin.lua");
    free(dir);
    if (origin == NULL) {
        free(text);
        fr_error_set(err, "plugin \"%s\": out of memory building plugin origin", entry->label);
        return FR_ERR;
    }

    *out_text = text;
    *out_origin = origin;
    return FR_OK;
}

int fr_plugins_resolve_remote(const fr_plugin_entry *entry, char **out_text, char **out_origin,
                              fr_error *err) {
    char *owner = NULL;
    char *name = NULL;
    if (split_repo(entry->repo, &owner, &name, err) != FR_OK) return FR_ERR;

    fr_range range;
    if (fr_range_parse(entry->version, &range, err) != FR_OK) {
        free(owner);
        free(name);
        return FR_ERR;
    }

    fr_version cached_version;
    int found = 0;
    if (find_cached_version(owner, name, &range, &cached_version, &found, err) != FR_OK) {
        free(owner);
        free(name);
        return FR_ERR;
    }

    int status;
    if (found) {
        char version_text[64];
        format_version(&cached_version, version_text, sizeof version_text);

        char *dir = NULL;
        status = build_plugin_cache_dir(owner, name, version_text, &dir, err);
        if (status == FR_OK) {
            char *file_path = join_path(dir, "plugin.lua");
            free(dir);
            if (file_path == NULL) {
                fr_error_set(err, "plugin \"%s\": out of memory building cache path", entry->label);
                status = FR_ERR;
            } else {
                status = fr_file_read_text(file_path, out_text, err);
                if (status == FR_OK) *out_origin = file_path;
                else free(file_path);
            }
        }
    } else {
        status = fetch_from_github(entry, owner, name, &range, out_text, out_origin, err);
    }

    free(owner);
    free(name);
    return status;
}

typedef void (*fr_cache_visit_fn)(const char *child_path, int is_directory, void *state);

/* Unlike for_each_subdirectory above, this visits files as well as directories
   and passes each one's full child path rather than a bare name, since
   remove_cache_tree below needs both to delete a whole plugin cache
   directory, not just read the version names one level below it. */
static void for_each_cache_child(const char *path, fr_cache_visit_fn visit, void *state) {
#ifdef _WIN32
    char pattern[1024];
    int written = snprintf(pattern, sizeof pattern, "%s/*", path);
    if (written < 0 || (size_t) written >= sizeof pattern) return;

    WIN32_FIND_DATAA found;
    HANDLE handle = FindFirstFileA(pattern, &found);
    if (handle == INVALID_HANDLE_VALUE) return;
    do {
        if (strcmp(found.cFileName, ".") == 0 || strcmp(found.cFileName, "..") == 0) continue;
        char child[1024];
        int child_written = snprintf(child, sizeof child, "%s/%s", path, found.cFileName);
        if (child_written < 0 || (size_t) child_written >= sizeof child) continue;
        visit(child, (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, state);
    } while (FindNextFileA(handle, &found));
    FindClose(handle);
#else
    DIR *dir = opendir(path);
    if (dir == NULL) return;
    const struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char child[1024];
        int child_written = snprintf(child, sizeof child, "%s/%s", path, entry->d_name);
        if (child_written < 0 || (size_t) child_written >= sizeof child) continue;
        struct stat info;
        if (stat(child, &info) != 0) continue;
        visit(child, S_ISDIR(info.st_mode), state);
    }
    closedir(dir);
#endif
}

static void remove_cache_tree(const char *path);

static void remove_cache_child(const char *child_path, int is_directory, void *state) {
    (void) state;
    if (is_directory) remove_cache_tree(child_path);
    else remove(child_path);
}

/* Best effort throughout, like fr_cache_write: a directory that was never created
   (nothing of this plugin was ever fetched) is not an error, and a stray file the
   OS would not let go of must not fail the whole "plugin update". */
static void remove_cache_tree(const char *path) {
    for_each_cache_child(path, remove_cache_child, NULL);
#ifdef _WIN32
    _rmdir(path);
#else
    rmdir(path);
#endif
}

void fr_plugins_discard_cached_version(const char *origin) {
    const char *file_slash = strrchr(origin, '/');
    if (file_slash == NULL) return;

    size_t length = (size_t) (file_slash - origin);
    char dir[1024];
    if (length == 0 || length >= sizeof dir) return;
    memcpy(dir, origin, length);
    dir[length] = '\0';

    remove_cache_tree(dir);
}

int fr_plugins_remove_cache(const char *repo, fr_error *err) {
    if (check_repo_shape(repo, err) != FR_OK) return FR_ERR;

    char root[1024];
    if (fr_cache_root(root, sizeof root, err) != FR_OK) return FR_ERR;

    char dir[1024];
    int written = snprintf(dir, sizeof dir, "%s/plugins/%s", root, repo);
    if (written < 0 || (size_t) written >= sizeof dir) {
        fr_error_set(err, "plugin cache directory for \"%s\" is too long", repo);
        return FR_ERR;
    }

    remove_cache_tree(dir);
    return FR_OK;
}
