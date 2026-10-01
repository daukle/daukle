#include "lua/lua_modules.h"

#include "archive/tar.h"
#include "lua/lua_sandbox.h"
#include "lua/lua_verbs.h"
#include "lua/luax.h"
#include "plugin/plugins.h"
#include "util/error.h"

#include "lauxlib.h"

#include <string.h>

/* Where a module runs: the entry chunk's own environment table, held under the
   address of this file static so no plugin can name the key. */
static const char module_env_key = 0;
static fr_plugin_source *loading_source;
static fr_plugin_deps *loading_deps;
/* Not a verb and not declared in "uses": reading a member of the artifact the
   manifest already pinned grants nothing the digest does not already cover.
   Reaching ANOTHER plugin's code is the edge that has to be declared, and the
   alias before the colon is that declaration. */
#define FR_PLUGIN_MODULE_LIMIT 64

typedef struct {
    char name[FR_TAR_MAX_NAME + 1];
    char owner_label[FR_PLUGIN_MAX_ALIAS + 1];  /* empty when owner is NULL */
    const fr_plugin_deps *owner;   /* NULL for a module of the plugin whose chunk is running */
    int value;   /* LUA_NOREF until the module has returned */
    int running;
} module_slot;

/* The memo is per load, so two dependents requiring the same member of the same
   artifact get their own instance each: they never share a memo, so no key can
   let them share an entry. Within one load the artifact is the key beside the
   member name, which keeps java:lib/util apart from foo:lib/util and still
   hands one dependent one instance however many paths reach it, a nested
   require inside a library module included. */
static module_slot module_slots[FR_PLUGIN_MODULE_LIMIT];
static size_t module_slot_count;

/* While a module reached across a plugin boundary runs, the artifact it came
   from is the one a require inside it resolves against: a module's requires
   belong to the plugin that owns the module, never to whoever asked for it.
   Bounded explicitly rather than by how deep the C recursion happens to go. */
typedef struct {
    fr_plugin_deps *deps;
    const char *label;      /* this frame's own artifact, borrowed from the acquisition */
    const char *dependent;  /* who required it, for the library environment's message */
} owner_frame;

static owner_frame owner_stack[FR_PLUGIN_DEPS_MAX_DEPTH];
static size_t owner_stack_depth;

static const owner_frame *current_owner(void) {
    return owner_stack_depth == 0 ? NULL : &owner_stack[owner_stack_depth - 1];
}

static void modules_clear(lua_State *state) {
    for (size_t index = 0; index < module_slot_count; index++) {
        if (module_slots[index].value != LUA_NOREF) {
            luaL_unref(state, LUA_REGISTRYINDEX, module_slots[index].value);
        }
    }
    module_slot_count = 0;
}

static module_slot *module_slot_for(const fr_plugin_deps *owner, const char *name) {
    for (size_t index = 0; index < module_slot_count; index++) {
        module_slot *slot = &module_slots[index];
        if (slot->owner == owner && strcmp(slot->name, name) == 0) return slot;
    }
    return NULL;
}

/* Names the chain rather than only the module that closed it, in the manner of
   tasks.c's report_cycle: the slots still running ARE the chain, in order. */
static int report_module_cycle(lua_State *state, const char *name) {
    char trail[400];
    size_t filled = 0;
    trail[0] = '\0';
    for (size_t index = 0; index < module_slot_count; index++) {
        const module_slot *slot = &module_slots[index];
        if (!slot->running) continue;
        int written = snprintf(trail + filled, sizeof trail - filled, "%s%s%s%s",
                               filled == 0 ? "" : " -> ", slot->owner_label,
                               slot->owner != NULL ? ":" : "", slot->name);
        if (written < 0 || (size_t) written >= sizeof trail - filled) break;
        filled += (size_t) written;
    }
    return luaL_error(state, "daukle.require(\"%s\") is a cycle: %s -> %s", name, trail, name);
}

/* Raises rather than reporting: the limit is the running plugin's own doing and
   there is no require left to answer past it. */
static module_slot *claim_module_slot(lua_State *state, const fr_plugin_deps *owner,
                                      const char *owner_label, const char *name) {
    if (module_slot_count == FR_PLUGIN_MODULE_LIMIT) {
        luaL_error(state, "a load may require at most %d modules across every plugin and"
                          " dependency in it", FR_PLUGIN_MODULE_LIMIT);
    }

    module_slot *slot = &module_slots[module_slot_count++];
    memset(slot, 0, sizeof *slot);
    snprintf(slot->name, sizeof slot->name, "%s", name);
    snprintf(slot->owner_label, sizeof slot->owner_label, "%s",
             owner_label != NULL ? owner_label : "");
    slot->owner = owner;
    slot->value = LUA_NOREF;
    slot->running = 1;
    return slot;
}

