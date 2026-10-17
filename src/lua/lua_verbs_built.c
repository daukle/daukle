#include "lua/lua_verbs_internal.h"

#include "cache/cache.h"
#include "config/config_lua.h"
#include "exec/tool.h"
#include "lua/lua_sandbox.h"
#include "util/error.h"

#include "lauxlib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FR_BUILT_PATH_MAX 1024

static void refuse_outside_a_task(lua_State *state) {
    if (fr_lua_generation_is_running()) {
        luaL_error(state, "daukle.built is not available while generating; "
                          "generation is a pure function of the manifest");
    }
    if (fr_lua_plugin_exec_is_refused()) {
        luaL_error(state, "daukle.built is not available while a plugin chunk is"
                          " loading; call it from a task or publish callback");
    }
    const char *phase = fr_lua_process_free_callback_phase();
    if (phase != NULL) {
        luaL_error(state, "daukle.built is not available while %s; call it from a task or"
                          " publish callback", phase);
    }
    if (!fr_lua_task_dispatch_is_running()) {
        luaL_error(state, "daukle.built is available only inside a task or publish callback");
    }
}

static int has_empty_or_dot_segment(const char *relative) {
    const char *segment = relative;
    for (;;) {
        const char *end = strchr(segment, '/');
        size_t length = end == NULL ? strlen(segment) : (size_t) (end - segment);
        if (length == 0 || (length == 1 && segment[0] == '.')) return 1;
        if (end == NULL) return 0;
        segment = end + 1;
    }
}

/* The lexical half of the containment. ".." is refused anywhere, as
   daukle.write and the sandbox refuse it, rather than only as a whole segment. */
static void refuse_an_unsafe_spelling(lua_State *state, const char *relative, size_t length) {
    for (size_t index = 0; index < length; index++) {
        if ((unsigned char) relative[index] < 0x20) {
            luaL_error(state, "daukle.built's path holds a control character");
        }
    }
    if (strchr(relative, '\\') != NULL) {
        luaL_error(state, "daukle.built's path separates directories with \"/\" and not \"\\\":"
                          " \"%s\"", relative);
    }
    if (relative[0] == '/' || (relative[0] != '\0' && relative[1] == ':')) {
        luaL_error(state, "daukle.built's path is relative to the derived directory, not"
                          " absolute: \"%s\"", relative);
    }
    if (fr_lua_sandbox_climbs_out(relative)) {
        luaL_error(state, "daukle.built's path must stay inside the derived directory: \"%s\"",
                   relative);
    }
    if (has_empty_or_dot_segment(relative)) {
        luaL_error(state, "daukle.built's path has an empty or \".\" segment: \"%s\"", relative);
    }
}

/* The derived directory's canonical form, which the caller frees, so the
   program's own canonical form can be compared against it. */
static char *canonical_derived_directory(lua_State *state, const char *derived) {
    fr_error err;
    char *canonical = NULL;
    if (fr_lua_sandbox_resolve_dir(state, derived, &canonical, &err) != FR_OK) {
        luaL_error(state, "%s", err.message);
    }
    return canonical;
}

/* The canonical form of the program in *out, which the caller frees: a symlink
   or junction the build left in the derived directory is followed here, so the
   containment check sees where it really leads. */
static int canonical_program(lua_State *state, const char *derived, const char *relative,
                             char **out, fr_error *err) {
    char joined[FR_BUILT_PATH_MAX];
    int written = snprintf(joined, sizeof joined, "%s/%s", derived, relative);
    if (written < 0 || (size_t) written >= sizeof joined) {
        fr_error_set(err, "daukle.built's path is too long (%d characters)",
                     (int) strlen(relative));
        return FR_ERR;
    }
    return fr_lua_sandbox_resolve(state, joined, out, err);
}

/* Turns a file the running task's build wrote under its derived directory into
   a tool handle, so daukle.exec can run what a build produced. It widens what a
   plugin may RUN to exactly that directory: no extension is guessed, a batch
   file is refused as daukle.tool refuses one, and a link inside the directory
   is followed before the containment check rather than trusted. No tool report
   line is written, because a program the project built is not a tool daukle
   found or provisioned. */
int fr_lua_verb_built(lua_State *state) {
    refuse_outside_a_task(state);
    size_t length = 0;
    const char *relative = luaL_checklstring(state, 1, &length);
    refuse_an_unsafe_spelling(state, relative, length);

    const char *derived = fr_lua_task_cwd();
    if (derived == NULL) {
        return luaL_error(state, "daukle.built has no derived directory to look in");
    }

    char *directory = canonical_derived_directory(state, derived);
    char candidate[FR_BUILT_PATH_MAX];
    int written = snprintf(candidate, sizeof candidate, "%s/%s", directory, relative);
    if (written < 0 || (size_t) written >= sizeof candidate) {
        free(directory);
        return luaL_error(state, "daukle.built's path is too long (%d characters)", (int) length);
    }
    if (fr_tool_is_batch_file(relative)) {
        free(directory);
        return luaL_error(state, FR_TOOL_BATCH_REFUSAL, relative, candidate);
    }
    if (fr_cache_directory_exists(candidate)) {
        free(directory);
        return luaL_error(state, "daukle.built's \"%s\" is a directory, not a program", relative);
    }
    if (!fr_tool_is_regular_file(candidate)) {
        free(directory);
        return luaL_error(state, "the derived directory %s holds no file \"%s\"", derived,
                          relative);
    }

    fr_error err;
    char *program = NULL;
    if (canonical_program(state, derived, relative, &program, &err) != FR_OK) {
        free(directory);
        return luaL_error(state, "%s", err.message);
    }
    int inside = fr_lua_sandbox_is_within(directory, program);
    free(directory);
    if (!inside) {
        free(program);
        return luaL_error(state, "daukle.built's \"%s\" resolves outside the derived directory",
                          relative);
    }
    int pushed = fr_lua_push_tool_handle(state, program, relative);
    free(program);
    if (!pushed) {
        return luaL_error(state, "the absolute path of \"%s\" is too long for a tool handle",
                          relative);
    }
    return 1;
}
