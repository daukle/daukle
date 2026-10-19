#include "lua/lua_verbs_internal.h"

#include "cache/cache.h"
#include "config/config_lua.h"
#include "config/jsonedit.h"
#include "config/jsonx.h"
#include "lua/lua_sandbox.h"
#include "lua/luax.h"
#include "net/http.h"
#include "plugin/registry.h"
#include "project/lang_region.h"
#include "project/region.h"
#include "util/error.h"
#include "util/tree.h"

#include "lauxlib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FR_VERB_MAX_HEADERS 16
#define FR_VERB_MAX_ARRAY 256

int fr_lua_verb_read(lua_State *state) {
    const char *relative = luaL_checkstring(state, 1);
    char *resolved = NULL;
    fr_error err;
    if (fr_lua_sandbox_resolve(state, relative, &resolved, &err) != FR_OK) {
        return luaL_error(state, "%s", err.message);
    }

    char *text = NULL;
    int status = fr_file_read_text(resolved, &text, &err);
    free(resolved);
    if (status != FR_OK) return luaL_error(state, "%s", err.message);

    lua_pushstring(state, text);
    free(text);
    return 1;
}

typedef struct {
    char **names;
    const char **kinds;
    size_t count;
    size_t capacity;
    int failed;
} fr_list_entries;

static void collect_list_entry(const char *name, int is_directory, int is_link, void *context) {
    fr_list_entries *entries = context;
    if (entries->failed) return;
    if (entries->count == entries->capacity) {
        size_t grown = entries->capacity == 0 ? 16 : entries->capacity * 2;
        char **names = realloc(entries->names, grown * sizeof *names);
        if (names != NULL) entries->names = names;
        const char **kinds = realloc((void *) entries->kinds, grown * sizeof *kinds);
        if (kinds != NULL) entries->kinds = kinds;
        if (names == NULL || kinds == NULL) {
            entries->failed = 1;
            return;
        }
        entries->capacity = grown;
    }
    size_t length = strlen(name) + 1;
    char *copy = malloc(length);
    if (copy == NULL) {
        entries->failed = 1;
        return;
    }
    memcpy(copy, name, length);
    entries->names[entries->count] = copy;
    entries->kinds[entries->count] = is_link ? "link" : (is_directory ? "directory" : "file");
    entries->count++;
}

static void free_list_entries(fr_list_entries *entries) {
    for (size_t index = 0; index < entries->count; index++) free(entries->names[index]);
    free(entries->names);
    free((void *) entries->kinds);
}

static void sort_list_entries(fr_list_entries *entries) {
    for (size_t index = 1; index < entries->count; index++) {
        char *name = entries->names[index];
        const char *kind = entries->kinds[index];
        size_t slot = index;
        while (slot > 0 && strcmp(entries->names[slot - 1], name) > 0) {
            entries->names[slot] = entries->names[slot - 1];
            entries->kinds[slot] = entries->kinds[slot - 1];
            slot--;
        }
        entries->names[slot] = name;
        entries->kinds[slot] = kind;
    }
}

static int push_list_entries(lua_State *state, const char *directory) {
    fr_list_entries entries = { 0 };
    fr_error err;
    if (fr_list_directory_checked(directory, collect_list_entry, &entries, &err) != FR_OK) {
        free_list_entries(&entries);
        lua_pushnil(state);
        lua_pushstring(state, err.message);
        return 2;
    }
    if (entries.failed) {
        free_list_entries(&entries);
        return luaL_error(state, "out of memory listing \"%s\"", directory);
    }
    sort_list_entries(&entries);
    lua_createtable(state, (int) entries.count, 0);
    for (size_t index = 0; index < entries.count; index++) {
        lua_createtable(state, 0, 2);
        lua_pushstring(state, entries.names[index]);
        lua_setfield(state, -2, "name");
        lua_pushstring(state, entries.kinds[index]);
        lua_setfield(state, -2, "kind");
        lua_rawseti(state, -2, (lua_Integer) index + 1);
    }
    free_list_entries(&entries);
    return 1;
}

/* @implNote narrower than fr_lua_sandbox_climbs_out, which refuses ".." anywhere: a walk hands
   this verb every directory name it meets, and "a..b" is an ordinary one. Containment is still
   decided on the canonical path afterwards, so only the lexical shortcut is relaxed. */
static int names_a_parent_segment(const char *relative) {
    if (relative[0] == '/' || relative[0] == '\\') return 1;
    if (relative[0] != '\0' && relative[1] == ':') return 1;
    const char *segment = relative;
    for (const char *cursor = relative;; cursor++) {
        if (*cursor != '/' && *cursor != '\\' && *cursor != '\0') continue;
        if (cursor - segment == 2 && segment[0] == '.' && segment[1] == '.') return 1;
        if (*cursor == '\0') return 0;
        segment = cursor + 1;
    }
}

