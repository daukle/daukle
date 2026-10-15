#include "lua/lua_verbs.h"
#include "lua/lua_verbs_internal.h"

#include "config/config_lua.h"
#include "exec/exec.h"
#include "exec/tool.h"
#include "exec/toolreport.h"
#include "lua/lua_sandbox.h"

#include "lauxlib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FR_VERB_MAX_ARGV 256
#define FR_VERB_MAX_ENV 64

/* Reads the "as" field of the options table at table_index without honoring
   a metatable, for the same reason raw_getfield in config_lua.c does:
   __index is a base global reachable in this sandbox. The value stays on the
   stack, since the returned pointer points into it. */
static const char *tool_option_label(lua_State *state, int table_index) {
    lua_pushstring(state, "as");
    lua_rawget(state, table_index);
    return lua_type(state, -1) == LUA_TSTRING ? lua_tostring(state, -1) : NULL;
}

int fr_lua_verb_tool(lua_State *state) {
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
    if (!fr_tool_name_is_valid(name)) {
        return luaL_error(state, FR_TOOL_NOT_A_NAME_REFUSAL, name);
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
    if (fr_lua_verbs_is_verbose()) fprintf(stderr, "tool %s (%s)\n", handle->name, handle->path);
    return 1;
}

static int option_flag(lua_State *state, int index, const char *key, int fallback) {
    if (lua_type(state, index) != LUA_TTABLE) return fallback;
    lua_getfield(state, index, key);
    int value = lua_isnil(state, -1) ? fallback : lua_toboolean(state, -1);
    lua_pop(state, 1);
    return value;
}

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

int fr_lua_verb_exec(lua_State *state) {
    if (fr_lua_generation_is_running()) {
        return luaL_error(state, "daukle.exec is not available while generating; "
                                 "generation is a pure function of the manifest");
    }
    if (fr_lua_plugin_exec_is_refused()) {
        return luaL_error(state, "daukle.exec is not available while a plugin chunk is"
                                 " loading; call it from a task or publish callback");
    }
    const char *phase = fr_lua_process_free_callback_phase();
    if (phase != NULL) {
        return luaL_error(state, "daukle.exec is not available while %s; call it from a task or"
                                 " publish callback", phase);
    }
    if (!fr_lua_task_dispatch_is_running()) {
        return luaL_error(state, "daukle.exec is available only inside a task or publish callback");
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

    if (fr_lua_verbs_is_verbose()) report_verbose_exec(handle, argv, count, cwd);

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
