#include "config/config_lua_internal.h"

#include "config/config_lua.h"
#include "lua/lua_verbs.h"
#include "lua/luax.h"
#include "plugin/plugins.h"

#include "lauxlib.h"

#include <stdlib.h>
#include <string.h>

/* Borrowed from the declaration fr_plugins_read_declaration filled, which the
   loader keeps alive for the whole of fr_lua_plugin_load and frees after. It
   is armed and disarmed around that call exactly as the environment allowlist
   is, so a path that is not a plugin chunk sees an empty list rather than a
   stale one. */
static const fr_plugin_requirement *chunk_requires;
static size_t chunk_requires_count;

void fr_lua_declare_set_required_aliases(const fr_plugin_requirement *requires, size_t count) {
    chunk_requires = requires;
    chunk_requires_count = count;
}

/* Copies the toolchain-name prefix of a colon-qualified task name into a
   fixed buffer. luaL_error's format string is Lua's own minimal printf
   (lua_pushvfstring), which has no precision specifier: "%.*s" raises
   "invalid option '%.'" at runtime instead of truncating, so the substring
   has to be built by hand before it can be interpolated with a plain "%s". */
static void copy_toolchain_prefix(const char *name, const char *colon, char *out, size_t out_size) {
    size_t length = (size_t) (colon - name);
    if (length >= out_size) length = out_size - 1;
    memcpy(out, name, length);
    out[length] = '\0';
}

static int chunk_requires_alias(const char *name, size_t length) {
    for (size_t index = 0; index < chunk_requires_count; index++) {
        const char *alias = chunk_requires[index].alias;
        if (alias != NULL && strlen(alias) == length && strncmp(alias, name, length) == 0) {
            return 1;
        }
    }
    return 0;
}

/* An edge naming "<owner>:<task>" points at a task this chunk did not declare,
   so the owner must be one this plugin declared a dependency on. A bare name
   carries no owner at all and is left alone: it is the aggregator mechanism of
   the task model's section 4, where a toolchain joins a "build" whose provider
   it must not have to name. Fills owner_out with the offending prefix and
   returns 0 when the edge is foreign; the caller raises, because it has a task
   slot to release first. */
static int edge_owner_is_declared(const char *edge, char *owner_out, size_t owner_size) {
    const char *colon = strchr(edge, ':');
    if (colon == NULL) return 1;
    size_t owner = (size_t) (colon - edge);
    if (fr_lua_declare_chunk_declares_toolchain(edge, owner) || chunk_requires_alias(edge, owner)) return 1;
    copy_toolchain_prefix(edge, colon, owner_out, owner_size);
    return 0;
}

/* Frees everything a task slot owns and zeroes it. This is needed only for a
   raise that happens before plugin_slot_count is incremented: the shutdown
   loop walks slots by that count, so a slot not yet counted has no other way
   to be released. Every check in lua_declare_task between slot->capability's
   allocation and that increment stands in for what would otherwise be a
   luaL_check* call, specifically so it can call this before raising instead
   of raising out from under an unfreed slot. */
static void release_task_slot(fr_lua_plugin_slot *slot) {
    free(slot->capability);
    free(slot->part_of);
    for (size_t index = 0; index < slot->env_count; index++) free(slot->env[index]);
    free(slot->env);
    for (size_t index = 0; index < slot->depends_on_count; index++) free(slot->depends_on[index]);
    free(slot->depends_on);
    memset(slot, 0, sizeof *slot);
}

