#include "plugins.h"

#include "cJSON.h"
#include "cache.h"
#include "config_lua.h"
#include "error.h"
#include "http.h"
#include "jsonx.h"
#include "lua_sandbox.h"
#include "lua_verbs.h"
#include "luax.h"
#include "region.h"
#include "semver.h"

#include "lauxlib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#endif

static char *dup_string(const char *text) {
    if (text == NULL) return NULL;
    size_t length = strlen(text) + 1;
    char *copy = malloc(length);
    if (copy != NULL) memcpy(copy, text, length);
    return copy;
}

static char *dup_prefix(const char *text, size_t length) {
    char *copy = malloc(length + 1);
    if (copy == NULL) return NULL;
    memcpy(copy, text, length);
    copy[length] = '\0';
    return copy;
}

/* A bare "owner/name" without "@range" is rejected rather than guessed at,
   so a manifest typo surfaces as a diagnostic instead of an unpinned fetch. */
static int parse_string_form(const char *label, const char *value, fr_plugin_entry *out,
                             fr_error *err) {
    if (value[0] == '.' || value[0] == '/') {
        out->kind = FR_PLUGIN_LOCAL;
        out->path = dup_string(value);
        return out->path != NULL ? FR_OK : FR_ERR;
    }

    const char *at = strchr(value, '@');
    if (at == NULL) {
        fr_error_set(err, "plugin \"%s\": \"%s\" needs a version, as \"owner/name@range\","
                          " or a path beginning with \".\" or \"/\"", label, value);
        return FR_ERR;
    }

    size_t repo_length = (size_t) (at - value);
    if (repo_length == 0 || at[1] == '\0') {
        fr_error_set(err, "plugin \"%s\": \"%s\" needs both a repo and a version around \"@\"",
                          label, value);
        return FR_ERR;
    }

    out->kind = FR_PLUGIN_REMOTE;
    out->repo = dup_prefix(value, repo_length);
    out->version = dup_string(at + 1);
    return (out->repo != NULL && out->version != NULL) ? FR_OK : FR_ERR;
}

static int parse_table_form(const char *label, const cJSON *member, fr_plugin_entry *out,
                            fr_error *err) {
    const char *repo = NULL;
    const char *version = NULL;
    if (fr_json_string(member, "repo", label, &repo, err) != FR_OK) return FR_ERR;
    if (fr_json_string(member, "version", label, &version, err) != FR_OK) return FR_ERR;

    const char *sha256 = NULL;
    if (cJSON_GetObjectItemCaseSensitive(member, "sha256") != NULL) {
        if (fr_json_string(member, "sha256", label, &sha256, err) != FR_OK) return FR_ERR;
    }

    out->kind = FR_PLUGIN_REMOTE;
    out->repo = dup_string(repo);
    out->version = dup_string(version);
    out->sha256 = sha256 != NULL ? dup_string(sha256) : NULL;
    if (out->repo == NULL || out->version == NULL || (sha256 != NULL && out->sha256 == NULL)) {
        return FR_ERR;
    }
    return FR_OK;
}

int fr_plugins_parse(const struct cJSON *document, fr_plugin_entry **out, size_t *out_count,
                     fr_error *err) {
    *out = NULL;
    *out_count = 0;

    const cJSON *plugins = cJSON_GetObjectItemCaseSensitive(document, "plugins");
    if (plugins == NULL) return FR_OK;

    if (!cJSON_IsObject(plugins)) {
        fr_error_set(err, "plugins must be a table");
        return FR_ERR;
    }

    int size = cJSON_GetArraySize(plugins);
    if (size == 0) return FR_OK;

    fr_plugin_entry *entries = calloc((size_t) size, sizeof *entries);
    if (entries == NULL) {
        fr_error_set(err, "out of memory reading plugins");
        return FR_ERR;
    }

    size_t count = 0;
    const cJSON *member = NULL;
    cJSON_ArrayForEach(member, plugins) {
        const char *label = member->string;
        count = count + 1;

        fr_plugin_entry *slot = &entries[count - 1];
        slot->label = dup_string(label);
        if (slot->label == NULL) {
            fr_error_set(err, "out of memory reading plugin \"%s\"", label);
            fr_plugins_free(entries, count);
            return FR_ERR;
        }

        int status;
        if (cJSON_IsString(member) && member->valuestring != NULL) {
            status = parse_string_form(label, member->valuestring, slot, err);
        } else if (cJSON_IsObject(member)) {
            status = parse_table_form(label, member, slot, err);
        } else {
            fr_error_set(err, "plugin \"%s\" must be a string or a table", label);
            status = FR_ERR;
        }

        if (status != FR_OK) {
            fr_plugins_free(entries, count);
            return FR_ERR;
        }
    }

    *out = entries;
    *out_count = count;
    return FR_OK;
}

