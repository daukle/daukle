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
#include "plugin_modules.h"
#include "region.h"
#include "resolvers.h"
#include "sha256.h"

#include "lauxlib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/stat.h>

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
    /* Not validated here: plugin_deps.c is the sole reader, and validating an
       absent table would tax the common case (no requires at all) for
       nothing. */
    out->overrides = cJSON_GetObjectItemCaseSensitive(member, "requires");

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

int fr_plugins_parse_entry(const char *label, const cJSON *member,
                           const fr_resolver_entry *resolvers, size_t resolver_count,
                           fr_plugin_entry *out, fr_error *err) {
    if (cJSON_IsString(member) && member->valuestring != NULL) {
        return parse_string_form(label, member->valuestring, resolvers, resolver_count, out, err);
    }
    if (cJSON_IsObject(member)) return parse_table_form(label, member, out, err);

    fr_error_set(err, "plugin \"%s\" must be a string or a table", label);
    return FR_ERR;
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

        if (fr_plugins_parse_entry(label, member, resolvers, resolver_count, slot, err) != FR_OK) {
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

void fr_plugins_free_entry(fr_plugin_entry *entry) {
    free(entry->label);
    free(entry->path);
    free(entry->url);
    free(entry->resolver);
    free(entry->coordinate);
    free(entry->sha256);
    memset(entry, 0, sizeof *entry);
}

void fr_plugins_free(fr_plugin_entry *entries, size_t count) {
    if (entries == NULL) return;
    for (size_t index = 0; index < count; index++) fr_plugins_free_entry(&entries[index]);
    free(entries);
}

/* Raised to stop the chunk the moment daukle.plugin has been read, so catching it
   distinguishes a declaration that was read from a plugin that really failed. */
#define FR_PLUGIN_DECLARATION_READ "daukle: plugin declaration read"

static fr_plugin_declaration *declaration_in_progress;

void fr_plugins_free_declaration(fr_plugin_declaration *declaration) {
    for (size_t index = 0; index < declaration->uses_count; index++) {
        free(declaration->uses[index]);
    }
    declaration->uses_count = 0;
    for (size_t index = 0; index < declaration->exports_count; index++) {
        free(declaration->exports[index]);
    }
    declaration->exports_count = 0;
    for (size_t index = 0; index < declaration->requires_count; index++) {
        free(declaration->requires[index].alias);
        free(declaration->requires[index].url);
        free(declaration->requires[index].sha256);
    }
    declaration->requires_count = 0;
}

static int record_uses(lua_State *state, fr_plugin_declaration *declaration) {
    if (lua_isnil(state, -1)) return 0;
    if (!lua_istable(state, -1)) {
        return luaL_error(state, "%s \"%s\": uses must be a list of verb names",
                          declaration->kind, declaration->label);
    }

    lua_Integer length = (lua_Integer) lua_rawlen(state, -1);
    for (lua_Integer index = 1; index <= length; index++) {
        lua_rawgeti(state, -1, index);
        if (lua_type(state, -1) != LUA_TSTRING) {
            return luaL_error(state, "%s \"%s\": every name in uses must be a string",
                              declaration->kind, declaration->label);
        }
        const char *name = lua_tostring(state, -1);
        if (!fr_lua_verbs_is_known(name)) {
            return luaL_error(state, "%s \"%s\": \"%s\" is not a daukle verb",
                              declaration->kind, declaration->label, name);
        }
        if (declaration->uses_count == FR_PLUGIN_MAX_USES) {
            return luaL_error(state, "%s \"%s\": uses names more than %d verbs",
                              declaration->kind, declaration->label, FR_PLUGIN_MAX_USES);
        }
        char *copy = fr_dup_string(name);
        if (copy == NULL) {
            return luaL_error(state, "%s \"%s\": out of memory reading uses",
                              declaration->kind, declaration->label);
        }
        declaration->uses[declaration->uses_count] = copy;
        declaration->uses_count++;
        lua_pop(state, 1);
    }
    return 0;
}

static int record_exports(lua_State *state, fr_plugin_declaration *declaration) {
    if (lua_isnil(state, -1)) return 0;
    if (strcmp(declaration->kind, "resolver") == 0) {
        return luaL_error(state, "resolver \"%s\": a resolver may not export a module, because a"
                                 " resolver has no dependents",
                          declaration->label);
    }
    if (!lua_istable(state, -1)) {
        return luaL_error(state, "%s \"%s\": exports must be a list of module names",
                          declaration->kind, declaration->label);
    }

    lua_Integer length = (lua_Integer) lua_rawlen(state, -1);
    for (lua_Integer index = 1; index <= length; index++) {
        lua_rawgeti(state, -1, index);
        if (lua_type(state, -1) != LUA_TSTRING) {
            return luaL_error(state, "%s \"%s\": every name in exports must be a string",
                              declaration->kind, declaration->label);
        }
        const char *name = lua_tostring(state, -1);
        fr_error name_err;
        if (fr_plugin_module_name_check(name, &name_err) != FR_OK) {
            return luaL_error(state, "%s \"%s\": %s", declaration->kind, declaration->label,
                              name_err.message);
        }
        if (declaration->exports_count == FR_PLUGIN_MAX_EXPORTS) {
            return luaL_error(state, "%s \"%s\": exports names more than %d modules",
                              declaration->kind, declaration->label, FR_PLUGIN_MAX_EXPORTS);
        }
        char *copy = fr_dup_string(name);
        if (copy == NULL) {
            return luaL_error(state, "%s \"%s\": out of memory reading exports",
                              declaration->kind, declaration->label);
        }
        declaration->exports[declaration->exports_count] = copy;
        declaration->exports_count++;
        lua_pop(state, 1);
    }
    return 0;
}

static int alias_is_well_formed(const char *alias) {
    for (const char *scan = alias; *scan != '\0'; scan++) {
        int ok = (*scan >= 'a' && *scan <= 'z') || (*scan >= 'A' && *scan <= 'Z')
              || (*scan >= '0' && *scan <= '9') || *scan == '-' || *scan == '_';
        if (!ok) return 0;
    }
    return alias[0] != '\0';
}

/* Reads table[key] raw, the way config_lua.c's raw_getfield does: index may be
   relative (record_requires calls these with -1), so it is converted to
   absolute before the key is pushed, or the push would shift what the
   caller's index means. */
static int raw_has_field(lua_State *state, int index, const char *key) {
    int absolute = lua_absindex(state, index);
    lua_pushstring(state, key);
    lua_rawget(state, absolute);
    int present = !lua_isnil(state, -1);
    lua_pop(state, 1);
    return present;
}

static const char *raw_string_field(lua_State *state, int index, const char *key) {
    int absolute = lua_absindex(state, index);
    lua_pushstring(state, key);
    lua_rawget(state, absolute);
    const char *value = lua_type(state, -1) == LUA_TSTRING ? lua_tostring(state, -1) : NULL;
    lua_pop(state, 1);
    return value;
}

static int record_requires(lua_State *state, fr_plugin_declaration *declaration) {
    if (lua_isnil(state, -1)) return 0;
    if (strcmp(declaration->kind, "resolver") == 0) {
        return luaL_error(state, "resolver \"%s\": a resolver may not require a plugin, because a"
                                 " resolver is acquired by the floor only",
                          declaration->label);
    }
    if (!lua_istable(state, -1)) {
        return luaL_error(state, "plugin \"%s\": requires must be a table of alias to artifact",
                          declaration->label);
    }

    lua_pushnil(state);
    while (lua_next(state, -2) != 0) {
        if (lua_type(state, -2) != LUA_TSTRING) {
            return luaL_error(state, "plugin \"%s\": every key in requires must be an alias",
                              declaration->label);
        }
        const char *alias = lua_tostring(state, -2);
        if (!alias_is_well_formed(alias)) {
            return luaL_error(state, "plugin \"%s\": the alias \"%s\" may hold only letters, digits,"
                                     " \"-\" and \"_\"",
                              declaration->label, alias);
        }
        if (strlen(alias) > FR_PLUGIN_MAX_ALIAS) {
            return luaL_error(state, "plugin \"%s\": the alias \"%s\" is longer than %d bytes",
                              declaration->label, alias, FR_PLUGIN_MAX_ALIAS);
        }
        if (declaration->requires_count == FR_PLUGIN_MAX_REQUIRES) {
            return luaL_error(state, "plugin \"%s\": requires names more than %d plugins",
                              declaration->label, FR_PLUGIN_MAX_REQUIRES);
        }
        if (!lua_istable(state, -1)) {
            return luaL_error(state, "plugin \"%s\": requires[\"%s\"] must be a table holding url"
                                     " and sha256",
                              declaration->label, alias);
        }
        if (raw_has_field(state, -1, "path")) {
            return luaL_error(state, "plugin \"%s\": requires[\"%s\"] names a path, and a dependency"
                                     " is acquired by url. A local one is written in the manifest as"
                                     " [plugins.%s].requires",
                              declaration->label, alias, declaration->label);
        }
        const char *url = raw_string_field(state, -1, "url");
        const char *sha256 = raw_string_field(state, -1, "sha256");
        if (url == NULL) {
            return luaL_error(state, "plugin \"%s\": requires[\"%s\"] names no url",
                              declaration->label, alias);
        }
        if (sha256 == NULL) {
            return luaL_error(state, "plugin \"%s\": requires[\"%s\"] has no sha256, and a"
                                     " dependency is always pinned: a pin that is not transitive"
                                     " pins nothing",
                              declaration->label, alias);
        }
        char *alias_copy = fr_dup_string(alias);
        char *url_copy = fr_dup_string(url);
        char *sha256_copy = fr_dup_string(sha256);
        if (alias_copy == NULL || url_copy == NULL || sha256_copy == NULL) {
            free(alias_copy);
            free(url_copy);
            free(sha256_copy);
            return luaL_error(state, "plugin \"%s\": out of memory reading requires",
                              declaration->label);
        }
        fr_plugin_requirement *slot = &declaration->requires[declaration->requires_count];
        slot->alias = alias_copy;
        slot->url = url_copy;
        slot->sha256 = sha256_copy;
        declaration->requires_count++;
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
            return luaL_error(state, "%s \"%s\": api must be a whole number",
                              declaration->kind, declaration->label);
        }
        if (api != 1) {
            return luaL_error(state, "%s \"%s\": needs daukle api %I, this daukle provides 1",
                              declaration->kind, declaration->label, (LUAI_UACINT) api);
        }
    }
    lua_pop(state, 1);

    lua_getfield(state, 1, "uses");
    record_uses(state, declaration);
    lua_pop(state, 1);

    lua_getfield(state, 1, "requires");
    record_requires(state, declaration);
    lua_pop(state, 1);

    lua_getfield(state, 1, "exports");
    record_exports(state, declaration);
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

static int read_declaration(lua_State *state, const char *text, size_t length, const char *origin,
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
    int status = fr_lua_run_in_env_bytes(state, text, length, origin, env, err);
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
   a resolver. kind names the caller ("plugin" or "resolver") in every message
   below, so a resolver's malformed uses is reported as a resolver, not a
   plugin. A NULL runtime state is a real error here, not a crash:
   read_declaration's first line touches it. */
int fr_plugins_read_declaration(const char *text, size_t length, const char *origin,
                                const char *kind, const char *label,
                                fr_plugin_declaration *out, fr_error *err) {
    memset(out, 0, sizeof *out);
    out->kind = kind;
    out->label = label;

    lua_State *state = fr_lua_runtime_state();
    if (state == NULL) {
        fr_error_set(err, "no lua runtime is open for \"%s\"", origin);
        return FR_ERR;
    }

    if (read_declaration(state, text, length, origin, out, err) != FR_OK) {
        fr_plugins_free_declaration(out);
        return FR_ERR;
    }
    return FR_OK;
}

static fr_plugin_report_entry *report_entries;
static size_t report_count;

static void free_report_entry(fr_plugin_report_entry *entry) {
    free(entry->label);
    free(entry->resolver);
    free(entry->url);
    free(entry->resolved);
    for (size_t index = 0; index < entry->uses_count; index++) free(entry->uses[index]);
    free(entry->uses);
}

static char **report_unused_resolvers;
static size_t report_unused_resolver_count;

void fr_plugins_report_clear(void) {
    for (size_t index = 0; index < report_count; index++) free_report_entry(&report_entries[index]);
    free(report_entries);
    report_entries = NULL;
    report_count = 0;

    for (size_t index = 0; index < report_unused_resolver_count; index++) {
        free(report_unused_resolvers[index]);
    }
    free(report_unused_resolvers);
    report_unused_resolvers = NULL;
    report_unused_resolver_count = 0;
}

const fr_plugin_report *fr_plugins_report(void) {
    static fr_plugin_report view;
    view.entries = report_entries;
    view.count = report_count;
    view.unused_resolvers = report_unused_resolvers;
    view.unused_resolver_count = report_unused_resolver_count;
    return &view;
}

/* Appends one entry, owning copies of everything it stores: entry and declaration are both
   about to be freed by their callers (fr_plugins_free and fr_plugins_free_declaration), so
   nothing here may keep a pointer into either. origin is the url a URL or resolved entry fetched, or the
   resolved path a local entry read; resolved is the resolver's own answer, NULL for the two
   kinds core names itself. */
static int append_report_entry(const fr_plugin_entry *entry, const char *origin,
                               const char *resolved, const char *digest,
                               const fr_plugin_declaration *declaration, fr_error *err) {
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
    slot->resolved = fr_dup_string(resolved != NULL ? resolved : origin);
    slot->url = entry->kind != FR_PLUGIN_PATH ? fr_dup_string(origin) : NULL;
    slot->resolver = fr_dup_string(entry->resolver);
    slot->uses = declaration->uses_count > 0
                     ? malloc(declaration->uses_count * sizeof *slot->uses)
                     : NULL;
    if (slot->label == NULL || slot->resolved == NULL
        || (entry->kind != FR_PLUGIN_PATH && slot->url == NULL)
        || (entry->resolver != NULL && slot->resolver == NULL)
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
/* stat and not access: access(path, X_OK) returns 0 for a directory, so it
   cannot tell one from a file at all. */
static int path_is_directory(const char *path) {
#ifdef _WIN32
    struct _stat info;
    return _stat(path, &info) == 0 && (info.st_mode & _S_IFDIR) != 0;
#else
    struct stat info;
    return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
#endif
}

static int acquire(const fr_plugin_entry *entry, const fr_resolver_entry *resolvers,
                   size_t resolver_count, char **out_text, size_t *out_length, char **out_origin,
                   char **out_resolved, int *out_is_directory, fr_error *err) {
    *out_text = NULL;
    *out_length = 0;
    *out_origin = NULL;
    *out_resolved = NULL;
    *out_is_directory = 0;

    if (entry->kind == FR_PLUGIN_PATH) {
        lua_State *state = fr_lua_runtime_state();
        /* The directory resolver, although this may name a file: it is the one
           that opens both kinds. fopen refuses a directory on Windows, so
           resolving as a file first would fail before anything could ask which
           kind it is. Containment is the same rule either way. */
        if (fr_lua_sandbox_resolve_dir(state, entry->path, out_origin, err) != FR_OK) {
            return FR_ERR;
        }
        if (path_is_directory(*out_origin)) {
            /* No bytes to hand back: a directory has no single byte string, which
               is also why it cannot be pinned. Its members are read as they are
               asked for. */
            *out_is_directory = 1;
            return FR_OK;
        }
        if (fr_file_read_bytes(*out_origin, out_text, out_length, err) != FR_OK) {
            free(*out_origin);
            *out_origin = NULL;
            return FR_ERR;
        }
        return FR_OK;
    }

    fr_http_headers headers = { { { NULL, NULL } }, 0 };
    if (entry->kind == FR_PLUGIN_URL) {
        *out_origin = fr_dup_string(entry->url);
        if (*out_origin == NULL) return out_of_memory(entry->label, err);
    } else {
        const fr_resolver_entry *resolver =
            fr_resolvers_find(resolvers, resolver_count, entry->resolver);
        if (resolver == NULL) return unknown_resolver(entry, resolvers, resolver_count, err);
        if (fr_resolvers_use(resolver, entry->coordinate, out_origin, out_resolved, &headers, err)
            != FR_OK) {
            return FR_ERR;
        }
    }

    fr_http_header sent[FR_HTTP_MAX_HEADERS];
    size_t sent_count = fr_http_headers_borrow(&headers, sent);
    int status = fr_plugin_fetch(*out_origin, sent, sent_count, out_text, out_length, err);
    fr_http_headers_free(&headers);
    if (status != FR_OK) {
        free(*out_origin);
        free(*out_resolved);
        *out_origin = NULL;
        *out_resolved = NULL;
        return FR_ERR;
    }
    return FR_OK;
}

int fr_plugins_acquire_source(const fr_plugin_entry *entry, const fr_resolver_entry *resolvers,
                              size_t resolver_count, char **out_text, size_t *out_length,
                              char **out_origin, char **out_resolved, char out_digest[65],
                              fr_plugin_source **out_source, fr_error *err) {
    *out_text = NULL;
    *out_length = 0;
    *out_origin = NULL;
    *out_resolved = NULL;
    out_digest[0] = '\0';
    *out_source = NULL;

    int is_directory = 0;
    if (acquire(entry, resolvers, resolver_count, out_text, out_length, out_origin, out_resolved,
               &is_directory, err)
        != FR_OK) {
        return FR_ERR;
    }

    /* Computed for every plugin that HAS a byte string, pinned or not: an
       unpinned entry's digest is what "daukle config print" shows, so adopting a
       pin is a copy and a paste. Checked before the source is opened, which for
       an archive already reads its central directory: verifying after would
       mean unpinned bytes had already been trusted that far. */
    if (!is_directory) {
        fr_sha256_hex(*out_text, *out_length, out_digest);
        if (entry->sha256 != NULL && !fr_sha256_hex_equal(out_digest, entry->sha256)) {
            fr_error_set(err, "plugin \"%s\": expected sha256 %s but the file is %s",
                        entry->label, entry->sha256, out_digest);
            if (entry->kind != FR_PLUGIN_PATH) fr_plugin_fetch_discard(*out_origin);
            free(*out_text); *out_text = NULL;
            free(*out_origin); *out_origin = NULL;
            free(*out_resolved); *out_resolved = NULL;
            return FR_ERR;
        }
    } else if (entry->sha256 != NULL) {
        /* Refused rather than approximated: a digest over some canonical reading
           of a directory would be neither the published archive's digest nor
           anything a standard tool reproduces, so copying it out of
           "daukle config print" would fail the day the entry became a url. */
        fr_error_set(err, "plugin \"%s\": \"%s\" is a directory and cannot carry a sha256; pin the"
                          " published archive instead",
                    entry->label, entry->path);
        free(*out_text); *out_text = NULL;
        free(*out_origin); *out_origin = NULL;
        free(*out_resolved); *out_resolved = NULL;
        return FR_ERR;
    }

    fr_plugin_source *source = NULL;
    int opened = is_directory
                     ? fr_plugin_source_open_directory(fr_lua_runtime_state(), entry->path, &source,
                                                       err)
                     : fr_plugin_source_open_bytes(*out_text, *out_length, &source, err);
    if (opened != FR_OK) {
        free(*out_text); *out_text = NULL;
        free(*out_origin); *out_origin = NULL;
        free(*out_resolved); *out_resolved = NULL;
        return FR_ERR;
    }

    *out_source = source;
    return FR_OK;
}

static int load_one(const fr_plugin_entry *entry, const fr_resolver_entry *resolvers,
                    size_t resolver_count, fr_error *err) {
    char *origin = NULL;
    char *text = NULL;
    char *resolved = NULL;
    size_t length = 0;
    char digest[65];
    fr_plugin_source *source = NULL;
    if (fr_plugins_acquire_source(entry, resolvers, resolver_count, &text, &length, &origin,
                                  &resolved, digest, &source, err)
        != FR_OK) {
        return FR_ERR;
    }

    /* The digest above covers the whole artifact; what RUNS is its entry, which
       for an archive is one member of it. */
    const char *chunk = NULL;
    size_t chunk_length = 0;
    int status = fr_plugin_source_entry(source, &chunk, &chunk_length, err);

    lua_State *state = fr_lua_runtime_state();
    fr_plugin_declaration declaration;
    memset(&declaration, 0, sizeof declaration);
    declaration.kind = "plugin";
    declaration.label = entry->label;
    if (status == FR_OK) {
        status = read_declaration(state, chunk, chunk_length, origin, &declaration, err);
    }

    fr_plugin_deps *deps = NULL;
    if (status == FR_OK) {
        status = fr_plugin_deps_acquire(&declaration, entry->overrides, resolvers, resolver_count,
                                        &deps, err);
    }
    if (status == FR_OK) {
        status = fr_lua_plugin_load(chunk, chunk_length, origin,
                                    (const char *const *) declaration.uses,
                                    declaration.uses_count, source, deps, err);
    }
    if (status == FR_OK) {
        status = append_report_entry(entry, origin, resolved, digest, &declaration, err);
    }

    fr_plugin_deps_close(deps);
    fr_plugins_free_declaration(&declaration);
    fr_plugin_source_close(source);
    free(text);
    free(origin);
    free(resolved);
    return status;
}

/* Declared-versus-referenced is a static comparison over the parsed entries, not
   something discovered by acquiring a resolver: section 4.1 deliberately never fetches
   or runs a resolver no plugin names, so this must not either. */
static int record_unused_resolvers(const fr_plugin_entry *entries, size_t entry_count,
                                   const fr_resolver_entry *resolvers, size_t resolver_count,
                                   fr_error *err) {
    if (resolver_count == 0) return FR_OK;

    char **unused = malloc(resolver_count * sizeof *unused);
    if (unused == NULL) {
        fr_error_set(err, "out of memory recording unused resolvers");
        return FR_ERR;
    }

    size_t unused_count = 0;
    for (size_t index = 0; index < resolver_count; index++) {
        const char *label = resolvers[index].label;
        int used = 0;
        for (size_t entry_index = 0; entry_index < entry_count && !used; entry_index++) {
            used = entries[entry_index].kind == FR_PLUGIN_RESOLVED
                && strcmp(entries[entry_index].resolver, label) == 0;
        }
        if (used) continue;

        char *copy = fr_dup_string(label);
        if (copy == NULL) {
            for (size_t free_index = 0; free_index < unused_count; free_index++) free(unused[free_index]);
            free(unused);
            fr_error_set(err, "out of memory recording unused resolvers");
            return FR_ERR;
        }
        unused[unused_count] = copy;
        unused_count++;
    }

    if (unused_count == 0) {
        free(unused);
        return FR_OK;
    }

    report_unused_resolvers = unused;
    report_unused_resolver_count = unused_count;
    return FR_OK;
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
    if (status == FR_OK) {
        status = record_unused_resolvers(entries, count, resolvers, resolver_count, err);
    }
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

    int was_refreshing = fr_cache_refreshing();
    int was_enabled = fr_cache_enabled();
    fr_cache_set_refreshing(1);
    fr_cache_set_enabled(1);

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
                fr_http_headers headers = { { { NULL, NULL } }, 0 };
                status = fr_resolvers_use(resolver, entry->coordinate, &url, &resolved, &headers,
                                          err);
                if (status == FR_OK) {
                    fr_plugin_fetch_discard(url);
                    (*out_removed_count)++;
                }
                fr_http_headers_free(&headers);
                free(url);
                free(resolved);
            }
        }

        if (label != NULL) break;
    }

    fr_cache_set_refreshing(was_refreshing);
    fr_cache_set_enabled(was_enabled);

    if (status != FR_OK) return FR_ERR;
    if (!matched) {
        fr_error_set(err, "no plugin named \"%s\"", label);
        return FR_ERR;
    }
    return FR_OK;
}
