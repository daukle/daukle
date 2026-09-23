#include "plugins.h"

#include "cJSON.h"
#include "cache.h"
#include "config_lua.h"
#include "error.h"
#include "jsonx.h"
#include "lua_sandbox.h"
#include "lua_verbs.h"
#include "luax.h"
#include "plugin_fetch.h"
#include "region.h"
#include "resolvers.h"
#include "sha256.h"

#include "lauxlib.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *fr_dup_string(const char *text) {
    if (text == NULL) return NULL;
    size_t length = strlen(text) + 1;
    char *copy = malloc(length);
    if (copy != NULL) memcpy(copy, text, length);
    return copy;
}

char *fr_dup_prefix(const char *text, size_t length) {
    char *copy = malloc(length + 1);
    if (copy == NULL) return NULL;
    memcpy(copy, text, length);
    copy[length] = '\0';
    return copy;
}

static int out_of_memory(const char *label, fr_error *err) {
    fr_error_set(err, "out of memory reading plugin \"%s\"", label);
    return FR_ERR;
}

/* Keeps only whole labels and marks the cut, rather than ending the one
   diagnostic a reader gets when a coordinate does not resolve mid-label. */
static void describe_declared_resolvers(char *out, size_t out_size,
                                        const fr_resolver_entry *resolvers, size_t count) {
    if (count == 0) {
        snprintf(out, out_size, "none");
        return;
    }

    static const char ELLIPSIS[] = ", ...";
    size_t used = 0;
    out[0] = '\0';
    for (size_t index = 0; index < count; index++) {
        const char *separator = index == 0 ? "" : ", ";
        size_t needed = strlen(separator) + strlen(resolvers[index].label);
        if (used + needed + sizeof ELLIPSIS > out_size) {
            snprintf(out + used, out_size - used, "%s", used == 0 ? "..." : ELLIPSIS);
            return;
        }
        used += (size_t) snprintf(out + used, out_size - used, "%s%s", separator,
                                  resolvers[index].label);
    }
}

static int no_resolver_named(const char *label, const char *value,
                             const fr_resolver_entry *resolvers, size_t resolver_count,
                             fr_error *err) {
    char declared[160];
    describe_declared_resolvers(declared, sizeof declared, resolvers, resolver_count);
    fr_error_set(err, "plugin \"%s\": \"%s\" names no resolver: write a file path, a url, or"
                      " \"<resolver>:<coordinate>\". Declared resolvers: %s",
                 label, value, declared);
    return FR_ERR;
}

/* "C:/plugins/mine.lua" read as the resolver "C" is the diagnostic this
   prevents, which is why the check comes before the colon split below. */
static int is_drive_path(const char *value) {
    return ((value[0] >= 'A' && value[0] <= 'Z') || (value[0] >= 'a' && value[0] <= 'z'))
        && value[1] == ':' && (value[2] == '/' || value[2] == '\\');
}

static int parse_string_form(const char *label, const char *value,
                             const fr_resolver_entry *resolvers, size_t resolver_count,
                             fr_plugin_entry *out, fr_error *err) {
    if (value[0] == '.' || value[0] == '/' || is_drive_path(value)) {
        out->kind = FR_PLUGIN_PATH;
        out->path = fr_dup_string(value);
        return out->path != NULL ? FR_OK : out_of_memory(label, err);
    }

    if (strncmp(value, "http://", 7) == 0 || strncmp(value, "https://", 8) == 0) {
        out->kind = FR_PLUGIN_URL;
        out->url = fr_dup_string(value);
        return out->url != NULL ? FR_OK : out_of_memory(label, err);
    }

    const char *colon = strchr(value, ':');
    if (colon == NULL || colon == value || colon[1] == '\0') {
        return no_resolver_named(label, value, resolvers, resolver_count, err);
    }

    out->kind = FR_PLUGIN_RESOLVED;
    out->resolver = fr_dup_prefix(value, (size_t) (colon - value));
    out->coordinate = fr_dup_string(colon + 1);
    return (out->resolver != NULL && out->coordinate != NULL) ? FR_OK : out_of_memory(label, err);
}