void fr_plugins_free(fr_plugin_entry *entries, size_t count) {
    if (entries == NULL) return;
    for (size_t index = 0; index < count; index++) {
        free(entries[index].label);
        free(entries[index].path);
        free(entries[index].repo);
        free(entries[index].version);
        free(entries[index].sha256);
    }
    free(entries);
}

#define FR_PLUGIN_MAX_USES 16

/* Raised to stop the chunk the moment daukle.plugin has been read, so catching it
   distinguishes a declaration that was read from a plugin that really failed. */
#define FR_PLUGIN_DECLARATION_READ "daukle: plugin declaration read"

typedef struct {
    const char *label;
    char *uses[FR_PLUGIN_MAX_USES];
    size_t uses_count;
} fr_plugin_declaration;

static fr_plugin_declaration *declaration_in_progress;

static void free_declaration(fr_plugin_declaration *declaration) {
    for (size_t index = 0; index < declaration->uses_count; index++) {
        free(declaration->uses[index]);
    }
    declaration->uses_count = 0;
}

static int record_uses(lua_State *state, fr_plugin_declaration *declaration) {
    if (lua_isnil(state, -1)) return 0;
    if (!lua_istable(state, -1)) {
        return luaL_error(state, "plugin \"%s\": uses must be a list of verb names",
                          declaration->label);
    }

    lua_Integer length = (lua_Integer) lua_rawlen(state, -1);
    for (lua_Integer index = 1; index <= length; index++) {
        lua_rawgeti(state, -1, index);
        if (lua_type(state, -1) != LUA_TSTRING) {
            return luaL_error(state, "plugin \"%s\": every name in uses must be a string",
                              declaration->label);
        }
        const char *name = lua_tostring(state, -1);
        if (!fr_lua_verbs_is_known(name)) {
            return luaL_error(state, "plugin \"%s\": \"%s\" is not a daukle verb",
                              declaration->label, name);
        }
        if (declaration->uses_count == FR_PLUGIN_MAX_USES) {
            return luaL_error(state, "plugin \"%s\": uses names more than %d verbs",
                              declaration->label, FR_PLUGIN_MAX_USES);
        }
        char *copy = dup_string(name);
        if (copy == NULL) {
            return luaL_error(state, "plugin \"%s\": out of memory reading uses",
                              declaration->label);
        }
        declaration->uses[declaration->uses_count] = copy;
        declaration->uses_count++;
        lua_pop(state, 1);
    }
    return 0;
}

static int declare_plugin(lua_State *state) {
    fr_plugin_declaration *declaration = declaration_in_progress;
    if (declaration == NULL) return 0;
    luaL_checktype(state, 1, LUA_TTABLE);

    lua_getfield(state, 1, "api");
    if (!lua_isnil(state, -1)) {
        int is_integer = 0;
        lua_Integer api = lua_tointegerx(state, -1, &is_integer);
        if (!is_integer) {
            return luaL_error(state, "plugin \"%s\": api must be a whole number",
                              declaration->label);
        }
        if (api != 1) {
            return luaL_error(state, "plugin \"%s\": needs daukle api %I, this daukle provides 1",
                              declaration->label, (LUAI_UACINT) api);
        }
    }
    lua_pop(state, 1);

    lua_getfield(state, 1, "uses");
    record_uses(state, declaration);
    lua_pop(state, 1);

    lua_pushliteral(state, FR_PLUGIN_DECLARATION_READ);
    return lua_error(state);
}

