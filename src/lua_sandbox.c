#include "lua_sandbox.h"

#include "error.h"
#include "luax.h"
#include "region.h"

#include "lauxlib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <errno.h>
#endif

#define FR_SANDBOX_BASE_DIR "daukle.base_dir"

static const char *KEPT[] = {
    "assert", "error", "ipairs", "pairs", "next", "select", "tonumber", "tostring",
    "type", "setmetatable", "getmetatable", "string", "table", "math", "_VERSION"
};

static const char *REMOVED[] = {
    "io", "os", "package", "require", "dofile", "loadfile", "load", "debug",
    "collectgarbage", "print", "coroutine", "utf8",
    "pcall", "xpcall", "rawget", "rawset", "rawequal", "rawlen"
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

#ifdef _WIN32

static int finalize_handle_path(HANDLE handle, char **out, fr_error *err) {
    char buffer[4096];
    DWORD length = GetFinalPathNameByHandleA(handle, buffer, sizeof buffer, FILE_NAME_NORMALIZED);
    if (length == 0 || length >= sizeof buffer) {
        fr_error_set(err, "cannot resolve the real path of a file");
        return FR_ERR;
    }
    /* GetFinalPathNameByHandle always answers in the "\\?\"-prefixed extended form,
       which the plain fopen() and snprintf-joined paths elsewhere here do not expect. */
    const char *resolved_start = strncmp(buffer, "\\\\?\\", 4) == 0 ? buffer + 4 : buffer;
    size_t resolved_length = strlen(resolved_start) + 1;
    char *resolved = malloc(resolved_length);
    if (resolved == NULL) {
        fr_error_set(err, "out of memory resolving a path");
        return FR_ERR;
    }
    memcpy(resolved, resolved_start, resolved_length);
    *out = resolved;
    return FR_OK;
}

static int canonical_file_path(const char *path, char **out, fr_error *err) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        fr_error_set(err, "cannot open \"%s\"", path);
        return FR_ERR;
    }
    HANDLE handle = (HANDLE) _get_osfhandle(_fileno(file));
    int status = finalize_handle_path(handle, out, err);
    fclose(file);
    return status;
}

/* fopen cannot open a directory; a directory handle needs the Win32 backup-
   semantics flag instead, which is why base_dir gets its own opener. */
int fr_lua_sandbox_canonical_dir(const char *path, char **out, fr_error *err) {
    HANDLE handle = CreateFileA(path, GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        fr_error_set(err, "cannot open \"%s\"", path);
        return FR_ERR;
    }
    int status = finalize_handle_path(handle, out, err);
    CloseHandle(handle);
    return status;
}

#else

static int canonical_file_path(const char *path, char **out, fr_error *err) {
    char *resolved = realpath(path, NULL);
    if (resolved == NULL) {
        fr_error_set(err, "cannot resolve \"%s\": %s", path, strerror(errno));
        return FR_ERR;
    }
    *out = resolved;
    return FR_OK;
}

int fr_lua_sandbox_canonical_dir(const char *path, char **out, fr_error *err) {
    return canonical_file_path(path, out, err);
}

#endif

/* Requires a whole path component to match so a sibling directory whose name
   merely starts with base_dir's name, such as "projectevil" beside "project",
   cannot pass a bare prefix test. */
static int within_base_dir(const char *canonical_base, const char *canonical_target) {
    size_t base_len = strlen(canonical_base);
    if (strncmp(canonical_base, canonical_target, base_len) != 0) return 0;
    char boundary = canonical_target[base_len];
    return boundary == '/' || boundary == '\\';
}

