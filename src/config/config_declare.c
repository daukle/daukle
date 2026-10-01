#include "config/config_lua_internal.h"

#include "config/config_lua.h"
#include "config/manifest.h"
#include "lua/lua_modules.h"
#include "lua/lua_sandbox.h"
#include "lua/lua_verbs.h"
#include "lua/luax.h"
#include "plugin/plugins.h"
#include "plugin/resolve.h"
#include "project/derived.h"
#include "util/error.h"

#include "cJSON.h"
#include "lauxlib.h"

#include <stdlib.h>
#include <string.h>

#define FR_LUA_MAX_PLUGINS 128

/* @implNote a released slot keeps whatever the declaration that used its index
   left in it, so every site that takes one zeroes it first: otherwise a
   language reusing a task's index inherits that task's part_of and depends_on,
   and fr_lua_runtime_shutdown frees them a second time. */
static fr_lua_plugin_slot plugin_slots[FR_LUA_MAX_PLUGINS];
static size_t plugin_slot_count;
static fr_registry *registering_into;

static fr_lua_plugin_slot *slot_for(const char *capability) {
    for (size_t index = 0; index < plugin_slot_count; index++) {
        if (strcmp(plugin_slots[index].capability, capability) == 0) return &plugin_slots[index];
    }
    return NULL;
}


/* Returns 0 with *out set, or does not return at all: every failure below is
   raised, not reported. Callers still check, because the 0-on-success return
   would otherwise invite a dereference that only longjmp keeps safe. */
static int take_slot(lua_State *state, const char *prefix, const char *field,
                     fr_lua_plugin_slot **out) {
    lua_getfield(state, 1, "name");
    const char *name = lua_tostring(state, -1);
    if (name == NULL) return luaL_error(state, "a %s needs a name", prefix);

    if (plugin_slot_count == FR_LUA_MAX_PLUGINS) {
        return luaL_error(state, "too many declarations in one configuration;"
                          " at most %d are allowed", FR_LUA_MAX_PLUGINS);
    }

    char capability[128];
    int written = snprintf(capability, sizeof capability, "%s%s", prefix, name);
    if (written < 0 || (size_t) written >= sizeof capability) {
        return luaL_error(state, "the plugin name \"%s\" is too long", name);
    }
    lua_pop(state, 1);

    if (slot_for(capability) != NULL) {
        return luaL_error(state, "\"%s\" is declared twice", capability);
    }

    fr_lua_plugin_slot *slot = &plugin_slots[plugin_slot_count];
    memset(slot, 0, sizeof *slot);
    size_t length = strlen(capability) + 1;
    slot->capability = malloc(length);
    if (slot->capability == NULL) return luaL_error(state, "out of memory");
    memcpy(slot->capability, capability, length);
    if (fr_lua_verbs_copy_declared_env(&slot->env, &slot->env_count) != 0) {
        free(slot->capability);
        slot->capability = NULL;
        return luaL_error(state, "out of memory recording env for \"%s\"", capability);
    }

    lua_getfield(state, 1, field);
    if (!lua_isfunction(state, -1)) {
        free(slot->capability);
        return luaL_error(state, "\"%s\" needs a %s function", capability, field);
    }
    slot->callback = luaL_ref(state, LUA_REGISTRYINDEX);
    plugin_slot_count++;
    *out = slot;
    return 0;
}

/* The most recently declared resolver's resolve callback. Unlike
   chunk_declared_resolver and chunk_declared_other below, this is not cleared
   per chunk: it must outlive the chunk that declared it, since
   fr_lua_resolver_call runs later, after the chunk that declared it has
   finished running. It is released by fr_lua_runtime_shutdown and replaced
   whenever a later chunk declares another resolver. */
static int resolver_callback = LUA_NOREF;
/* Copied rather than borrowed for the same reason a slot copies it: the
   declaration is freed when the load ends and a resolver runs long after. */
static char **resolver_env;
static size_t resolver_env_count;
static int chunk_declared_resolver;
static int chunk_declared_other;

/* Whether the chunk fr_lua_plugin_load most recently ran declared a resolver
   and was itself accepted. Unlike resolver_callback, this does not survive a
   chunk that declared one and then failed: fr_lua_plugin_load sets it once
   the chunk's outcome (and any rollback of resolver_callback) is known, so it
   never reports true for a declaration that was rolled back, and never
   reports true for an earlier, unrelated chunk. */
static int resolver_declared;

/* Whether the chunk now running was acquired AS a resolver, which is the only
   way a resolver may be declared. Without it any plugin chunk could call
   daukle.resolver and replace the callback a later, memoized entry resolves
   through, so an unpinned plugin would decide where a pinned resolver's
   plugins come from: the pin a resolver is required to carry governs the
   whole acquisition path only if nothing else can install one. */
