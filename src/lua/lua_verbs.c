#include "lua/lua_verbs.h"

#include "cache/cache.h"
#include "config/config_lua.h"
#include "config/jsonedit.h"
#include "config/jsonx.h"
#include "exec/exec.h"
#include "exec/tool.h"
#include "exec/toolreport.h"
#include "lua/lua_modules.h"
#include "lua/lua_sandbox.h"
#include "lua/luax.h"
#include "net/http.h"
#include "plugin/registry.h"
#include "project/lang_region.h"
#include "project/region.h"
#include "provision/provision.h"
#include "util/error.h"

#include "cJSON.h"
#include "lauxlib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FR_VERB_MAX_HEADERS 16
#define FR_VERB_MAX_ARRAY 256

static const char *BASE[] = {
    "assert", "error", "ipairs", "pairs", "next", "select", "tonumber", "tostring",
    "type", "setmetatable", "getmetatable", "string", "table", "math", "_VERSION"
};

static const char *KNOWN_VERBS[] = {
    "fetch", "read", "cache", "env", "region", "json_set", "json_parse", "parse", "exec", "tool",
    "provision", "artifact", "publish"
};

/* daukle.publish is spec section 10's next reserved name: a fourth table and
   a "daukle publish" command, not yet implemented. Keeping one name here
   pins the array's shape and keeps fr_lua_verbs_is_reserved's own test true
   for a name this version does not implement. */
static const char *RESERVED_VERBS[] = { "publish" };

int fr_lua_verbs_is_known(const char *name) {
    for (size_t index = 0; index < sizeof KNOWN_VERBS / sizeof KNOWN_VERBS[0]; index++) {
        if (strcmp(KNOWN_VERBS[index], name) == 0) return 1;
    }
    return 0;
}

int fr_lua_verbs_is_reserved(const char *name) {
    for (size_t index = 0; index < sizeof RESERVED_VERBS / sizeof RESERVED_VERBS[0]; index++) {
        if (strcmp(RESERVED_VERBS[index], name) == 0) return 1;
    }
    return 0;
}

/* The variables daukle itself reads, and therefore the only ones it is
   responsible for a child seeing. This is not a denylist of secrets, which
   child spec 1 refused as wrong by construction: that refusal is about guessing
   which of the USER's variables are secret, which is unknowable. These two are
   enumerated by reading daukle's own plugins, which ask for them by name.
   daukle cannot make a child safe; it can stop being the reason one sees a
   credential, and those are different claims. D-32. */
static const char *const CORE_CREDENTIALS[] = { "DAUKLE_TOKEN", "GITHUB_TOKEN" };

static const char *const *declared_env;
static size_t declared_env_count;

void fr_lua_verbs_set_declared_env(const char *const *names, size_t count) {
    declared_env = names;
    declared_env_count = count;
}

/* Case insensitive on Windows, where environment variable names are, so that a
   plugin declaring "DAUKLE_TOKEN" is not bypassed by a child inheriting
   "daukle_token". build_environment_block already follows this rule for
   overrides, and a control that followed a different one would be a hole. */
static int env_names_match(const char *left, const char *right) {
#ifdef _WIN32
    return _stricmp(left, right) == 0;
#else
    return strcmp(left, right) == 0;
#endif
}

int fr_lua_verbs_copy_declared_env(char ***out, size_t *out_count) {
    *out = NULL;
    *out_count = 0;
    if (declared_env_count == 0) return 0;

    char **copy = calloc(declared_env_count, sizeof *copy);
    if (copy == NULL) return -1;
    for (size_t index = 0; index < declared_env_count; index++) {
        size_t length = strlen(declared_env[index]) + 1;
        copy[index] = malloc(length);
        if (copy[index] == NULL) {
            for (size_t done = 0; done < index; done++) free(copy[done]);
            free(copy);
            return -1;
        }
        memcpy(copy[index], declared_env[index], length);
    }
    *out = copy;
    *out_count = declared_env_count;
    return 0;
}

int fr_lua_verbs_is_core_credential(const char *name) {
    for (size_t index = 0; index < sizeof CORE_CREDENTIALS / sizeof CORE_CREDENTIALS[0];
         index++) {
        if (env_names_match(name, CORE_CREDENTIALS[index])) return 1;
    }
    return 0;
}

int fr_lua_verbs_env_is_declared(const char *name) {
    for (size_t index = 0; index < declared_env_count; index++) {
        if (env_names_match(name, declared_env[index])) return 1;
    }
    return 0;
}

size_t fr_lua_verbs_env_to_scrub(const char **out, size_t limit) {
    size_t written = 0;
    for (size_t index = 0; index < sizeof CORE_CREDENTIALS / sizeof CORE_CREDENTIALS[0]
                           && written < limit; index++) {
        if (fr_lua_verbs_env_is_declared(CORE_CREDENTIALS[index])) continue;
        out[written] = CORE_CREDENTIALS[index];
        written++;
    }
    return written;
}

static int env_declared_exec;
static int env_declared_tool;
static int env_declared_provision;
static int env_declared_artifact;

int fr_lua_verbs_env_declared_exec(void) {
    return env_declared_exec;
}

int fr_lua_verbs_env_declared_tool(void) {
    return env_declared_tool;
}

int fr_lua_verbs_env_declared_provision(void) {
    return env_declared_provision;
}

