#include "lua/lua_verbs.h"
#include "lua/lua_verbs_internal.h"

#include "lauxlib.h"

#include <stdlib.h>
#include <string.h>

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

size_t fr_lua_verbs_core_credentials(const char **out, size_t limit) {
    size_t written = 0;
    for (size_t index = 0; index < sizeof CORE_CREDENTIALS / sizeof CORE_CREDENTIALS[0]
                           && written < limit; index++) {
        out[written] = CORE_CREDENTIALS[index];
        written++;
    }
    return written;
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

int fr_lua_verb_env(lua_State *state) {
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