typedef struct {
    module_slot *slot;
    const char *name;
    const char *chunk_name;
    const char *text;
    size_t length;
    const char *dependent;   /* who the library environment names as asking */
    const char *member;
} module_call;

/* Expects the environment the module runs in on the stack top, and leaves what
   the module returned there instead. */
static int run_module(lua_State *state, module_slot *slot, const char *name,
                      const char *chunk_name, const char *text, size_t length) {
    if (fr_lua_load_named(state, text, length, chunk_name) != LUA_OK) return lua_error(state);
    lua_insert(state, -2);

    if (lua_setupvalue(state, -2, 1) == NULL) {
        /* Never reached for a chunk compiled from source, and checked anyway:
           failing here silently would run the module against the globals rather
           than against what the plugin declared. */
        return luaL_error(state, "daukle.require(\"%s\") could not be given its environment", name);
    }

    lua_call(state, 0, 1);

    /* luaL_ref stores nil perfectly well, so a module that returns nothing is
       remembered as having returned nothing rather than run again. It pops the
       value, so it is pushed back for the caller. */
    lua_pushvalue(state, -1);
    slot->value = luaL_ref(state, LUA_REGISTRYINDEX);
    slot->running = 0;
    return 1;
}

/* A name with no alias: the running plugin's own module, or, inside a module
   reached across a boundary, one of that module's own siblings. */
static int require_own_module(lua_State *state, const char *name) {
    const owner_frame *frame = current_owner();

    /* One condition rather than two: a source is held only while a plugin chunk
       runs, so this covers both a single-file plugin and a call from outside a
       load, which the "daukle.plugin must be the first call" refusal already
       reaches first. */
    if (frame == NULL && loading_source == NULL) {
        return luaL_error(state, "daukle.require(\"%s\"): this plugin has no modules", name);
    }

    const fr_plugin_deps *owner = frame != NULL ? frame->deps : NULL;
    module_slot *slot = module_slot_for(owner, name);
    if (slot != NULL) {
        if (slot->running) return report_module_cycle(state, name);
        lua_rawgeti(state, LUA_REGISTRYINDEX, slot->value);
        return 1;
    }

    const char *text = NULL;
    size_t length = 0;
    fr_error err;
    int found = frame != NULL
        ? fr_plugin_deps_own_member(frame->deps, name, &text, &length, &err)
        : fr_plugin_source_member(loading_source, name, &text, &length, &err);
    if (found != FR_OK) return luaL_error(state, "%s", err.message);

    slot = claim_module_slot(state, owner, frame != NULL ? frame->label : NULL, name);

    char chunk_name[FR_TAR_MAX_NAME + 8];
    snprintf(chunk_name, sizeof chunk_name, "@%s.lua", name);

    if (frame != NULL) {
        if (fr_lua_verbs_push_library_env(state, frame->dependent, name, &err) != FR_OK) {
            return luaL_error(state, "%s", err.message);
        }
    } else {
        lua_rawgetp(state, LUA_REGISTRYINDEX, &module_env_key);
    }
    return run_module(state, slot, name, chunk_name, text, length);
}

/* Everything the cross-plugin path does once the owner frame is pushed, so that
   the frame can be popped whether it returns or raises. Reached only through
   lua_pcall, and its one argument is the call description as light userdata,
   which needs no allocation to push and no file static to nest.

   @implNote popping here is the mechanism; fr_lua_plugin_load's
   owner_stack_depth resets are a backstop, and NO test can tell the two apart.
   A leaked frame routes the dependent's own bare requires through the provider,
   where fr_plugin_deps_own_member does not consult exports, but BASE carries no
   pcall, so nothing can catch a raise mid-load and a leak is observable only
   across a load boundary, which the resets already own. Do not read a green
   suite as permission to drop the pop: it will stay green until the sandbox can
   catch an error, and the hole opens the moment it can. */