static int require_declaration_first(lua_State *state) {
    const fr_plugin_declaration *declaration = declaration_in_progress;
    return luaL_error(state, "daukle.plugin must be the first call in \"%s\"",
                      declaration != NULL ? declaration->label : "?");
}

static int protected_declaration_env(lua_State *state) {
    lua_newtable(state);
    lua_pushcfunction(state, declare_plugin);
    lua_setfield(state, -2, "plugin");

    lua_newtable(state);
    lua_pushcfunction(state, require_declaration_first);
    lua_setfield(state, -2, "__index");
    lua_setmetatable(state, -2);

    lua_setfield(state, 1, "daukle");
    return 0;
}

static int read_declaration(lua_State *state, const char *text, const char *origin,
                            fr_plugin_declaration *declaration, fr_error *err) {
    int top = lua_gettop(state);
    if (fr_lua_verbs_push_env(state, NULL, 0, err) != FR_OK) return FR_ERR;
    int env = lua_gettop(state);

    lua_pushcfunction(state, protected_declaration_env);
    lua_pushvalue(state, env);
    if (lua_pcall(state, 1, 0, 0) != LUA_OK) {
        fr_error_set(err, "%s", fr_lua_error_text(state));
        lua_settop(state, top);
        return FR_ERR;
    }

    declaration_in_progress = declaration;
    int status = fr_lua_run_in_env(state, text, origin, env, err);
    declaration_in_progress = NULL;
    lua_settop(state, top);

    if (status == FR_OK) return FR_OK;
    /* Anchored at offset 0: lua's error() prefixes the position, so a forged one is not. */
    return strncmp(err->message, FR_PLUGIN_DECLARATION_READ,
                   sizeof FR_PLUGIN_DECLARATION_READ - 1) == 0 ? FR_OK : FR_ERR;
}

static int split_repo(const char *repo, char **out_owner, char **out_name, fr_error *err) {
    const char *slash = strchr(repo, '/');
    if (slash == NULL || slash == repo || slash[1] == '\0') {
        fr_error_set(err, "repo \"%s\" must be \"owner/name\"", repo);
        return FR_ERR;
    }
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

/* Only src/plugins.c and test/support.c enumerate directories, and this one
   may not include the test header, so its own small copy stays here. */
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

static int resolve_remote_text(const fr_plugin_entry *entry, char **out_text, char **out_origin,
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

static int load_one(const fr_plugin_entry *entry, fr_error *err) {
    lua_State *state = fr_lua_runtime_state();

    char *path = NULL;
    char *text = NULL;
    if (entry->kind == FR_PLUGIN_LOCAL) {
        if (fr_lua_sandbox_resolve(state, entry->path, &path, err) != FR_OK) return FR_ERR;
        if (fr_file_read_text(path, &text, err) != FR_OK) {
            free(path);
            return FR_ERR;
        }
    } else {
        if (resolve_remote_text(entry, &text, &path, err) != FR_OK) return FR_ERR;
    }

    fr_plugin_declaration declaration = { entry->label, { NULL }, 0 };
    int status = read_declaration(state, text, path, &declaration, err);
    if (status == FR_OK) {
        status = fr_lua_plugin_load(text, path, (const char *const *) declaration.uses,
                                    declaration.uses_count, err);
    }

    free_declaration(&declaration);
    free(text);
    free(path);
    return status;
}

int fr_plugins_load(fr_registry *registry, const struct cJSON *document, const char *base_dir,
                    fr_error *err) {
    fr_plugin_entry *entries = NULL;
    size_t count = 0;
    if (fr_plugins_parse(document, &entries, &count, err) != FR_OK) return FR_ERR;
    if (count == 0) return FR_OK;

    if (fr_lua_runtime_begin(base_dir, registry, err) != FR_OK) {
        fr_plugins_free(entries, count);
        return FR_ERR;
    }

    int status = FR_OK;
    for (size_t index = 0; index < count && status == FR_OK; index++) {
        status = load_one(&entries[index], err);
    }
    fr_plugins_free(entries, count);
    return status;
}
