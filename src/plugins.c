#include "plugins.h"

#include "cJSON.h"
#include "config_lua.h"
#include "error.h"
#include "jsonx.h"
#include "lua_sandbox.h"
#include "lua_verbs.h"
#include "luax.h"
#include "region.h"

#include "lauxlib.h"

#include <stdlib.h>
#include <string.h>

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
   is what tells a declaration that was read from a plugin that really failed. */
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

static int ignore_call(lua_State *state) {
    (void) state;
    return 0;
}

static int ignore_field(lua_State *state) {
    lua_pushcfunction(state, ignore_call);
    return 1;
}

static int protected_declaration_env(lua_State *state) {
    lua_newtable(state);
    lua_pushcfunction(state, declare_plugin);
    lua_setfield(state, -2, "plugin");

    lua_newtable(state);
    lua_pushcfunction(state, ignore_field);
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
    return strstr(err->message, FR_PLUGIN_DECLARATION_READ) != NULL ? FR_OK : FR_ERR;
}

static int load_one(const fr_plugin_entry *entry, fr_error *err) {
    if (entry->kind != FR_PLUGIN_LOCAL) {
        fr_error_set(err, "plugin \"%s\": remote plugins are not supported yet", entry->label);
        return FR_ERR;
    }

    lua_State *state = fr_lua_runtime_state();
    char *path = NULL;
    if (fr_lua_sandbox_resolve(state, entry->path, &path, err) != FR_OK) return FR_ERR;

    char *text = NULL;
    if (fr_file_read_text(path, &text, err) != FR_OK) {
        free(path);
        return FR_ERR;
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