static int protected_run_member(lua_State *state) {
    const module_call *call = lua_touserdata(state, 1);
    lua_settop(state, 0);

    fr_error err;
    if (fr_lua_verbs_push_library_env(state, call->dependent, call->member, &err) != FR_OK) {
        return luaL_error(state, "%s", err.message);
    }
    return run_module(state, call->slot, call->name, call->chunk_name, call->text, call->length);
}

static int require_across_plugins(lua_State *state, const char *name, const char *colon) {
    const owner_frame *frame = current_owner();
    fr_plugin_deps *from = frame != NULL ? frame->deps : loading_deps;
    const char *from_label = frame != NULL ? frame->label : fr_plugin_deps_label(loading_deps);

    /* Refused as over-long rather than truncated: a truncated alias is reported
       undeclared under a spelling the author never wrote. */
    size_t alias_length = (size_t) (colon - name);
    if (alias_length > FR_PLUGIN_MAX_ALIAS) {
        return luaL_error(state, "daukle.require(\"%s\"): the plugin before the \":\" is longer"
                                 " than %d bytes", name, FR_PLUGIN_MAX_ALIAS);
    }
    char alias[FR_PLUGIN_MAX_ALIAS + 1];
    memcpy(alias, name, alias_length);
    alias[alias_length] = '\0';
    const char *member = colon + 1;

    fr_error err;
    if (fr_plugin_module_name_check(member, &err) != FR_OK) {
        return luaL_error(state, "%s", err.message);
    }

    const char *text = NULL;
    size_t length = 0;
    fr_plugin_deps *owner = NULL;
    const char *owner_label = NULL;
    if (fr_plugin_deps_member(from, alias, member, &text, &length, &owner, &owner_label, &err)
        != FR_OK) {
        return luaL_error(state, "%s", err.message);
    }

    module_slot *slot = module_slot_for(owner, member);
    if (slot != NULL) {
        if (slot->running) return report_module_cycle(state, name);
        lua_rawgeti(state, LUA_REGISTRYINDEX, slot->value);
        return 1;
    }

    if (owner_stack_depth == FR_PLUGIN_DEPS_MAX_DEPTH) {
        return luaL_error(state, "daukle.require(\"%s\"): modules reach across more than %d plugin"
                                 " boundaries", name, FR_PLUGIN_DEPS_MAX_DEPTH);
    }

    slot = claim_module_slot(state, owner, owner_label, member);

    char chunk_name[FR_TAR_MAX_NAME * 2];
    snprintf(chunk_name, sizeof chunk_name, "@%s:%s.lua", owner_label, member);

    module_call call;
    call.slot = slot;
    call.name = name;
    call.chunk_name = chunk_name;
    call.text = text;
    call.length = length;
    call.dependent = from_label != NULL ? from_label : "this plugin";
    call.member = member;

    lua_pushcfunction(state, protected_run_member);
    lua_pushlightuserdata(state, &call);
    owner_stack[owner_stack_depth].deps = owner;
    owner_stack[owner_stack_depth].label = owner_label;
    owner_stack[owner_stack_depth].dependent = call.dependent;
    owner_stack_depth++;
    int status = lua_pcall(state, 1, 1, 0);
    owner_stack_depth--;

    if (status != LUA_OK) return lua_error(state);
    return 1;
}

static int lua_require_module(lua_State *state) {
    const char *name = luaL_checkstring(state, 1);

    /* Before the split below and not after: a windows drive letter carries a
       colon too, so "C:/x" is an escape rather than a coordinate. */
    if (fr_lua_sandbox_climbs_out(name)) {
        return luaL_error(state, "daukle.require(\"%s\"): a module name may not leave the plugin",
                          name);
    }

    const char *colon = strchr(name, ':');
    if (colon != NULL) return require_across_plugins(state, name, colon);
    return require_own_module(state, name);
}

lua_CFunction fr_lua_verbs_require_function(void) { return lua_require_module; }

void fr_lua_modules_open(lua_State *state, int env, fr_plugin_source *source,
                         fr_plugin_deps *deps) {
    modules_clear(state);
    owner_stack_depth = 0;
    loading_source = source;
    loading_deps = deps;
    lua_pushvalue(state, env);
    lua_rawsetp(state, LUA_REGISTRYINDEX, &module_env_key);
}

void fr_lua_modules_close(lua_State *state) {
    loading_source = NULL;
    loading_deps = NULL;
    owner_stack_depth = 0;
    modules_clear(state);
    lua_pushnil(state);
    lua_rawsetp(state, LUA_REGISTRYINDEX, &module_env_key);
}
