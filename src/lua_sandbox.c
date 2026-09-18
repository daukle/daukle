#include "lua_sandbox.h"

#include "error.h"
#include "luax.h"
#include "region.h"

#include "lauxlib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FR_SANDBOX_BASE_DIR "daukle.base_dir"

static const char *KEPT[] = {
    "assert", "error", "ipairs", "pairs", "next", "select", "tonumber", "tostring",
    "type", "pcall", "xpcall", "setmetatable", "getmetatable", "rawget", "rawset",
    "rawequal", "rawlen", "string", "table", "math", "_VERSION"
};

static const char *REMOVED[] = {
    "io", "os", "package", "require", "dofile", "loadfile", "load", "debug",
    "collectgarbage", "print"
};

static int removed_name(lua_State *state) {
    const char *name = lua_tostring(state, 2);
    for (size_t index = 0; index < sizeof REMOVED / sizeof REMOVED[0]; index++) {
        if (name != NULL && strcmp(REMOVED[index], name) == 0) {
            return luaL_error(state, "%s is not available in a daukle configuration", name);
        }
    }
    lua_pushnil(state);
    return 1;
}

/* Rejects any path containing "..", which also rejects a legitimate file named
   "a..b.lua". A simple, auditable rule beats a clever normalisation here. */
static int climbs_out(const char *relative_path) {
    if (relative_path[0] == '/' || relative_path[0] == '\\') return 1;
    if (relative_path[0] != '\0' && relative_path[1] == ':') return 1;
    for (const char *cursor = relative_path; *cursor != '\0'; cursor++) {
        if (cursor[0] == '.' && cursor[1] == '.') return 1;
    }
    return 0;
}

static int sandbox_include(lua_State *state) {
    const char *relative_path = luaL_checkstring(state, 1);
    if (climbs_out(relative_path)) {
        return luaL_error(state, "\"%s\" is outside the project directory", relative_path);
    }

    lua_getfield(state, LUA_REGISTRYINDEX, FR_SANDBOX_BASE_DIR);
    const char *base_dir = lua_tostring(state, -1);

    char path[512];
    int written = snprintf(path, sizeof path, "%s/%s", base_dir, relative_path);
    lua_pop(state, 1);
    if (written < 0 || (size_t) written >= sizeof path) {
        return luaL_error(state, "the include path \"%s\" is too long", relative_path);
    }

    fr_error err;
    char *text = NULL;
    if (fr_file_read_text(path, &text, &err) != FR_OK) {
        return luaL_error(state, "%s", err.message);
    }

    int status = fr_lua_load_named(state, text, strlen(text), relative_path);
    free(text);
    if (status != LUA_OK) return lua_error(state);

    lua_call(state, 0, 0);
    return 0;
}

int fr_lua_sandbox_install(lua_State *state, const char *base_dir, fr_error *err) {
    lua_pushstring(state, base_dir);
    lua_setfield(state, LUA_REGISTRYINDEX, FR_SANDBOX_BASE_DIR);

    /* Read every kept name out of the CURRENT globals before installing the
       replacement: swap the table first and this loop reads back nils. */
    lua_newtable(state);
    for (size_t index = 0; index < sizeof KEPT / sizeof KEPT[0]; index++) {
        lua_getglobal(state, KEPT[index]);
        lua_setfield(state, -2, KEPT[index]);
    }

    lua_newtable(state);
    lua_pushcfunction(state, removed_name);
    lua_setfield(state, -2, "__index");
    lua_setmetatable(state, -2);

    lua_newtable(state);
    lua_pushcfunction(state, sandbox_include);
    lua_setfield(state, -2, "include");
    lua_setfield(state, -2, "daukle");

    lua_pushvalue(state, -1);
    lua_setfield(state, -2, "_G");
    lua_rawseti(state, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);

    (void) err;
    return FR_OK;
}