static int contained_directory(const char *base, const char *directory, char **out_canonical,
                               fr_error *err) {
    if (fr_lua_sandbox_canonical_dir(directory, out_canonical, err) != FR_OK) return FR_ERR;
    if (strcmp(base, *out_canonical) == 0 || fr_lua_sandbox_is_within(base, *out_canonical)) {
        return FR_OK;
    }
    free(*out_canonical);
    *out_canonical = NULL;
    return FR_ERR;
}

static void parent_of(char *path) {
    char *slash = strrchr(path, '/');
    char *backslash = strrchr(path, '\\');
    if (backslash != NULL && (slash == NULL || backslash > slash)) slash = backslash;
    if (slash != NULL) *slash = '\0';
}

/* @implNote returns nil (and a reason when the path exists) instead of raising, because a
   plugin has no pcall to tell a missing path from an unreadable one; a path outside the base
   still raises, exactly as daukle.read does. A missing path is answered before containment is
   checked, since it cannot be canonicalised and points nowhere. */
int fr_lua_verb_list(lua_State *state) {
    const char *relative = luaL_checkstring(state, 1);
    if (names_a_parent_segment(relative)) {
        return luaL_error(state, "\"%s\" is outside the project directory", relative);
    }
    fr_error err;
    char *base = NULL;
    if (fr_lua_sandbox_base_dir(state, &base, &err) != FR_OK) return luaL_error(state, "%s", err.message);
    char lexical[1024];
    int written = snprintf(lexical, sizeof lexical, "%s/%s", base, relative);
    if (written < 0 || (size_t) written >= sizeof lexical) {
        free(base);
        return luaL_error(state, "the path is too long (%d characters)", (int) strlen(relative));
    }
    int is_directory = fr_cache_directory_exists(lexical);
    if (!is_directory && !fr_cache_file_exists(lexical)) {
        free(base);
        lua_pushnil(state);
        return 1;
    }
    if (!is_directory) parent_of(lexical);
    char *canonical = NULL;
    int status = contained_directory(base, lexical, &canonical, &err);
    free(base);
    if (status != FR_OK) return luaL_error(state, "\"%s\" is outside the project directory", relative);
    if (!is_directory) {
        free(canonical);
        lua_pushnil(state);
        lua_pushfstring(state, "%s is not a directory", relative);
        return 2;
    }
    int results = push_list_entries(state, canonical);
    free(canonical);
    return results;
}

int fr_lua_verb_fetch(lua_State *state) {
    const char *url = luaL_checkstring(state, 1);

    fr_http_header headers[FR_VERB_MAX_HEADERS];
    size_t header_count = 0;
    if (!lua_isnoneornil(state, 2)) {
        luaL_checktype(state, 2, LUA_TTABLE);
        lua_pushnil(state);
        while (lua_next(state, 2) != 0) {
            if (header_count == FR_VERB_MAX_HEADERS) {
                return luaL_error(state, "at most %d headers", FR_VERB_MAX_HEADERS);
            }
            if (lua_type(state, -2) != LUA_TSTRING || lua_type(state, -1) != LUA_TSTRING) {
                return luaL_error(state, "every header name and value must be a string");
            }
            /* headers[].name/value point into the table still on the stack
               below; the table must not be popped before fr_http_get runs. */
            headers[header_count].name = lua_tostring(state, -2);
            headers[header_count].value = lua_tostring(state, -1);
            header_count++;
            lua_pop(state, 1);
        }
    }

    char *body = NULL;
    size_t length = 0;
    fr_error err;
    if (fr_http_get(url, headers, header_count, &body, &length, &err) != FR_OK) {
        return luaL_error(state, "%s", err.message);
    }
    lua_pushlstring(state, body, length);
    free(body);
    return 1;
}

int fr_lua_verb_cache(lua_State *state) {
    const char *project = luaL_checkstring(state, 1);
    const char *version = luaL_checkstring(state, 2);
    const char *artifact = luaL_checkstring(state, 3);
    luaL_checktype(state, 4, LUA_TFUNCTION);

    char *cached = NULL;
    fr_error err;
    /* A miss is FR_OK with no text; FR_ERR is the cache refusing the path or
       failing to read it, and falling through to the producer would turn that
       into a silent refetch on every run. */
    if (fr_cache_read(project, version, artifact, &cached, &err) != FR_OK) {
        return luaL_error(state, "%s", err.message);
    }
    if (cached != NULL) {
        lua_pushstring(state, cached);
        free(cached);
        return 1;
    }

    lua_pushvalue(state, 4);
    /* Runs inside the plugin's protected frame already, so a producer error
       must propagate with its original message and traceback intact. */
    lua_call(state, 0, 1);
    size_t length = 0;
    const char *produced = lua_tolstring(state, -1, &length);
    if (produced == NULL) return luaL_error(state, "the producer returned no text");

    fr_cache_write(project, version, artifact, produced, length);
    return 1;
}

static const char *pending_region_body;

