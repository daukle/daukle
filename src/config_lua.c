#include "config_lua.h"

#include "error.h"
#include "luax.h"
#include "lua_sandbox.h"

#include "cJSON.h"
#include "lauxlib.h"

#include <stdlib.h>
#include <string.h>

static lua_State *runtime_state = NULL;
static void (*log_sink)(const char *message) = NULL;

static const char *host_os(void) {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
}

static const char *host_arch(void) {
#if defined(__aarch64__) || defined(_M_ARM64)
    return "aarch64";
#elif defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#else
    return "x86";
#endif
}

static int lua_env(lua_State *state) {
    const char *name = luaL_checkstring(state, 1);
    const char *value = getenv(name);
    if (value == NULL) {
        lua_pushnil(state);
        return 1;
    }
    lua_getglobal(state, "daukle");
    lua_getfield(state, -1, "_env_reads");
    lua_pushstring(state, value);
    lua_setfield(state, -2, name);
    lua_pop(state, 2);
    lua_pushstring(state, value);
    return 1;
}

static int lua_log(lua_State *state) {
    const char *message = luaL_checkstring(state, 1);
    if (log_sink != NULL) log_sink(message);
    return 0;
}

static int publish_daukle_table(lua_State *state, const cJSON *document, fr_error *err) {
    lua_getglobal(state, "daukle");

    if (document != NULL) {
        if (fr_lua_push_json(state, document, err) != FR_OK) return FR_ERR;
    } else {
        lua_newtable(state);
    }
    lua_setfield(state, -2, "config");

    lua_newtable(state);
    lua_pushstring(state, host_os());
    lua_setfield(state, -2, "os");
    lua_pushstring(state, host_arch());
    lua_setfield(state, -2, "arch");
    lua_setfield(state, -2, "host");

    lua_newtable(state);
    lua_setfield(state, -2, "_env_reads");

    lua_pushcfunction(state, lua_env);
    lua_setfield(state, -2, "env");
    lua_pushcfunction(state, lua_log);
    lua_setfield(state, -2, "log");

    lua_pop(state, 1);
    return FR_OK;
}

static int config_lua_load(void *state_unused, const char *text, const char *origin,
                           const char *base_dir, fr_registry *registry, const cJSON *document,
                           cJSON **out, fr_error *err) {
    (void) state_unused; (void) registry;

    fr_lua_runtime_shutdown();
    runtime_state = fr_lua_open(64u * 1024u * 1024u, err);
    if (runtime_state == NULL) return FR_ERR;
    if (fr_lua_sandbox_install(runtime_state, base_dir, err) != FR_OK) {
        fr_lua_runtime_shutdown();
        return FR_ERR;
    }
    if (publish_daukle_table(runtime_state, document, err) != FR_OK) return FR_ERR;

    if (fr_lua_run(runtime_state, text, origin, err) != FR_OK) return FR_ERR;

    lua_getglobal(runtime_state, "daukle");
    lua_getfield(runtime_state, -1, "config");
    int status = fr_lua_to_json(runtime_state, -1, out, err);
    lua_pop(runtime_state, 2);
    return status;
}

void fr_lua_runtime_shutdown(void) {
    if (runtime_state == NULL) return;
    fr_lua_close(runtime_state);
    runtime_state = NULL;
}

void fr_lua_set_log_sink(void (*sink)(const char *message)) {
    log_sink = sink;
}

const fr_config_plugin FR_CONFIG_LUA = { "daukle.config/lua", "daukle.lua", 1, config_lua_load, NULL };
