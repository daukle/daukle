#include "lua/lua_verbs_internal.h"

#include "cache/cache.h"
#include "exec/tool.h"
#include "lua/lua_sandbox.h"
#include "lua/luax.h"
#include "provision/provision.h"
#include "util/error.h"

#include "lauxlib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
int fr_lua_root_path(lua_State *state) {
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
int fr_lua_root_dir(lua_State *state) {
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

int fr_lua_root_tool(lua_State *state) {
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
    if (fr_lua_verbs_is_verbose()) fprintf(stderr, "tool %s (%s)\n", handle->name, handle->path);
    return 1;
}