static int render_pending_body(const fr_consumer *consumer, const fr_resolved *resolved,
                               size_t count, char **out_text, fr_error *err) {
    (void) consumer; (void) resolved; (void) count;
    *out_text = malloc(strlen(pending_region_body) + 1);
    if (*out_text == NULL) {
        fr_error_set(err, "out of memory rendering a region");
        return FR_ERR;
    }
    strcpy(*out_text, pending_region_body);
    return FR_OK;
}

int fr_lua_verb_region(lua_State *state) {
    const char *text = luaL_checkstring(state, 1);
    const char *begin = luaL_checkstring(state, 2);
    const char *end = luaL_checkstring(state, 3);
    pending_region_body = luaL_checkstring(state, 4);

    char *out = NULL;
    fr_error err;
    int status = fr_lang_region_apply(begin, end, render_pending_body, NULL, NULL, 0,
                                      text, &out, &err);
    pending_region_body = NULL;
    if (status != FR_OK) return luaL_error(state, "%s", err.message);

    lua_pushstring(state, out);
    free(out);
    return 1;
}

static int json_set_array(lua_State *state, const char *text, const char *path, const char *key) {
    lua_Integer count = luaL_len(state, 4);
    if (count < 0 || count > FR_VERB_MAX_ARRAY) {
        return luaL_error(state, "at most %d array values", FR_VERB_MAX_ARRAY);
    }
    if (!lua_checkstack(state, (int) count + 4)) {
        return luaL_error(state, "cannot grow the lua stack for %d array values", (int) count);
    }

    const char *values[FR_VERB_MAX_ARRAY];
    for (lua_Integer index = 1; index <= count; index++) {
        lua_geti(state, 4, index);
        if (lua_type(state, -1) != LUA_TSTRING) {
            return luaL_error(state, "array value %d is not a string", (int) index);
        }
        /* The value stays on the stack: values[] points into it, and popping
           before fr_json_edit_set_string_array runs would free it. */
        values[index - 1] = lua_tostring(state, -1);
    }

    char *out = NULL;
    fr_error err;
    if (fr_json_edit_set_string_array(text, path, key, values, (size_t) count, &out, &err) != FR_OK) {
        return luaL_error(state, "%s", err.message);
    }
    lua_pushstring(state, out);
    free(out);
    return 1;
}

int fr_lua_verb_json_set(lua_State *state) {
    const char *text = luaL_checkstring(state, 1);
    const char *path = luaL_checkstring(state, 2);
    const char *key = luaL_checkstring(state, 3);

    char *out = NULL;
    fr_error err;
    int status;
    if (lua_isnoneornil(state, 4)) {
        status = fr_json_edit_remove(text, path, key, &out, &err);
    } else if (lua_type(state, 4) == LUA_TTABLE) {
        return json_set_array(state, text, path, key);
    } else {
        status = fr_json_edit_set_string(text, path, key, luaL_checkstring(state, 4), &out, &err);
    }
    if (status != FR_OK) return luaL_error(state, "%s", err.message);

    lua_pushstring(state, out);
    free(out);
    return 1;
}

int fr_lua_verb_json_parse(lua_State *state) {
    const char *text = luaL_checkstring(state, 1);

    cJSON *document = NULL;
    fr_error err;
    if (fr_json_parse(text, &document, &err) != FR_OK) {
        return luaL_error(state, "daukle.json_parse: %s", err.message);
    }

    int pushed = fr_lua_push_json(state, document, &err);
    cJSON_Delete(document);
    if (pushed != FR_OK) return luaL_error(state, "daukle.json_parse: %s", err.message);
    return 1;
}

int fr_lua_verb_parse(lua_State *state) {
    const char *text = luaL_checkstring(state, 1);
    const char *file_name = luaL_checkstring(state, 2);

    fr_registry *registry = fr_lua_registering_registry();
    if (registry == NULL) return luaL_error(state, "no registry is loading");

    char capability[128];
    const char *extension = strrchr(file_name, '.');
    if (extension == NULL) return luaL_error(state, "\"%s\" has no extension", file_name);
    snprintf(capability, sizeof capability, "daukle.config/%s", extension + 1);

    const fr_config_plugin *plugin = fr_registry_config(registry, capability);
    if (plugin == NULL) return luaL_error(state, "no plugin reads \"%s\"", extension + 1);
    if (plugin->overlay) {
        return luaL_error(state, "daukle.parse will not run \"%s\", which is an executable format",
                          extension + 1);
    }

    cJSON *document = NULL;
    fr_error err;
    if (plugin->load(plugin->state, text, file_name, ".", registry, NULL, &document, &err) != FR_OK) {
        return luaL_error(state, "%s", err.message);
    }

    int pushed = fr_lua_push_json(state, document, &err);
    cJSON_Delete(document);
    if (pushed != FR_OK) return luaL_error(state, "%s", err.message);
    return 1;
}