int fr_lua_verbs_env_declared_artifact(void) {
    return env_declared_artifact;
}

static int verb_env(lua_State *state) {
    const char *name = luaL_checkstring(state, 1);
    if (fr_lua_verbs_is_core_credential(name) && !fr_lua_verbs_env_is_declared(name)) {
        return luaL_error(state, "this plugin may not read %s: name it in env on"
                                 " daukle.plugin{}", name);
    }
    const char *value = getenv(name);
    if (value == NULL) {
        lua_pushnil(state);
    } else {
        lua_pushstring(state, value);
    }
    return 1;
}

static int verb_read(lua_State *state) {
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

static int reserved_verb(lua_State *state) {
    return luaL_error(state, "daukle.publish is not implemented in this version");
}

static int verb_fetch(lua_State *state) {
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

static int verb_cache(lua_State *state) {
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

static int verb_region(lua_State *state) {
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

static int verb_json_set(lua_State *state) {
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

static int verb_json_parse(lua_State *state) {
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

static int verb_parse(lua_State *state) {
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

#define FR_TOOL_HANDLE "daukle.tool"
#define FR_VERB_MAX_ARGV 256

#define FR_TOOL_NAME_MAX 128

typedef struct {
    char path[1024];
    char name[FR_TOOL_NAME_MAX];
} fr_lua_tool;

static int g_verbose;

void fr_lua_verbs_set_verbose(int enabled) {
    g_verbose = enabled;
}

/* A tool is named, not pathed: fr_lua_sandbox_climbs_out catches a leading
   separator, a drive letter and ".." anywhere, the same rule a relative
   include path is held to, and the interior-separator check on top of it is
   this verb's own, since an include path may legitimately nest into a
   subdirectory and a tool name never may. */
static int tool_name_is_valid(const char *name) {
    if (fr_lua_sandbox_climbs_out(name)) return 0;
    return strpbrk(name, "/\\") == NULL;
}

/* Reads the "as" field of the options table at table_index without honoring
   a metatable, for the same reason raw_getfield in config_lua.c does:
   __index is a base global reachable in this sandbox. The value stays on the
   stack, since the returned pointer points into it. */
static const char *tool_option_label(lua_State *state, int table_index) {
    lua_pushstring(state, "as");
    lua_rawget(state, table_index);
    return lua_type(state, -1) == LUA_TSTRING ? lua_tostring(state, -1) : NULL;
}

static int verb_tool(lua_State *state) {
    if (fr_lua_generation_is_running()) {
        return luaL_error(state, "daukle.tool is not available while generating; "
                                 "generation is a pure function of the manifest");
    }
    const char *name = luaL_checkstring(state, 1);
    const char *label = NULL;
    if (!lua_isnoneornil(state, 2)) {
        if (lua_type(state, 2) == LUA_TSTRING) {
            return luaL_error(state, "daukle.tool does not match version constraints: resolving"
                                     " one is the plugin's own business, not core's");
        }
        luaL_checktype(state, 2, LUA_TTABLE);
        label = tool_option_label(state, 2);
        if (label == NULL && !lua_isnil(state, -1)) {
            return luaL_error(state, "daukle.tool option \"as\" must be a string");
        }
        if (label != NULL && !fr_toolreport_label_is_safe(label)) {
            return luaL_error(state, "daukle.tool option \"as\" may not hold a control character");
        }
    }
    if (!tool_name_is_valid(name)) {
        return luaL_error(state, "\"%s\" is not a tool name: a tool is named, not pathed", name);
    }
    /* Checked before resolving, both so the cheap answer comes first and so
       the message can report a length without echoing an unbounded name. */
    if (strlen(name) >= FR_TOOL_NAME_MAX) {
        return luaL_error(state, "the tool name is too long (%d characters)", (int) strlen(name));
    }

    char *path = NULL;
    fr_error err;
    if (fr_tool_resolve(name, &path, &err) != FR_OK) return luaL_error(state, "%s", err.message);
    fr_toolreport_used_installed(name, label, path);

    fr_lua_tool *handle = lua_newuserdatauv(state, sizeof *handle, 0);
    int path_written = snprintf(handle->path, sizeof handle->path, "%s", path);
    free(path);
    if (path_written < 0 || (size_t) path_written >= sizeof handle->path) {
        return luaL_error(state, "the path \"%s\" resolves to is too long for a tool handle", name);
    }
    snprintf(handle->name, sizeof handle->name, "%s", name);

    luaL_getmetatable(state, FR_TOOL_HANDLE);
    lua_setmetatable(state, -2);
    if (g_verbose) fprintf(stderr, "tool %s (%s)\n", handle->name, handle->path);
    return 1;
}

#define FR_PROVISION_HANDLE "daukle.provision.root"

/* Sized like fr_provision_result.root, so taking that root cannot truncate. */
#define FR_PROVISION_ROOT_MAX 1024

typedef struct {
    char root[FR_PROVISION_ROOT_MAX];
} fr_lua_root;

static const char *const PROVISION_FIELDS[] = { "url", "sha256", "as", "headers" };

static int provision_field_is_known(const char *key) {
    for (size_t index = 0; index < sizeof PROVISION_FIELDS / sizeof PROVISION_FIELDS[0]; index++) {
        if (strcmp(PROVISION_FIELDS[index], key) == 0) return 1;
    }
    return 0;
}

/* Refused by name rather than ignored: a silently dropped version = "21" is a
   user believing they pinned something. The message names whichever key the
   walk reached and never a fixed one, because lua_next's order over a plugin's
   table is unspecified, so with two unknown keys either may be the one seen. */
static int refuse_an_unknown_provision_field(lua_State *state, const char *verb) {
    lua_pushnil(state);
    while (lua_next(state, 1) != 0) {
        if (lua_type(state, -2) != LUA_TSTRING) {
            return luaL_error(state, "%s takes named fields only", verb);
        }
        const char *key = lua_tostring(state, -2);
        if (!provision_field_is_known(key)) {
            return luaL_error(state, "%s does not take \"%s\"; it takes url, sha256"
                                     ", as and headers", verb, key);
        }
        lua_pop(state, 1);
    }
    return 0;
}

/* Raw, never lua_getfield: setmetatable is a base global a plugin keeps, so a
   field answered by an __index could hand one digest to the check here and a
   different one to the fetch, and the digest read IS the whole enforcement of
   the pin. The value stays on the stack, since the answer points into it. */
static const char *provision_field(lua_State *state, const char *key) {
    lua_pushstring(state, key);
    lua_rawget(state, 1);
    return lua_type(state, -1) == LUA_TSTRING ? lua_tostring(state, -1) : NULL;
}

/* One "%s" and no other specifier. The template exists so that "Bearer " is not
   written into core, and it is one specifier precisely so it cannot become a
   format string a plugin controls. */
static int format_takes_one_string(const char *format) {
    int seen = 0;
    for (const char *scan = format; *scan != 0; scan++) {
        if (*scan != 37) continue;
        scan++;
        if (*scan == 37) continue;
        if (*scan != 115) return 0;
        seen++;
    }
    return seen == 1;
}


#define FR_PROVISION_MAX_HEADERS 8

/* Reads the "headers" table, resolving each value from the environment. A value
   is a table naming a variable, never a string: a literal would make a plugin,
   and the manifest that pins it, places a secret ends up, and both are
   committed. The variable must be one this plugin declared, so one allowlist
   governs reading and fetching rather than two. An unset variable omits the
   header rather than sending it empty, which is what plugins/github.lua already
   does: an empty Bearer is a worse diagnostic than an anonymous request. D-32. */
static size_t read_provision_headers(lua_State *state, fr_http_header *out) {
    lua_pushstring(state, "headers");
    lua_rawget(state, 1);
    if (lua_isnil(state, -1)) return 0;
    if (!lua_istable(state, -1)) {
        luaL_error(state, "daukle.provision field \"headers\" must be a table");
    }

    int headers_index = lua_gettop(state);
    size_t count = 0;
    lua_pushnil(state);
    while (lua_next(state, headers_index) != 0) {
        if (lua_type(state, -2) != LUA_TSTRING) {
            luaL_error(state, "daukle.provision: every header name must be a string");
        }
        const char *name = lua_tostring(state, -2);
        if (lua_type(state, -1) == LUA_TSTRING) {
            luaL_error(state, "daukle.provision: headers.%s must name an environment"
                              " variable, not a value", name);
        }
        if (!lua_istable(state, -1)) {
            luaL_error(state, "daukle.provision: headers.%s must be a table naming env", name);
        }
        if (count == FR_PROVISION_MAX_HEADERS) {
            luaL_error(state, "daukle.provision: more than %d headers",
                       FR_PROVISION_MAX_HEADERS);
        }

        lua_pushstring(state, "env");
        lua_rawget(state, -2);
        const char *variable = lua_type(state, -1) == LUA_TSTRING
                             ? lua_tostring(state, -1) : NULL;
        if (variable == NULL) {
            luaL_error(state, "daukle.provision: headers.%s needs an env string", name);
        }
        if (fr_lua_verbs_is_core_credential(variable)
            && !fr_lua_verbs_env_is_declared(variable)) {
            luaL_error(state, "daukle.provision: headers.%s names %s, which this plugin did"
                              " not declare in env", name, variable);
        }

        lua_pushstring(state, "format");
        lua_rawget(state, -3);
        const char *format = lua_type(state, -1) == LUA_TSTRING
                           ? lua_tostring(state, -1) : NULL;
        if (format != NULL && !format_takes_one_string(format)) {
            luaL_error(state, "daukle.provision: headers.%s format takes exactly one %%s", name);
        }

        const char *value = getenv(variable);
        if (value != NULL && value[0] != 0) {
            char *rendered = NULL;
            if (format != NULL) {
                size_t length = strlen(format) + strlen(value) + 1;
                rendered = malloc(length);
                if (rendered != NULL) snprintf(rendered, length, format, value);
            } else {
                rendered = fr_dup_string(value);
            }
            char *header_name = fr_dup_string(name);
            if (rendered == NULL || header_name == NULL) {
                free(rendered);
                free(header_name);
                luaL_error(state, "out of memory building the %s header", name);
            }
            out[count].name = header_name;
            out[count].value = rendered;
            count++;
        }

        lua_pop(state, 3);
    }
    return count;
}

static void free_provision_headers(fr_http_header *headers, size_t count) {
    for (size_t index = 0; index < count; index++) {
        free((char *) headers[index].name);
        free((char *) headers[index].value);
    }
}

static int verb_provision(lua_State *state) {
    if (fr_lua_generation_is_running()) {
        return luaL_error(state, "daukle.provision is not available while generating; "
                                 "generation is a pure function of the manifest");
    }
    /* The per-kind refusals in config_lua fire when daukle.language and its
       siblings are CALLED, by which time a chunk that provisioned at its top
       level has already put a tree on disk during `daukle check`. The refusal
       has to reach the call, exactly as daukle.exec's does. */
    if (fr_lua_plugin_exec_is_refused()) {
        return luaL_error(state, "daukle.provision is not available while a plugin chunk is"
                                 " loading; call it from a task or publish callback");
    }
    luaL_checktype(state, 1, LUA_TTABLE);
    refuse_an_unknown_provision_field(state, "daukle.provision");

    const char *url = provision_field(state, "url");
    if (url == NULL) return luaL_error(state, "daukle.provision needs a url string");

    const char *digest = provision_field(state, "sha256");
    if (digest == NULL) {
        return luaL_error(state, "daukle.provision needs a sha256 string: there is no unpinned"
                                 " form of it and no flag that relaxes one");
    }

    const char *label = provision_field(state, "as");
    if (label == NULL && !lua_isnil(state, -1)) {
        return luaL_error(state, "daukle.provision field \"as\" must be a string");
    }
    if (label != NULL && !fr_toolreport_label_is_safe(label)) {
        return luaL_error(state, "daukle.provision field \"as\" may not hold a control character");
    }

    fr_http_header headers[FR_PROVISION_MAX_HEADERS];
    memset(headers, 0, sizeof headers);
    size_t header_count = read_provision_headers(state, headers);

    fr_provision_result result;
    fr_error err;
    int provisioned = fr_provision(url, digest, header_count > 0 ? headers : NULL,
                                   header_count, &result, &err);
    free_provision_headers(headers, header_count);
    if (provisioned != FR_OK) {
        return luaL_error(state, "%s", err.message);
    }
    fr_toolreport_provisioned(label, url, digest, result.was_cached);
    fr_toolreport_symlinks_skipped(digest, result.unpack.symlinks_skipped,
                                   result.unpack.first_symlink_skipped);

    fr_lua_root *handle = lua_newuserdatauv(state, sizeof *handle, 0);
    snprintf(handle->root, sizeof handle->root, "%s", result.root);
    luaL_getmetatable(state, FR_PROVISION_HANDLE);
    lua_setmetatable(state, -2);
    return 1;
}

/* Returns a path string and not a handle, which is the opposite of root:tool and
   is deliberate: a tool handle is opaque so that daukle.exec can refuse an
   arbitrary string, and an artifact is never executed, only named to a tool as
   an argument. This widens what a plugin may NAME, exactly as root:path did,
   and not what it may RUN. */
static int verb_artifact(lua_State *state) {
    if (fr_lua_generation_is_running()) {
        return luaL_error(state, "daukle.artifact is not available while generating; "
                                 "generation is a pure function of the manifest");
    }
    if (fr_lua_plugin_exec_is_refused()) {
        return luaL_error(state, "daukle.artifact is not available while a plugin chunk is"
                                 " loading; call it from a task or publish callback");
    }
    luaL_checktype(state, 1, LUA_TTABLE);
    refuse_an_unknown_provision_field(state, "daukle.artifact");

    const char *url = provision_field(state, "url");
    if (url == NULL) return luaL_error(state, "daukle.artifact needs a url string");

    const char *digest = provision_field(state, "sha256");
    if (digest == NULL) {
        return luaL_error(state, "daukle.artifact needs a sha256 string: there is no unpinned"
                                 " form of it and no flag that relaxes one");
    }

    const char *label = provision_field(state, "as");
    if (label == NULL && !lua_isnil(state, -1)) {
        return luaL_error(state, "daukle.artifact field \"as\" must be a string");
    }
    if (label != NULL && !fr_toolreport_label_is_safe(label)) {
        return luaL_error(state, "daukle.artifact field \"as\" may not hold a control character");
    }

    fr_http_header headers[FR_PROVISION_MAX_HEADERS];
    memset(headers, 0, sizeof headers);
    size_t header_count = read_provision_headers(state, headers);

    fr_artifact_result result;
    fr_error err;
    int fetched = fr_artifact(url, digest, header_count > 0 ? headers : NULL, header_count,
                              &result, &err);
    free_provision_headers(headers, header_count);
    if (fetched != FR_OK) {
        return luaL_error(state, "%s", err.message);
    }
    fr_toolreport_artifact(label, url, digest, result.was_cached);

    lua_pushstring(state, result.path);
    return 1;
}

static const char *member_base_name(const char *member) {
    const char *slash = strrchr(member, '/');
    return slash != NULL ? slash + 1 : member;
}

/* The containment rules root:tool and root:path share, held in one place
   because the two differ only in what they do with a member that is there.
   A member is pathed where a tool name never is: the tree it indexes was
   chosen and verified by the plugin's own digest, so nesting into it is the
   point. The containment rule still holds, and a backslash is refused because
   it separates on one host only, so the same member would name a file there
   and a strangely named one everywhere else.

   It raises rather than returning on every refusal, so a caller never sees
   NULL and the NULL returns below are unreachable; they are there because
   luaL_error's int return cannot be forwarded from a function returning a
   pointer. */
static const char *root_member_path(lua_State *state, const char *verb, char *path, size_t size) {
    fr_lua_root *root = luaL_testudata(state, 1, FR_PROVISION_HANDLE);
    if (root == NULL) {
        luaL_error(state, "%s must be called on a root from daukle.provision", verb);
        return NULL;
    }
    const char *member = luaL_checkstring(state, 2);
    if (member[0] == '\0') {
        luaL_error(state, "a provisioned root's member needs a name");
        return NULL;
    }
    if (fr_lua_sandbox_climbs_out(member)) {
        luaL_error(state, "\"%s\" climbs out of the provisioned root", member);
        return NULL;
    }
    if (strchr(member, '\\') != NULL) {
        luaL_error(state, "\"%s\" names a member with a backslash; a member inside a"
                          " provisioned root is spelled with forward slashes", member);
        return NULL;
    }

    int written = snprintf(path, size, "%s/%s", root->root, member);
    if (written < 0 || (size_t) written >= size) {
        luaL_error(state, "\"%s\" is too long a path inside the provisioned root", member);
        return NULL;
    }
    return member;
}

/* Names a member without resolving it to something runnable. It exists because
   a tool handle is opaque and only ever executable, so without this a plugin
   cannot pass a provisioned script to a provisioned interpreter, which is the
   shape of npm under node and of the Gradle launcher under java. It widens what
   a plugin may NAME and not what it may RUN: daukle.exec takes a handle, never a
   string, so what this returns can only be an argument. */
static int root_path(lua_State *state) {
    char path[FR_PROVISION_ROOT_MAX];
    const char *member = root_member_path(state, "path", path, sizeof path);

    if (!fr_tool_is_regular_file(path)) {
        return luaL_error(state, "the provisioned root holds no \"%s\"", member);
    }

    /* A provisioned root inherits DAUKLE_CACHE_DIR verbatim and may be relative. */
    char *absolute = fr_tool_absolute_path(path);
    if (absolute == NULL) {
        return luaL_error(state, "\"%s\" is in the provisioned root but its absolute path could"
                                 " not be resolved", member);
    }
    lua_pushstring(state, absolute);
    free(absolute);
    return 1;
}

/* Names a DIRECTORY, and with no argument the root itself. It exists because a
   classpath entry is often a directory (a distribution's lib, or an unpacked
   tree) and nothing could name one: the only route was to name a file inside it
   and strip the member off the returned string, which is separator dependent and
   relies on a member a plugin merely expects to be there. Kept separate from
   root:path so each verb refuses the other's shape and a mistyped name fails
   loudly instead of resolving to something plausible. */
static int root_dir(lua_State *state) {
    fr_lua_root *root = luaL_testudata(state, 1, FR_PROVISION_HANDLE);
    if (root == NULL) {
        return luaL_error(state, "dir must be called on a root from daukle.provision");
    }

    char path[FR_PROVISION_ROOT_MAX];
    const char *member = NULL;
    if (lua_isnoneornil(state, 2)) {
        int written = snprintf(path, sizeof path, "%s", root->root);
        if (written < 0 || (size_t) written >= sizeof path) {
            return luaL_error(state, "the provisioned root's path is too long");
        }
    } else {
        member = root_member_path(state, "dir", path, sizeof path);
    }

    if (!fr_cache_directory_exists(path)) {
        return luaL_error(state, "the provisioned root holds no directory \"%s\"",
                          member == NULL ? "." : member);
    }

    /* A provisioned root inherits DAUKLE_CACHE_DIR verbatim and may be relative. */
    char *absolute = fr_tool_absolute_path(path);
    if (absolute == NULL) {
        return luaL_error(state, "\"%s\" is in the provisioned root but its absolute path could"
                                 " not be resolved", member == NULL ? "." : member);
    }
    lua_pushstring(state, absolute);
    free(absolute);
    return 1;
}

static int root_tool(lua_State *state) {
    char path[FR_PROVISION_ROOT_MAX];
    const char *member = root_member_path(state, "tool", path, sizeof path);

    if (fr_tool_is_batch_file(member)) {
        return luaL_error(state, FR_TOOL_BATCH_REFUSAL, member, path);
    }
    /* No extension is guessed: the plugin already branches on host.os to choose
       which archive to pin, so it can name the member that archive holds. */
    if (!fr_tool_is_executable_file(path)) {
        return luaL_error(state, "the provisioned root holds no executable \"%s\"", member);
    }

    /* A provisioned root inherits DAUKLE_CACHE_DIR verbatim and may be relative. */
    char *absolute = fr_tool_absolute_path(path);
    if (absolute == NULL) {
        return luaL_error(state, "\"%s\" is in the provisioned root but its absolute path could"
                                 " not be resolved", member);
    }

    fr_lua_tool *handle = lua_newuserdatauv(state, sizeof *handle, 0);
    int path_written = snprintf(handle->path, sizeof handle->path, "%s", absolute);
    free(absolute);
    if (path_written < 0 || (size_t) path_written >= sizeof handle->path) {
        return luaL_error(state, "the absolute path of \"%s\" is too long for a tool handle",
                          member);
    }
    snprintf(handle->name, sizeof handle->name, "%s", member_base_name(member));
    luaL_getmetatable(state, FR_TOOL_HANDLE);
    lua_setmetatable(state, -2);
    if (g_verbose) fprintf(stderr, "tool %s (%s)\n", handle->name, handle->path);
    return 1;
}

static int option_flag(lua_State *state, int index, const char *key, int fallback) {
    if (lua_type(state, index) != LUA_TTABLE) return fallback;
    lua_getfield(state, index, key);
    int value = lua_isnil(state, -1) ? fallback : lua_toboolean(state, -1);
    lua_pop(state, 1);
    return value;
}

#define FR_VERB_MAX_ENV 64

static const char *EXEC_OPTIONS[] = { "cwd", "capture", "check", "env" };

/* A key daukle does not know is a key daukle must refuse. An ignored "env"
   was the whole of D-17: a plugin written from child spec 1 set a credential,
   daukle discarded it without a word, and the tool then failed for a reason
   that named neither. Returns nonzero with the error already pushed. */
static int refuse_unknown_exec_option(lua_State *state, int index) {
    if (lua_type(state, index) != LUA_TTABLE) return 0;
    lua_pushnil(state);
    while (lua_next(state, index) != 0) {
        if (lua_type(state, -2) != LUA_TSTRING) {
            lua_pop(state, 2);
            lua_pushstring(state, "daukle.exec option keys must be strings");
            return 1;
        }
        const char *key = lua_tostring(state, -2);
        int known = 0;
        for (size_t option = 0; option < sizeof EXEC_OPTIONS / sizeof EXEC_OPTIONS[0]; option++) {
            if (strcmp(EXEC_OPTIONS[option], key) == 0) { known = 1; break; }
        }
        if (!known) {
            lua_pushfstring(state, "\"%s\" is not a daukle.exec option; only cwd, capture, check"
                                   " and env are", key);
            lua_remove(state, -2);
            lua_remove(state, -2);
            return 1;
        }
        lua_pop(state, 1);
    }
    return 0;
}

/* Space-joining alone would print {"a b"} and {"a","b"} identically, and the
   line would read like the shell command daukle deliberately never builds. */
static int argument_needs_showing_as_one(const char *argument) {
    return argument[0] == '\0' || strpbrk(argument, " \t") != NULL;
}

static void report_verbose_exec(const fr_lua_tool *handle, const char *const *argv,
                                lua_Integer argv_count, const char *cwd) {
    fprintf(stderr, "exec %s (%s)\n", handle->name, handle->path);
    fprintf(stderr, "  args:");
    for (lua_Integer index = 0; index < argv_count; index++) {
        const char *argument = argv[index];
        if (argument_needs_showing_as_one(argument)) fprintf(stderr, " [%s]", argument);
        else fprintf(stderr, " %s", argument);
    }
    fprintf(stderr, "\n  cwd:  %s\n", cwd != NULL ? cwd : ".");
}

static int verb_exec(lua_State *state) {
    if (fr_lua_generation_is_running()) {
        return luaL_error(state, "daukle.exec is not available while generating; "
                                 "generation is a pure function of the manifest");
    }
    if (fr_lua_plugin_exec_is_refused()) {
        return luaL_error(state, "daukle.exec is not available while a plugin chunk is"
                                 " loading; call it from a task or publish callback");
    }
    if (luaL_testudata(state, 1, FR_TOOL_HANDLE) == NULL) {
        return luaL_error(state, "daukle.exec argument 1 must be a tool handle from daukle.tool");
    }
    fr_lua_tool *handle = lua_touserdata(state, 1);
    luaL_checktype(state, 2, LUA_TTABLE);

    if (refuse_unknown_exec_option(state, 3) != 0) return lua_error(state);

    int capture = option_flag(state, 3, "capture", 0);
    int check = option_flag(state, 3, "check", 1);

    int anchor_base = lua_gettop(state);
    const char *requested_cwd = NULL;
    if (lua_type(state, 3) == LUA_TTABLE) {
        lua_getfield(state, 3, "cwd");
        if (!lua_isnil(state, -1)) {
            if (lua_type(state, -1) != LUA_TSTRING) {
                return luaL_error(state, "daukle.exec option \"cwd\" must be a string");
            }
            requested_cwd = lua_tostring(state, -1);
        }
    }
    if (requested_cwd == NULL) requested_cwd = fr_lua_task_cwd();

    lua_Integer count = luaL_len(state, 2);
    if (count < 0 || count > FR_VERB_MAX_ARGV) {
        return luaL_error(state, "daukle.exec takes at most %d arguments", FR_VERB_MAX_ARGV);
    }
    if (!lua_checkstack(state, (int) count + 4)) {
        return luaL_error(state, "cannot grow the lua stack for %d arguments", (int) count);
    }

    const char *argv[FR_VERB_MAX_ARGV];
    for (lua_Integer index = 1; index <= count; index++) {
        lua_geti(state, 2, index);
        if (lua_type(state, -1) != LUA_TSTRING) {
            return luaL_error(state, "daukle.exec argument %d is not a string",
                              (int) index);
        }
        /* Left on the stack; argv[] points into these until fr_exec_run returns. */
        argv[index - 1] = lua_tostring(state, -1);
    }

    fr_error err;
    char *cwd = NULL;
    if (requested_cwd != NULL
        && fr_lua_sandbox_resolve_dir(state, requested_cwd, &cwd, &err) != FR_OK) {
        return luaL_error(state, "%s", err.message);
    }

    if (g_verbose) report_verbose_exec(handle, argv, count, cwd);

    /* The names and values stay on the lua stack below anchor_base until
       fr_exec_run returns, exactly as argv[] does. */
    fr_exec_env_entry env_entries[FR_VERB_MAX_ENV];
    size_t env_count = 0;
    if (lua_type(state, 3) == LUA_TTABLE) {
        lua_getfield(state, 3, "env");
        if (!lua_isnil(state, -1)) {
            if (lua_type(state, -1) != LUA_TTABLE) {
                free(cwd);
                return luaL_error(state, "daukle.exec option \"env\" must be a table");
            }
            int table = lua_gettop(state);
            lua_pushnil(state);
            while (lua_next(state, table) != 0) {
                if (env_count == FR_VERB_MAX_ENV) {
                    free(cwd);
                    return luaL_error(state, "daukle.exec takes at most %d environment entries",
                                      FR_VERB_MAX_ENV);
                }
                if (lua_type(state, -2) != LUA_TSTRING || lua_type(state, -1) != LUA_TSTRING) {
                    free(cwd);
                    return luaL_error(state, "every daukle.exec env name and value must be"
                                             " a string");
                }
                const char *name = lua_tostring(state, -2);
                if (name[0] == '\0' || strchr(name, '=') != NULL) {
                    free(cwd);
                    return luaL_error(state, "a daukle.exec env name may not be empty or hold"
                                             " \"=\", found \"%s\"", name);
                }
                env_entries[env_count].name = name;
                env_entries[env_count].value = lua_tostring(state, -1);
                env_count++;
                lua_pop(state, 1);
            }
        }
    }

    fr_exec_request request;
    memset(&request, 0, sizeof request);
    request.program = handle->path;
    request.argv = argv;
    request.argv_count = (size_t) count;
    request.cwd = cwd;
    request.capture = capture;
    request.env = env_count > 0 ? env_entries : NULL;
    request.env_count = env_count;

    /* Every credential core reads that this plugin did not declare. The child
       inherits the rest of the environment untouched, including whatever the
       user exported: daukle cannot know which of those are secret and does not
       guess. What it can do is stop being the reason a child sees one of its
       own. D-32. */
    const char *scrub[FR_LUA_VERBS_MAX_SCRUB];
    size_t scrub_count = fr_lua_verbs_env_to_scrub(scrub, FR_LUA_VERBS_MAX_SCRUB);
    request.scrub = scrub_count > 0 ? scrub : NULL;
    request.scrub_count = scrub_count;

    fr_exec_result result;
    int status = fr_exec_run(&request, &result, &err);
    free(cwd);
    lua_settop(state, anchor_base);
    if (status != FR_OK) {
        return luaL_error(state, "%s", err.message);
    }

    if (check && result.code != 0) {
        int code = result.code;
        fr_exec_result_free(&result);
        return luaL_error(state, "%s exited with code %d", handle->name, code);
    }

    /* Both buffers are pushed and freed before lua_newtable runs, so its own OOM raise cannot strand either. */
    int stdout_index = 0;
    int stderr_index = 0;
    if (capture) {
        lua_pushstring(state, result.stdout_text != NULL ? result.stdout_text : "");
        stdout_index = lua_gettop(state);
        free(result.stdout_text);
        result.stdout_text = NULL;

        lua_pushstring(state, result.stderr_text != NULL ? result.stderr_text : "");
        stderr_index = lua_gettop(state);
        free(result.stderr_text);
        result.stderr_text = NULL;
    }
    fr_exec_result_free(&result);

    lua_newtable(state);
    lua_pushinteger(state, result.code);
    lua_setfield(state, -2, "code");
    if (capture) {
        lua_pushvalue(state, stdout_index);
        lua_setfield(state, -2, "stdout");
        lua_pushvalue(state, stderr_index);
        lua_setfield(state, -2, "stderr");
        if (result.truncated) {
            lua_pushboolean(state, 1);
            lua_setfield(state, -2, "truncated");
        }
    }
    return 1;
}

/* install_one handles every declared verb plus the reserved publish; a name
   that is known (fr_lua_verbs_is_known) but neither handled here nor reserved
   would be left unset, which cannot currently happen. */
static void install_one(lua_State *state, const char *name) {
    if (strcmp(name, "env") == 0) {
        lua_pushcfunction(state, verb_env);
    } else if (strcmp(name, "read") == 0) {
        lua_pushcfunction(state, verb_read);
    } else if (strcmp(name, "fetch") == 0) {
        lua_pushcfunction(state, verb_fetch);
    } else if (strcmp(name, "cache") == 0) {
        lua_pushcfunction(state, verb_cache);
    } else if (strcmp(name, "region") == 0) {
        lua_pushcfunction(state, verb_region);
    } else if (strcmp(name, "json_set") == 0) {
        lua_pushcfunction(state, verb_json_set);
    } else if (strcmp(name, "json_parse") == 0) {
        lua_pushcfunction(state, verb_json_parse);
    } else if (strcmp(name, "parse") == 0) {
        lua_pushcfunction(state, verb_parse);
    } else if (strcmp(name, "tool") == 0) {
        env_declared_tool = 1;
        lua_pushcfunction(state, verb_tool);
    } else if (strcmp(name, "provision") == 0) {
        env_declared_provision = 1;
        lua_pushcfunction(state, verb_provision);
    } else if (strcmp(name, "artifact") == 0) {
        env_declared_artifact = 1;
        lua_pushcfunction(state, verb_artifact);
    } else if (strcmp(name, "exec") == 0) {
        env_declared_exec = 1;
        lua_pushcfunction(state, verb_exec);
    } else if (fr_lua_verbs_is_reserved(name)) {
        lua_pushcfunction(state, reserved_verb);
    } else {
        return;
    }
    lua_setfield(state, -2, name);
}

static int undeclared_verb(lua_State *state) {
    const char *name = lua_tostring(state, 2);
    return luaL_error(state, "daukle.%s was not declared in uses",
                      name != NULL ? name : "?");
}

static const char *const *pending_verbs;
static size_t pending_verb_count;

static int protected_push_env(lua_State *state) {
    /* getmetatable and setmetatable are both base globals a plugin keeps, and
       this metatable is shared by every state, so leaving it reachable would
       be a cross-plugin channel the sandbox does not otherwise have. */
    luaL_newmetatable(state, FR_TOOL_HANDLE);
    lua_pushboolean(state, 0);
    lua_setfield(state, -2, "__metatable");
    lua_pop(state, 1);

    luaL_newmetatable(state, FR_PROVISION_HANDLE);
    lua_pushboolean(state, 0);
    lua_setfield(state, -2, "__metatable");
    lua_newtable(state);
    lua_pushcfunction(state, root_tool);
    lua_setfield(state, -2, "tool");
    lua_pushcfunction(state, root_path);
    lua_setfield(state, -2, "path");
    lua_pushcfunction(state, root_dir);
    lua_setfield(state, -2, "dir");
    lua_setfield(state, -2, "__index");
    lua_pop(state, 1);

    lua_newtable(state);
    for (size_t index = 0; index < sizeof BASE / sizeof BASE[0]; index++) {
        lua_getglobal(state, BASE[index]);
        lua_setfield(state, -2, BASE[index]);
    }

    lua_newtable(state);
    fr_lua_verbs_install_registration(state);
    for (size_t index = 0; index < pending_verb_count; index++) {
        install_one(state, pending_verbs[index]);
    }

    lua_newtable(state);
    lua_pushcfunction(state, undeclared_verb);
    lua_setfield(state, -2, "__index");
    lua_pushstring(state, "the daukle plugin environment");
    lua_setfield(state, -2, "__metatable");
    lua_setmetatable(state, -2);

    lua_setfield(state, -2, "daukle");

    lua_pushvalue(state, -1);
    lua_setfield(state, -2, "_G");
    return 1;
}

/* dependent and member are upvalues, not file statics: this __index is
   rebuilt fresh by every push_library_env call, and daukle.require can nest
   one library environment inside another, so a shared static would let the
   inner call's names overwrite the outer, still-live environment's raiser. */
static int outside_the_library_env(lua_State *state) {
    const char *name = lua_tostring(state, 2);
    const char *dependent = lua_tostring(state, lua_upvalueindex(1));
    const char *member = lua_tostring(state, lua_upvalueindex(2));
    return luaL_error(state, "daukle.%s is not available to \"%s\", required by \"%s\" across a"
                             " plugin boundary",
                      name != NULL ? name : "?", member, dependent);
}

static int protected_push_library_env(lua_State *state) {
    const char *dependent = lua_tostring(state, 1);
    const char *member = lua_tostring(state, 2);

    lua_newtable(state);
    for (size_t index = 0; index < sizeof BASE / sizeof BASE[0]; index++) {
        lua_getglobal(state, BASE[index]);
        lua_setfield(state, -2, BASE[index]);
    }

    lua_newtable(state);
    lua_pushcfunction(state, fr_lua_verbs_require_function());
    lua_setfield(state, -2, "require");

    lua_newtable(state);
    lua_pushstring(state, dependent);
    lua_pushstring(state, member);
    lua_pushcclosure(state, outside_the_library_env, 2);
    lua_setfield(state, -2, "__index");
    lua_pushstring(state, "the daukle library environment");
    lua_setfield(state, -2, "__metatable");
    lua_setmetatable(state, -2);

    lua_setfield(state, -2, "daukle");

    lua_pushvalue(state, -1);
    lua_setfield(state, -2, "_G");
    return 1;
}

int fr_lua_verbs_push_library_env(lua_State *state, const char *dependent, const char *member,
                                  fr_error *err) {
    lua_pushcfunction(state, protected_push_library_env);
    lua_pushstring(state, dependent);
    lua_pushstring(state, member);
    int status = lua_pcall(state, 2, 1, 0);
    if (status != LUA_OK) {
        fr_error_set(err, "%s", fr_lua_error_text(state));
        lua_pop(state, 1);
        return FR_ERR;
    }
    return FR_OK;
}

int fr_lua_verbs_push_env(lua_State *state, const char *const *verbs, size_t verb_count,
                          fr_error *err) {
    env_declared_exec = 0;
    env_declared_tool = 0;
    env_declared_provision = 0;
    env_declared_artifact = 0;
    pending_verbs = verbs;
    pending_verb_count = verb_count;
    lua_pushcfunction(state, protected_push_env);
    int status = lua_pcall(state, 0, 1, 0);
    pending_verbs = NULL;
    pending_verb_count = 0;

    if (status != LUA_OK) {
        fr_error_set(err, "%s", fr_lua_error_text(state));
        lua_pop(state, 1);
        return FR_ERR;
    }
    return FR_OK;
}
