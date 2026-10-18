#include "lua/lua_verbs_internal.h"

#include "cache/cache.h"
#include "config/config_lua.h"
#include "lua/lua_sandbox.h"
#include "project/region.h"

#include "lauxlib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A plugin may write one file, into the derived directory it already owns, and
   the path may descend into a subdirectory of it, which is created. The same
   containment rules as the generate writer apply: no climbing out, no
   backslash, no control character.

   It exists because core already writes plugin-authored files there from a
   generate hook, and generate cannot run a tool, so a list that must be
   enumerated could never be written by the one hook able to write it. D-80. */
int fr_lua_verb_write(lua_State *state) {
    if (fr_lua_generation_is_running()) {
        return luaL_error(state, "daukle.write is not available while generating; return the"
                                 " file from the generate hook instead");
    }
    if (fr_lua_plugin_exec_is_refused()) {
        return luaL_error(state, "daukle.write is not available while a plugin chunk is"
                                 " loading; call it from a task, publish or migrator callback");
    }
    luaL_checktype(state, 1, LUA_TTABLE);

    lua_getfield(state, 1, "path");
    const char *name = lua_tostring(state, -1);
    if (name == NULL) {
        return luaL_error(state, "daukle.write needs a \"path\" string");
    }
    if (name[0] == '\0') {
        return luaL_error(state, "daukle.write's \"path\" may not be empty");
    }
    if (fr_lua_sandbox_climbs_out(name)) {
        return luaL_error(state, "daukle.write's \"path\" must stay inside the derived"
                                 " directory: \"%s\"", name);
    }
    if (strchr(name, '\\') != NULL) {
        return luaL_error(state, "daukle.write's \"path\" separates directories with \"/\" and"
                                 " not \"\\\": \"%s\"", name);
    }
    for (const char *cursor = name; *cursor != '\0'; cursor++) {
        if ((unsigned char) *cursor < 0x20) {
            return luaL_error(state, "daukle.write's \"path\" holds a control character");
        }
    }
    if (strcmp(name, ".") == 0 || name[strlen(name) - 1] == '/') {
        return luaL_error(state, "daukle.write's \"path\" must name a file, not a directory: \"%s\"", name);
    }

    lua_getfield(state, 1, "text");
    const char *text = lua_tostring(state, -1);
    if (text == NULL) {
        return luaL_error(state, "daukle.write needs a \"text\" string");
    }

    lua_getfield(state, 1, "committed");
    int committed = lua_toboolean(state, -1);
    if (!lua_isnil(state, -1) && !lua_isboolean(state, -1)) {
        return luaL_error(state, "daukle.write's \"committed\" must be true or false");
    }
    lua_pop(state, 1);

    /* A committed file is one a clone must carry, so writing one is part of
       resolving rather than of building: an ordinary build must not modify a
       tracked file behind the user's back. Same gate as daukle.pin, and for
       the same reason, which is why it names the same flag. D-85. */
    if (committed && !fr_lua_verbs_is_resolving()) {
        return luaL_error(state, "daukle.write{ committed = true } is available only in a resolve"
                                 " run: pass --resolve. An ordinary build does not rewrite a file"
                                 " the project has committed");
    }

    const char *directory = committed ? fr_lua_task_generated_dir() : fr_lua_task_cwd();
    if (directory == NULL) {
        return luaL_error(state, "daukle.write has no %s directory to write into",
                          committed ? "generated" : "derived");
    }

    fr_error err;
    char *resolved = NULL;
    if (committed) {
        /* The generated directory is created here rather than at sync, because
           a project that never resolves should never grow one. Resolving a
           path canonicalises it, which fails on a directory that is not there
           yet, so it is made first and the containment check still runs. */
        char *base = NULL;
        if (fr_lua_sandbox_base_dir(state, &base, &err) != FR_OK) {
            return luaL_error(state, "%s", err.message);
        }
        char tree[512];
        int built = snprintf(tree, sizeof tree, "%s/%s", base, directory);
        free(base);
        if (built < 0 || (size_t) built >= sizeof tree) {
            return luaL_error(state, "the generated directory path is too long");
        }
        fr_cache_make_directories(tree);
    }

    /* The task cwd is relative to the project root while the tool this file is
       written for runs WITH the derived directory as its cwd, so a relative
       answer would be wrong for the one caller it exists for. Resolving the
       directory also re-checks that it is still inside the project. */
    if (fr_lua_sandbox_resolve_dir(state, directory, &resolved, &err) != FR_OK) {
        return luaL_error(state, "%s", err.message);
    }

    char path[512];
    int written = snprintf(path, sizeof path, "%s/%s", resolved, name);
    free(resolved);
    if (written < 0 || (size_t) written >= sizeof path) {
        return luaL_error(state, "the path is too long (%d characters)", (int) strlen(name));
    }

    char *last_separator = strrchr(path, '/');
    if (last_separator != NULL) {
        *last_separator = '\0';
        fr_cache_make_directories(path);
        /* Silent by design (see fr_cache_make_directories), so the outcome is
           asked for here: unchecked, a directory that could not be made
           surfaces as a write error naming the FILE. */
        if (!fr_cache_directory_exists(path)) {
            return luaL_error(state, "could not create the directory %s", path);
        }
        *last_separator = '/';
    }

    if (fr_file_write_text(path, text, &err) != FR_OK) {
        return luaL_error(state, "%s", err.message);
    }

    lua_pushstring(state, path);
    return 1;
}