int fr_lua_sandbox_resolve(lua_State *state, const char *relative, char **out_path, fr_error *err) {
    if (climbs_out(relative)) {
        fr_error_set(err, "\"%s\" is outside the project directory", relative);
        return FR_ERR;
    }

    lua_getfield(state, LUA_REGISTRYINDEX, FR_SANDBOX_BASE_DIR);
    const char *canonical_base = lua_tostring(state, -1);

    char path[512];
    int written = snprintf(path, sizeof path, "%s/%s", canonical_base, relative);
    lua_pop(state, 1);
    if (written < 0 || (size_t) written >= sizeof path) {
        /* The path itself, not just this message, is bounded by a fixed buffer;
           echoing an over-long argument back would just overflow fr_error's own
           bound and truncate this message before "too long" is ever written. */
        fr_error_set(err, "the include path is too long (%d characters)", (int) strlen(relative));
        return FR_ERR;
    }

    char *canonical_target = NULL;
    if (canonical_file_path(path, &canonical_target, err) != FR_OK) {
        return FR_ERR;
    }
    /* A symlink or an NTFS junction inside base_dir can point anywhere on disk;
       climbs_out only rejects the literal argument, so the real destination has
       to be resolved and re-checked, not just the lexical request. */
    if (!within_base_dir(canonical_base, canonical_target)) {
        free(canonical_target);
        fr_error_set(err, "\"%s\" is outside the project directory", relative);
        return FR_ERR;
    }

    *out_path = canonical_target;
    return FR_OK;
}

static int sandbox_include(lua_State *state) {
    const char *relative_path = luaL_checkstring(state, 1);

    fr_error err;
    char *resolved = NULL;
    if (fr_lua_sandbox_resolve(state, relative_path, &resolved, &err) != FR_OK) {
        return luaL_error(state, "%s", err.message);
    }

    char *text = NULL;
    int status = fr_file_read_text(resolved, &text, &err);
    free(resolved);
    if (status != FR_OK) {
        return luaL_error(state, "%s", err.message);
    }

    int load_status = fr_lua_load_named(state, text, strlen(text), relative_path);
    free(text);
    if (load_status != LUA_OK) return lua_error(state);

    lua_call(state, 0, 0);
    return 0;
}

/* fr_lua_sandbox_install is a plain C entry point: no lua_pcall frame is active
   yet, so nothing here may touch the Lua C API directly before one is
   installed (the same reasoning, and the same abort-on-unprotected-OOM risk,
   as config_lua.c's protected_* functions). The base directory travels through
   this file static rather than a lua_pushcclosure upvalue, since creating a
   closure with an upvalue allocates on the push itself, before any pcall
   exists to catch that allocation failing. */
static const char *pending_base_dir;

static int protected_install(lua_State *state) {
    const char *canonical_base = pending_base_dir;

    lua_pushstring(state, canonical_base);
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
    /* __metatable makes setmetatable(_G, ...) raise instead of silently disarming
       __index: without it a script can strip the removed-name diagnostic for free. */
    lua_pushstring(state, "the daukle sandbox");
    lua_setfield(state, -2, "__metatable");
    lua_setmetatable(state, -2);

    lua_newtable(state);
    lua_pushcfunction(state, sandbox_include);
    lua_setfield(state, -2, "include");
    lua_setfield(state, -2, "daukle");

    lua_pushvalue(state, -1);
    lua_setfield(state, -2, "_G");
    lua_rawseti(state, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);

    return 0;
}

int fr_lua_sandbox_install(lua_State *state, const char *base_dir, char **out_canonical,
                           fr_error *err) {
    char *canonical_base = NULL;
    if (fr_lua_sandbox_canonical_dir(base_dir, &canonical_base, err) != FR_OK) return FR_ERR;

    pending_base_dir = canonical_base;
    lua_pushcfunction(state, protected_install);
    int status = lua_pcall(state, 0, 0, 0);
    pending_base_dir = NULL;

    if (status != LUA_OK) {
        free(canonical_base);
        fr_error_set(err, "%s", fr_lua_error_text(state));
        lua_pop(state, 1);
        return FR_ERR;
    }
    if (out_canonical != NULL) {
        *out_canonical = canonical_base;
    } else {
        free(canonical_base);
    }
    return FR_OK;
}
