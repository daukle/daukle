#include "lua_verbs.h"

#include "cache.h"
#include "config_json.h"
#include "config_lua.h"
#include "error.h"
#include "exec.h"
#include "http.h"
#include "jsonedit.h"
#include "lang_region.h"
#include "lua_sandbox.h"
#include "luax.h"
#include "region.h"
#include "registry.h"
#include "tool.h"

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
    "publish"
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

static int verb_env(lua_State *state) {
    const char *name = luaL_checkstring(state, 1);
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
    if (fr_config_json_parse(text, &document, &err) != FR_OK) {
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

typedef struct {
    char path[1024];
    char name[128];
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

static int verb_tool(lua_State *state) {
    const char *name = luaL_checkstring(state, 1);
    if (!tool_name_is_valid(name)) {
        return luaL_error(state, "\"%s\" is not a tool name: a tool is named, not pathed", name);
    }

    char *path = NULL;
    fr_error err;
    if (fr_tool_resolve(name, &path, &err) != FR_OK) return luaL_error(state, "%s", err.message);

    fr_lua_tool *handle = lua_newuserdatauv(state, sizeof *handle, 0);
    snprintf(handle->path, sizeof handle->path, "%s", path);
    snprintf(handle->name, sizeof handle->name, "%s", name);
    free(path);

    luaL_getmetatable(state, FR_TOOL_HANDLE);
    lua_setmetatable(state, -2);
    return 1;
}

static int option_flag(lua_State *state, int index, const char *key, int fallback) {
    if (lua_type(state, index) != LUA_TTABLE) return fallback;
    lua_getfield(state, index, key);
    int value = lua_isnil(state, -1) ? fallback : lua_toboolean(state, -1);
    lua_pop(state, 1);
    return value;
}

static void report_verbose_exec(const fr_lua_tool *handle, const char *const *argv,
                                lua_Integer argv_count, const char *cwd) {
    fprintf(stderr, "exec %s (%s)\n", handle->name, handle->path);
    fprintf(stderr, "  args:");
    for (lua_Integer index = 0; index < argv_count; index++) fprintf(stderr, " %s", argv[index]);
    fprintf(stderr, "\n  cwd:  %s\n", cwd != NULL ? cwd : ".");
}

static int verb_exec(lua_State *state) {
    if (luaL_testudata(state, 1, FR_TOOL_HANDLE) == NULL) {
        return luaL_error(state, "daukle.exec argument 1 must be a tool handle from daukle.tool");
    }
    fr_lua_tool *handle = lua_touserdata(state, 1);
    luaL_checktype(state, 2, LUA_TTABLE);

    int capture = option_flag(state, 3, "capture", 0);
    int check = option_flag(state, 3, "check", 1);

    /* Left unpopped below: a numeric cwd's Lua-converted string needs the same anchor argv does. */
    int anchor_base = lua_gettop(state);
    const char *cwd = NULL;
    if (lua_type(state, 3) == LUA_TTABLE) {
        lua_getfield(state, 3, "cwd");
        if (!lua_isnil(state, -1)) {
            if (lua_type(state, -1) != LUA_TSTRING) {
                return luaL_error(state, "daukle.exec option \"cwd\" must be a string");
            }
            cwd = lua_tostring(state, -1);
        }
    }

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

    if (g_verbose) report_verbose_exec(handle, argv, count, cwd);

    fr_exec_request request;
    request.program = handle->path;
    request.argv = argv;
    request.argv_count = (size_t) count;
    request.cwd = cwd;
    request.capture = capture;

    fr_exec_result result;
    fr_error err;
    int status = fr_exec_run(&request, &result, &err);
    lua_settop(state, anchor_base);
    if (status != FR_OK) {
        return luaL_error(state, "%s", err.message);
    }

    if (check && result.code != 0) {
        int code = result.code;
        fr_exec_result_free(&result);
        return luaL_error(state, "%s exited with code %d", handle->name, code);
    }

    lua_newtable(state);
    lua_pushinteger(state, result.code);
    lua_setfield(state, -2, "code");
    if (capture) {
        /* Freed right after its own push, not batched, so a later raise cannot strand this one. */
        lua_pushstring(state, result.stdout_text != NULL ? result.stdout_text : "");
        free(result.stdout_text);
        result.stdout_text = NULL;
        lua_setfield(state, -2, "stdout");

        lua_pushstring(state, result.stderr_text != NULL ? result.stderr_text : "");
        free(result.stderr_text);
        result.stderr_text = NULL;
        lua_setfield(state, -2, "stderr");

        if (result.truncated) {
            lua_pushboolean(state, 1);
            lua_setfield(state, -2, "truncated");
        }
    }
    fr_exec_result_free(&result);
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
        lua_pushcfunction(state, verb_tool);
    } else if (strcmp(name, "exec") == 0) {
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
    luaL_newmetatable(state, FR_TOOL_HANDLE);
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

int fr_lua_verbs_push_env(lua_State *state, const char *const *verbs, size_t verb_count,
                          fr_error *err) {
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