/* A table entry names exactly one of "path", "url", or "resolver" with
   "coordinate", the three forms the string form discriminates between, and any
   of them may also carry a "sha256" pin. */
static int parse_table_form(const char *label, const cJSON *member, fr_plugin_entry *out,
                            fr_error *err) {
    if (cJSON_GetObjectItemCaseSensitive(member, "sha256") != NULL) {
        const char *sha256 = NULL;
        if (fr_json_string(member, "sha256", label, &sha256, err) != FR_OK) return FR_ERR;
        out->sha256 = fr_dup_string(sha256);
        if (out->sha256 == NULL) return out_of_memory(label, err);
    }

    int has_path = cJSON_GetObjectItemCaseSensitive(member, "path") != NULL;
    int has_url = cJSON_GetObjectItemCaseSensitive(member, "url") != NULL;
    int has_resolver = cJSON_GetObjectItemCaseSensitive(member, "resolver") != NULL;
    int has_coordinate = cJSON_GetObjectItemCaseSensitive(member, "coordinate") != NULL;

    int named_groups = has_path + has_url + (has_resolver || has_coordinate);
    if (named_groups > 1) {
        fr_error_set(err, "plugin \"%s\": a table entry names one of path, url, or resolver with"
                          " coordinate, not several", label);
        return FR_ERR;
    }
    if (named_groups == 0) {
        fr_error_set(err, "plugin \"%s\": a table entry needs one of path, url, or resolver with"
                          " coordinate", label);
        return FR_ERR;
    }

    if (has_path) {
        const char *path = NULL;
        if (fr_json_string(member, "path", label, &path, err) != FR_OK) return FR_ERR;

        out->kind = FR_PLUGIN_PATH;
        out->path = fr_dup_string(path);
        return out->path != NULL ? FR_OK : out_of_memory(label, err);
    }

    if (has_url) {
        const char *url = NULL;
        if (fr_json_string(member, "url", label, &url, err) != FR_OK) return FR_ERR;

        out->kind = FR_PLUGIN_URL;
        out->url = fr_dup_string(url);
        return out->url != NULL ? FR_OK : out_of_memory(label, err);
    }

    if (!has_resolver || !has_coordinate) {
        fr_error_set(err, "plugin \"%s\": a resolver entry names both resolver and coordinate;"
                          " \"%s\" is missing", label, has_resolver ? "coordinate" : "resolver");
        return FR_ERR;
    }

    const char *resolver = NULL;
    const char *coordinate = NULL;
    if (fr_json_string(member, "resolver", label, &resolver, err) != FR_OK) return FR_ERR;
    if (fr_json_string(member, "coordinate", label, &coordinate, err) != FR_OK) return FR_ERR;

    out->kind = FR_PLUGIN_RESOLVED;
    out->resolver = fr_dup_string(resolver);
    out->coordinate = fr_dup_string(coordinate);
    return (out->resolver != NULL && out->coordinate != NULL) ? FR_OK : out_of_memory(label, err);
}