static int acquiring_resolver;

void fr_lua_set_acquiring_resolver(int acquiring) {
    acquiring_resolver = acquiring;
}

static int lua_declare_language(lua_State *state) {
    if (fr_lua_verbs_env_declared_exec()) {
        return luaL_error(state, "daukle.exec is available only to a toolchain or publisher plugin");
    }
    if (fr_lua_verbs_env_declared_provision()) {
        return luaL_error(state, "daukle.provision is available only to a toolchain or publisher plugin");
    }
    if (chunk_declared_resolver) {
        return luaL_error(state, "a resolver chunk declares only a resolver");
    }
    chunk_declared_other = 1;
    luaL_checktype(state, 1, LUA_TTABLE);
    fr_lua_plugin_slot *slot = NULL;
    if (take_slot(state, "daukle.language/", "apply", &slot) != 0 || slot == NULL) {
        return luaL_error(state, "a language plugin could not be declared");
    }

    fr_language_plugin plugin = { slot->capability, fr_lua_dispatch_language_apply, slot };
    fr_error err;
    if (fr_registry_add_language(registering_into, &plugin, &err) != FR_OK) {
        return luaL_error(state, "%s", err.message);
    }
    return 0;
}

static int lua_declare_source(lua_State *state) {
    if (fr_lua_verbs_env_declared_exec()) {
        return luaL_error(state, "daukle.exec is available only to a toolchain or publisher plugin");
    }
    if (fr_lua_verbs_env_declared_provision()) {
        return luaL_error(state, "daukle.provision is available only to a toolchain or publisher plugin");
    }
    if (chunk_declared_resolver) {
        return luaL_error(state, "a resolver chunk declares only a resolver");
    }
    chunk_declared_other = 1;
    luaL_checktype(state, 1, LUA_TTABLE);
    fr_lua_plugin_slot *slot = NULL;
    if (take_slot(state, "daukle.source/", "load", &slot) != 0 || slot == NULL) {
        return luaL_error(state, "a source plugin could not be declared");
    }

    fr_source_plugin plugin = { slot->capability, fr_lua_dispatch_source_load, slot };
    fr_error err;
    if (fr_registry_add_source(registering_into, &plugin, &err) != FR_OK) {
        return luaL_error(state, "%s", err.message);
    }
    return 0;
}

static int lua_declare_resolver(lua_State *state) {
    if (!acquiring_resolver) {
        return luaL_error(state, "a resolver may only be declared by a chunk acquired as a"
                                 " resolver, which this one was not");
    }
    if (fr_lua_verbs_env_declared_exec()) {
        return luaL_error(state, "daukle.exec is available only to a toolchain or publisher plugin");
    }
    if (fr_lua_verbs_env_declared_tool()) {
        return luaL_error(state, "a resolver may not start a process, so it may not declare"
                                 " daukle.tool");
    }
    if (fr_lua_verbs_env_declared_provision()) {
        return luaL_error(state, "daukle.provision is available only to a toolchain or publisher plugin");
    }
    luaL_checktype(state, 1, LUA_TTABLE);
    if (chunk_declared_other || chunk_declared_resolver) {
        return luaL_error(state, "a resolver chunk declares only a resolver");
    }

    fr_lua_raw_getfield(state, 1, "resolve");
    if (!lua_isfunction(state, -1)) {
        return luaL_error(state, "a resolver needs a resolve function");
    }
    if (resolver_callback != LUA_NOREF) luaL_unref(state, LUA_REGISTRYINDEX, resolver_callback);
    resolver_callback = luaL_ref(state, LUA_REGISTRYINDEX);
    for (size_t index = 0; index < resolver_env_count; index++) free(resolver_env[index]);
    free(resolver_env);
    resolver_env = NULL;
    resolver_env_count = 0;
    if (fr_lua_verbs_copy_declared_env(&resolver_env, &resolver_env_count) != 0) {
        return luaL_error(state, "out of memory recording env for the resolver");
    }
    chunk_declared_resolver = 1;
    return 0;
}

int fr_lua_resolver_declared(void) {
    return resolver_declared;
}


/* Which toolchains the chunk now running has declared, so that a task naming
   "<toolchain>:" can be checked against them. Reset per chunk in
   fr_lua_plugin_load. The check runs at declaration, so a plugin must declare
   a toolchain before the tasks that name it. */
#define FR_LUA_MAX_CHUNK_TOOLCHAINS 8
static char *chunk_toolchains[FR_LUA_MAX_CHUNK_TOOLCHAINS];
static size_t chunk_toolchain_count;
static size_t chunk_publisher_count;