int fr_lua_declare_task(lua_State *state) {
    if (fr_lua_declare_chunk_declared_resolver()) {
        return luaL_error(state, "a resolver chunk declares only a resolver");
    }
    /* A task keeps exec and provision by inheriting them from the toolchain
       that owns it, which holds only where there IS one: a chunk declaring no
       toolchain has nothing to pass the capability down from, and a colon-free
       name names no owner either.
       Both messages name the POSITION rather than the chunk. At this point a
       toolchain declared further down the same chunk has not run yet and is
       indistinguishable from one that never comes, so "only to a toolchain
       plugin" would be false for a plugin that is one and simply declares it
       below. Naming the order is true in both cases and is also the fix. */
    if (fr_lua_declare_chunk_toolchain_count() == 0) {
        if (fr_lua_declare_chunk_publisher_count() > 0) {
            return luaL_error(state, "a publisher declares publishers, not tasks: its publish"
                                     " function is the task, and daukle.exec is available there");
        }
        if (fr_lua_verbs_env_declared_exec()) {
            return luaL_error(state, "daukle.exec is available only to a task whose toolchain is"
                                     " declared above it; this chunk declares none yet");
        }
        if (fr_lua_verbs_env_declared_provision()) {
            return luaL_error(state, "daukle.provision is available only to a task whose toolchain"
                                     " is declared above it; this chunk declares none yet");
        }
    }
    fr_lua_declare_mark_chunk_other();
    luaL_checktype(state, 1, LUA_TTABLE);

    fr_lua_raw_getfield(state, 1, "name");
    const char *name = lua_tostring(state, -1);
    if (name == NULL) return luaL_error(state, "a task needs a name");

    const char *colon = strchr(name, ':');
    if (colon != NULL && !fr_lua_declare_chunk_declares_toolchain(name, (size_t) (colon - name))) {
        char toolchain_name[64];
        copy_toolchain_prefix(name, colon, toolchain_name, sizeof toolchain_name);
        return luaL_error(state, "task \"%s\" cannot be declared here: no toolchain \"%s\" is"
                                 " declared above this point in this plugin", name, toolchain_name);
    }

    char capability[128];
    int written = snprintf(capability, sizeof capability, "daukle.task/%s", name);
    if (written < 0 || (size_t) written >= sizeof capability) {
        return luaL_error(state, "the task name \"%s\" is too long", name);
    }
    lua_pop(state, 1);

    if (fr_lua_declare_slot_for(capability) != NULL) {
        return luaL_error(state, "\"%s\" is declared twice", capability);
    }
    fr_lua_plugin_slot *slot = fr_lua_declare_next_slot();
    if (slot == NULL) {
        return luaL_error(state, "too many declarations in one configuration;"
                          " at most %d are allowed", FR_LUA_MAX_PLUGINS);
    }

    memset(slot, 0, sizeof *slot);
    slot->capability = fr_dup_string(capability);
    if (slot->capability == NULL) return luaL_error(state, "out of memory");
    /* Every other declaration copies the chunk's env here, and this one did not,
       so a task ran with an EMPTY allowlist and daukle.exec scrubbed every core
       credential from the child whatever the plugin declared. A task is the only
       callback allowed to exec, so the allowlist was unreachable from the one
       place it governs. D-32. */
    if (fr_lua_verbs_copy_declared_env(&slot->env, &slot->env_count) != 0) {
        release_task_slot(slot);
        return luaL_error(state, "out of memory recording env for \"%s\"", capability);
    }

    fr_lua_raw_getfield(state, 1, "partOf");
    if (!lua_isnil(state, -1)) {
        if (!lua_isstring(state, -1)) {
            release_task_slot(slot);
            return luaL_error(state, "\"%s\" needs partOf to be a string", capability);
        }
        char owner[64];
        if (!edge_owner_is_declared(lua_tostring(state, -1), owner, sizeof owner)) {
            char edge[128];
            snprintf(edge, sizeof edge, "%s", lua_tostring(state, -1));
            release_task_slot(slot);
            return luaL_error(state, "\"%s\" is part of \"%s\", which belongs to \"%s\": a plugin"
                                     " may name a task of a toolchain it declares or of a plugin"
                                     " it requires, and this one requires no \"%s\"",
                              capability, edge, owner, owner);
        }
        slot->part_of = fr_dup_string(lua_tostring(state, -1));
        if (slot->part_of == NULL) {
            release_task_slot(slot);
            return luaL_error(state, "out of memory");
        }
    }
    lua_pop(state, 1);

    fr_lua_raw_getfield(state, 1, "dependsOn");
    if (!lua_isnil(state, -1)) {
        if (!lua_istable(state, -1)) {
            release_task_slot(slot);
            return luaL_error(state, "\"%s\" needs dependsOn to be a table", capability);
        }
        lua_Integer length = (lua_Integer) lua_rawlen(state, -1);
        if (length > 0) {
            slot->depends_on = calloc((size_t) length, sizeof *slot->depends_on);
            if (slot->depends_on == NULL) {
                release_task_slot(slot);
                return luaL_error(state, "out of memory");
            }
        }
        for (lua_Integer index = 1; index <= length; index++) {
            lua_rawgeti(state, -1, index);
            if (!lua_isstring(state, -1)) {
                release_task_slot(slot);
                return luaL_error(state, "\"%s\" needs dependsOn to contain only strings", capability);
            }
            char owner[64];
            if (!edge_owner_is_declared(lua_tostring(state, -1), owner, sizeof owner)) {
                char edge[128];
                snprintf(edge, sizeof edge, "%s", lua_tostring(state, -1));
                release_task_slot(slot);
                return luaL_error(state, "\"%s\" depends on \"%s\", which belongs to \"%s\": a"
                                         " plugin may name a task of a toolchain it declares or of"
                                         " a plugin it requires, and this one requires no \"%s\"",
                                  capability, edge, owner, owner);
            }
            slot->depends_on[index - 1] = fr_dup_string(lua_tostring(state, -1));
            if (slot->depends_on[index - 1] == NULL) {
                release_task_slot(slot);
                return luaL_error(state, "out of memory");
            }
            slot->depends_on_count = (size_t) index;
            lua_pop(state, 1);
        }
    }
    lua_pop(state, 1);

    fr_lua_raw_getfield(state, 1, "run");
    if (!lua_isnil(state, -1)) {
        if (!lua_isfunction(state, -1)) {
            release_task_slot(slot);
            return luaL_error(state, "\"%s\" needs run to be a function", capability);
        }
        slot->callback = luaL_ref(state, LUA_REGISTRYINDEX);
        slot->has_run = 1;
    } else {
        lua_pop(state, 1);
    }
    fr_lua_declare_count_slot();

    fr_task_plugin plugin = { slot->capability, slot->part_of,
                              (const char *const *) slot->depends_on, slot->depends_on_count,
                              slot->has_run ? fr_lua_dispatch_task_run : NULL, slot };
    fr_error err;
    if (fr_registry_add_task(fr_lua_registering_registry(), &plugin, &err) != FR_OK) {
        return luaL_error(state, "%s", err.message);
    }
    return 0;
}

int fr_lua_declare_publisher(lua_State *state) {
    if (fr_lua_declare_chunk_declared_resolver()) {
        return luaL_error(state, "a resolver chunk declares only a resolver");
    }
    fr_lua_declare_mark_chunk_other();
    luaL_checktype(state, 1, LUA_TTABLE);

    fr_lua_plugin_slot *slot = NULL;
    if (fr_lua_declare_take_slot(state, "daukle.task/publish:", "publish", &slot) != 0 || slot == NULL) {
        return luaL_error(state, "a publisher could not be declared");
    }

    fr_task_plugin plugin = { slot->capability, NULL, NULL, 0, fr_lua_dispatch_task_run, slot };
    fr_error err;
    if (fr_registry_add_task(fr_lua_registering_registry(), &plugin, &err) != FR_OK) {
        return luaL_error(state, "%s", err.message);
    }
    fr_lua_declare_count_publisher();
    return 0;
}

int fr_lua_declare_plugin(lua_State *state) {
    (void) state;
    return 0;
}