int fr_plugins_parse(const struct cJSON *document, const fr_resolver_entry *resolvers,
                     size_t resolver_count, fr_plugin_entry **out, size_t *out_count,
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
        slot->label = fr_dup_string(label);
        if (slot->label == NULL) {
            out_of_memory(label, err);
            fr_plugins_free(entries, count);
            return FR_ERR;
        }

        int status;
        if (cJSON_IsString(member) && member->valuestring != NULL) {
            status = parse_string_form(label, member->valuestring, resolvers, resolver_count,
                                       slot, err);
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

int fr_plugins_reject_in_fetched(const struct cJSON *document, const char *project, fr_error *err) {
    if (cJSON_GetObjectItemCaseSensitive(document, "plugins") == NULL) return FR_OK;
    fr_error_set(err, "project \"%s\" declares plugins, which only the root manifest may do",
                 project);
    return FR_ERR;
}

void fr_plugins_free(fr_plugin_entry *entries, size_t count) {
    if (entries == NULL) return;
    for (size_t index = 0; index < count; index++) {
        free(entries[index].label);
        free(entries[index].path);
        free(entries[index].url);
        free(entries[index].resolver);
        free(entries[index].coordinate);
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
        char *copy = fr_dup_string(name);
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

/* Shared with resolvers.c: a resolver's chunk starts with the same
   daukle.plugin{ uses = {...} } call a plugin's does, and acquiring it goes
   through this same read-then-load split so that declaration is honoured
   there too, rather than every verb being either always on or always off for
   a resolver. */
int fr_plugins_read_uses(const char *text, const char *origin, const char *label,
                         char ***out_uses, size_t *out_uses_count, fr_error *err) {
    *out_uses = NULL;
    *out_uses_count = 0;

    lua_State *state = fr_lua_runtime_state();
    fr_plugin_declaration declaration = { label, { NULL }, 0 };
    if (read_declaration(state, text, origin, &declaration, err) != FR_OK) {
        free_declaration(&declaration);
        return FR_ERR;
    }
    if (declaration.uses_count == 0) return FR_OK;

    char **uses = malloc(declaration.uses_count * sizeof *uses);
    if (uses == NULL) {
        free_declaration(&declaration);
        return out_of_memory(label, err);
    }
    memcpy(uses, declaration.uses, declaration.uses_count * sizeof *uses);
    *out_uses = uses;
    *out_uses_count = declaration.uses_count;
    return FR_OK;
}

void fr_plugins_free_uses(char **uses, size_t count) {
    if (uses == NULL) return;
    for (size_t index = 0; index < count; index++) free(uses[index]);
    free(uses);
}

/* stricmp/strcasecmp are not portable C11; a sha256 hex digest is a bounded
   64 characters, so comparing lowercased copies in fixed buffers is safe. */
static int digest_matches(const char *actual, const char *pinned) {
    size_t length = strlen(actual);
    if (length != strlen(pinned) || length >= 65) return 0;
    for (size_t index = 0; index < length; index++) {
        if (tolower((unsigned char) actual[index]) != tolower((unsigned char) pinned[index])) {
            return 0;
        }
    }
    return 1;
}

static fr_plugin_report_entry *report_entries;
static size_t report_count;

static void free_report_entry(fr_plugin_report_entry *entry) {
    free(entry->label);
    free(entry->resolved);
    for (size_t index = 0; index < entry->uses_count; index++) free(entry->uses[index]);
    free(entry->uses);
}

void fr_plugins_report_clear(void) {
    for (size_t index = 0; index < report_count; index++) free_report_entry(&report_entries[index]);
    free(report_entries);
    report_entries = NULL;
    report_count = 0;
}

const fr_plugin_report *fr_plugins_report(void) {
    static fr_plugin_report view;
    view.entries = report_entries;
    view.count = report_count;
    return &view;
}

/* Appends one entry, owning copies of everything it stores: entry and declaration are both
   about to be freed by their callers (fr_plugins_free and free_declaration), so nothing here
   may keep a pointer into either. */
static int append_report_entry(const fr_plugin_entry *entry, const char *resolved,
                               const char *digest, const fr_plugin_declaration *declaration,
                               fr_error *err) {
    fr_plugin_report_entry *grown = realloc(report_entries, (report_count + 1) * sizeof *grown);
    if (grown == NULL) {
        fr_error_set(err, "out of memory recording plugin \"%s\" in the report", entry->label);
        return FR_ERR;
    }
    report_entries = grown;

    fr_plugin_report_entry *slot = &report_entries[report_count];
    memset(slot, 0, sizeof *slot);
    slot->kind = entry->kind;
    memcpy(slot->sha256, digest, sizeof slot->sha256);

    slot->label = fr_dup_string(entry->label);
    slot->resolved = fr_dup_string(resolved);
    slot->uses = declaration->uses_count > 0
                     ? malloc(declaration->uses_count * sizeof *slot->uses)
                     : NULL;
    if (slot->label == NULL || slot->resolved == NULL
        || (declaration->uses_count > 0 && slot->uses == NULL)) {
        free_report_entry(slot);
        fr_error_set(err, "out of memory recording plugin \"%s\" in the report", entry->label);
        return FR_ERR;
    }

    for (size_t index = 0; index < declaration->uses_count; index++) {
        slot->uses[index] = fr_dup_string(declaration->uses[index]);
        if (slot->uses[index] == NULL) {
            slot->uses_count = index;
            free_report_entry(slot);
            fr_error_set(err, "out of memory recording plugin \"%s\" in the report", entry->label);
            return FR_ERR;
        }
    }
    slot->uses_count = declaration->uses_count;

    report_count++;
    return FR_OK;
}

static int unknown_resolver(const fr_plugin_entry *entry, const fr_resolver_entry *resolvers,
                            size_t resolver_count, fr_error *err) {
    char declared[160];
    describe_declared_resolvers(declared, sizeof declared, resolvers, resolver_count);
    fr_error_set(err, "plugin \"%s\": no resolver \"%s\" is declared. Declared resolvers: %s",
                 entry->label, entry->resolver, declared);
    return FR_ERR;
}

/* Acquires entry's chunk and reports where it came from: origin is what the
   digest pin is checked against and what a failed pin discards, resolved is
   the resolver's own answer and stays NULL for the two kinds core names
   itself. */
static int acquire(const fr_plugin_entry *entry, const fr_resolver_entry *resolvers,
                   size_t resolver_count, char **out_text, char **out_origin, char **out_resolved,
                   fr_error *err) {
    *out_text = NULL;
    *out_origin = NULL;
    *out_resolved = NULL;

    if (entry->kind == FR_PLUGIN_PATH) {
        lua_State *state = fr_lua_runtime_state();
        if (fr_lua_sandbox_resolve(state, entry->path, out_origin, err) != FR_OK) return FR_ERR;
        if (fr_file_read_text(*out_origin, out_text, err) != FR_OK) {
            free(*out_origin);
            *out_origin = NULL;
            return FR_ERR;
        }
        return FR_OK;
    }

    if (entry->kind == FR_PLUGIN_URL) {
        *out_origin = fr_dup_string(entry->url);
        if (*out_origin == NULL) return out_of_memory(entry->label, err);
    } else {
        const fr_resolver_entry *resolver =
            fr_resolvers_find(resolvers, resolver_count, entry->resolver);
        if (resolver == NULL) return unknown_resolver(entry, resolvers, resolver_count, err);
        if (fr_resolvers_use(resolver, entry->coordinate, out_origin, out_resolved, err) != FR_OK) {
            return FR_ERR;
        }
    }

    if (fr_plugin_fetch(*out_origin, out_text, err) != FR_OK) {
        free(*out_origin);
        free(*out_resolved);
        *out_origin = NULL;
        *out_resolved = NULL;
        return FR_ERR;
    }
    return FR_OK;
}

static int load_one(const fr_plugin_entry *entry, const fr_resolver_entry *resolvers,
                    size_t resolver_count, fr_error *err) {
    char *origin = NULL;
    char *text = NULL;
    char *resolved = NULL;
    if (acquire(entry, resolvers, resolver_count, &text, &origin, &resolved, err) != FR_OK) {
        return FR_ERR;
    }

    /* Computed for every plugin, pinned or not: an unpinned entry's digest is what
       "daukle config print" shows, so adopting a pin is a copy and a paste. Checked
       before read_declaration, which already runs the chunk: verifying after would
       mean the mismatched code had already executed. */
    char digest[65];
    fr_sha256_hex(text, strlen(text), digest);
    if (entry->sha256 != NULL && !digest_matches(digest, entry->sha256)) {
        fr_error_set(err, "plugin \"%s\": expected sha256 %s but the file is %s",
                    entry->label, entry->sha256, digest);
        if (entry->kind != FR_PLUGIN_PATH) fr_plugin_fetch_discard(origin);
        free(text);
        free(origin);
        free(resolved);
        return FR_ERR;
    }

    lua_State *state = fr_lua_runtime_state();
    fr_plugin_declaration declaration = { entry->label, { NULL }, 0 };
    int status = read_declaration(state, text, origin, &declaration, err);
    if (status == FR_OK) {
        status = fr_lua_plugin_load(text, origin, (const char *const *) declaration.uses,
                                    declaration.uses_count, err);
    }
    if (status == FR_OK) {
        status = append_report_entry(entry, resolved != NULL ? resolved : origin, digest,
                                     &declaration, err);
    }

    free_declaration(&declaration);
    free(text);
    free(origin);
    free(resolved);
    return status;
}

int fr_plugins_load(fr_registry *registry, const struct cJSON *document, const char *base_dir,
                    fr_error *err) {
    fr_plugins_report_clear();
    fr_resolvers_clear();

    fr_resolver_entry *resolvers = NULL;
    size_t resolver_count = 0;
    if (fr_resolvers_parse(document, &resolvers, &resolver_count, err) != FR_OK) return FR_ERR;

    fr_plugin_entry *entries = NULL;
    size_t count = 0;
    int status = fr_plugins_parse(document, resolvers, resolver_count, &entries, &count, err);
    if (status == FR_OK && count > 0) {
        status = fr_lua_runtime_begin(base_dir, registry, err);
        for (size_t index = 0; index < count && status == FR_OK; index++) {
            status = load_one(&entries[index], resolvers, resolver_count, err);
        }
    }

    fr_plugins_free(entries, count);
    fr_resolvers_free(resolvers, resolver_count);
    return status;
}

int fr_plugins_update_cache(const fr_plugin_entry *entries, size_t count,
                            const fr_resolver_entry *resolvers, size_t resolver_count,
                            const char *label, size_t *out_removed_count, fr_error *err) {
    *out_removed_count = 0;

    int cache_was_enabled = fr_cache_enabled();
    fr_cache_set_enabled(0);

    int status = FR_OK;
    int matched = label == NULL;

    for (size_t index = 0; index < count && status == FR_OK; index++) {
        const fr_plugin_entry *entry = &entries[index];
        if (label != NULL && strcmp(entry->label, label) != 0) continue;
        matched = 1;

        if (entry->kind == FR_PLUGIN_URL) {
            fr_plugin_fetch_discard(entry->url);
            (*out_removed_count)++;
        } else if (entry->kind == FR_PLUGIN_RESOLVED) {
            const fr_resolver_entry *resolver =
                fr_resolvers_find(resolvers, resolver_count, entry->resolver);
            if (resolver == NULL) {
                status = unknown_resolver(entry, resolvers, resolver_count, err);
            } else {
                char *url = NULL;
                char *resolved = NULL;
                status = fr_resolvers_use(resolver, entry->coordinate, &url, &resolved, err);
                if (status == FR_OK) {
                    fr_plugin_fetch_discard(url);
                    (*out_removed_count)++;
                }
                free(url);
                free(resolved);
            }
        }

        if (label != NULL) break;
    }

    fr_cache_set_enabled(cache_was_enabled);

    if (status != FR_OK) return FR_ERR;
    if (!matched) {
        fr_error_set(err, "no plugin named \"%s\"", label);
        return FR_ERR;
    }
    return FR_OK;
}