static void chunk_toolchains_clear(void) {
    for (size_t index = 0; index < chunk_toolchain_count; index++) free(chunk_toolchains[index]);
    chunk_toolchain_count = 0;
}

/* resolver_callback is deliberately excluded here; see its own comment. */
static void chunk_state_clear(void) {
    chunk_toolchains_clear();
    chunk_declared_resolver = 0;
    chunk_declared_other = 0;
    chunk_publisher_count = 0;
}

static int chunk_declares_toolchain(const char *name, size_t length) {
    for (size_t index = 0; index < chunk_toolchain_count; index++) {
        if (strlen(chunk_toolchains[index]) == length
            && strncmp(chunk_toolchains[index], name, length) == 0) {
            return 1;
        }
    }
    return 0;
}

static int lua_declare_toolchain(lua_State *state) {
    if (chunk_declared_resolver) {
        return luaL_error(state, "a resolver chunk declares only a resolver");
    }
    chunk_declared_other = 1;
    luaL_checktype(state, 1, LUA_TTABLE);
    fr_lua_plugin_slot *slot = NULL;
    if (take_slot(state, "daukle.toolchain/", "generate", &slot) != 0 || slot == NULL) {
        return luaL_error(state, "a toolchain plugin could not be declared");
    }

    if (chunk_toolchain_count == FR_LUA_MAX_CHUNK_TOOLCHAINS) {
        return luaL_error(state, "a plugin chunk may declare at most %d toolchains",
                          FR_LUA_MAX_CHUNK_TOOLCHAINS);
    }

    fr_lua_raw_getfield(state, 1, "name");
    const char *toolchain_name = lua_tostring(state, -1);
    if (toolchain_name == NULL) return luaL_error(state, "a toolchain needs a name");
    chunk_toolchains[chunk_toolchain_count] = fr_dup_string(toolchain_name);
    lua_pop(state, 1);
    if (chunk_toolchains[chunk_toolchain_count] == NULL) return luaL_error(state, "out of memory");
    chunk_toolchain_count++;

    fr_toolchain_plugin plugin = { slot->capability, fr_lua_dispatch_toolchain_generate, slot };
    fr_error err;
    if (fr_registry_add_toolchain(registering_into, &plugin, &err) != FR_OK) {
        return luaL_error(state, "%s", err.message);
    }
    return 0;
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

static int lua_declare_task(lua_State *state) {
    if (chunk_declared_resolver) {
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
    if (chunk_toolchain_count == 0) {
        if (chunk_publisher_count > 0) {
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
    chunk_declared_other = 1;
    luaL_checktype(state, 1, LUA_TTABLE);

    fr_lua_raw_getfield(state, 1, "name");
    const char *name = lua_tostring(state, -1);
    if (name == NULL) return luaL_error(state, "a task needs a name");

    const char *colon = strchr(name, ':');
    if (colon != NULL && !chunk_declares_toolchain(name, (size_t) (colon - name))) {
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

    if (slot_for(capability) != NULL) {
        return luaL_error(state, "\"%s\" is declared twice", capability);
    }
    if (plugin_slot_count == FR_LUA_MAX_PLUGINS) {
        return luaL_error(state, "too many declarations in one configuration;"
                          " at most %d are allowed", FR_LUA_MAX_PLUGINS);
    }

    fr_lua_plugin_slot *slot = &plugin_slots[plugin_slot_count];
    memset(slot, 0, sizeof *slot);
    slot->capability = fr_dup_string(capability);
    if (slot->capability == NULL) return luaL_error(state, "out of memory");

    fr_lua_raw_getfield(state, 1, "partOf");
    if (!lua_isnil(state, -1)) {
        if (!lua_isstring(state, -1)) {
            release_task_slot(slot);
            return luaL_error(state, "\"%s\" needs partOf to be a string", capability);
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
    plugin_slot_count++;

    fr_task_plugin plugin = { slot->capability, slot->part_of,
                              (const char *const *) slot->depends_on, slot->depends_on_count,
                              slot->has_run ? fr_lua_dispatch_task_run : NULL, slot };
    fr_error err;
    if (fr_registry_add_task(registering_into, &plugin, &err) != FR_OK) {
        return luaL_error(state, "%s", err.message);
    }
    return 0;
}

static int lua_declare_publisher(lua_State *state) {
    if (chunk_declared_resolver) {
        return luaL_error(state, "a resolver chunk declares only a resolver");
    }
    chunk_declared_other = 1;
    luaL_checktype(state, 1, LUA_TTABLE);

    fr_lua_plugin_slot *slot = NULL;
    if (take_slot(state, "daukle.task/publish:", "publish", &slot) != 0 || slot == NULL) {
        return luaL_error(state, "a publisher could not be declared");
    }

    fr_task_plugin plugin = { slot->capability, NULL, NULL, 0, fr_lua_dispatch_task_run, slot };
    fr_error err;
    if (fr_registry_add_task(registering_into, &plugin, &err) != FR_OK) {
        return luaL_error(state, "%s", err.message);
    }
    chunk_publisher_count++;
    return 0;
}

static int lua_declare_plugin(lua_State *state) {
    (void) state;
    return 0;
}

void fr_lua_verbs_install_registration(lua_State *state) {
    lua_pushcfunction(state, fr_lua_verbs_require_function());
    lua_setfield(state, -2, "require");
    lua_pushcfunction(state, lua_declare_language);
    lua_setfield(state, -2, "language");
    lua_pushcfunction(state, lua_declare_source);
    lua_setfield(state, -2, "source");
    lua_pushcfunction(state, lua_declare_toolchain);
    lua_setfield(state, -2, "toolchain");
    lua_pushcfunction(state, lua_declare_task);
    lua_setfield(state, -2, "task");
    lua_pushcfunction(state, lua_declare_publisher);
    lua_setfield(state, -2, "publisher");
    lua_pushcfunction(state, lua_declare_resolver);
    lua_setfield(state, -2, "resolver");
    lua_pushcfunction(state, lua_declare_plugin);
    lua_setfield(state, -2, "plugin");
}

/* A second registry reference to the same value, under a key nothing else can
   reuse. Holding the reference rather than the slot number is what makes a
   rollback restore the right closure: by the time one is needed the original
   slot may already have been freed and handed to an unrelated luaL_ref. */
static int hold_second_reference(lua_State *state, int reference) {
    if (reference == LUA_NOREF) return LUA_NOREF;
    lua_rawgeti(state, LUA_REGISTRYINDEX, reference);
    return luaL_ref(state, LUA_REGISTRYINDEX);
}

static void release_reference(lua_State *state, int reference) {
    if (reference != LUA_NOREF) luaL_unref(state, LUA_REGISTRYINDEX, reference);
}

/* lua_declare_resolver replaces resolver_callback while the chunk is still
   running, so a chunk that declared one and then failed has to put back what
   it displaced; every other outcome just drops the spare reference. */
static void settle_resolver_callback(lua_State *state, int chunk_succeeded, int backup) {
    if (!chunk_succeeded && chunk_declared_resolver) {
        release_reference(state, resolver_callback);
        resolver_callback = backup;
        return;
    }
    release_reference(state, backup);
}

int fr_lua_declare_chunk_begin(lua_State *state) {
    int backup = hold_second_reference(state, resolver_callback);
    chunk_state_clear();
    return backup;
}

void fr_lua_declare_chunk_end(lua_State *state, int succeeded, int backup) {
    resolver_declared = succeeded ? chunk_declared_resolver : 0;
    settle_resolver_callback(state, succeeded, backup);
    chunk_state_clear();
}

void fr_lua_declare_set_registry(fr_registry *registry) {
    registering_into = registry;
}

fr_registry *fr_lua_registering_registry(void) {
    return registering_into;
}

void fr_lua_declare_reset(void) {
    resolver_callback = LUA_NOREF;
    resolver_declared = 0;
    for (size_t index = 0; index < resolver_env_count; index++) free(resolver_env[index]);
    free(resolver_env);
    resolver_env = NULL;
    resolver_env_count = 0;
    for (size_t index = 0; index < plugin_slot_count; index++) {
        free(plugin_slots[index].capability);
        free(plugin_slots[index].part_of);
        for (size_t d = 0; d < plugin_slots[index].depends_on_count; d++) {
            free(plugin_slots[index].depends_on[d]);
        }
        free(plugin_slots[index].depends_on);
        for (size_t e = 0; e < plugin_slots[index].env_count; e++) {
            free(plugin_slots[index].env[e]);
        }
        free(plugin_slots[index].env);
    }
    plugin_slot_count = 0;
    registering_into = NULL;
}

void fr_lua_declare_install_config_surface(lua_State *state) {
    lua_pushcfunction(state, lua_declare_language);
    lua_setfield(state, -2, "language");
    lua_pushcfunction(state, lua_declare_source);
    lua_setfield(state, -2, "source");
}

void fr_lua_declare_set_resolver_env(void) {
    fr_lua_verbs_set_declared_env((const char *const *) resolver_env, resolver_env_count);
}

int fr_lua_declare_resolver_callback(void) {
    return resolver_callback;
}
