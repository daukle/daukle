#include "config/config_lua_internal.h"

#include "config/config_lua.h"
#include "lua/lua_verbs.h"
#include "lua/luax.h"
#include "plugin/plugins.h"

#include "lauxlib.h"

#include <stdlib.h>
#include <string.h>

#define FR_MIGRATE_CAPABILITY_SIZE 256

static const char *chunk_label;

void fr_lua_declare_set_chunk_label(const char *label) {
    chunk_label = label;
}

void fr_lua_declare_free_migrate_strings(fr_lua_plugin_slot *slot) {
    free(slot->tool);
    free(slot->toolchain);
    free(slot->id);
    free(slot->label);
}

/* Frees what a slot owns before it is counted: the shutdown loop walks slots by
   count, so a slot a declaration raised out of has no other way to be released. */
static void release_migrate_slot(fr_lua_plugin_slot *slot) {
    free(slot->capability);
    fr_lua_declare_free_migrate_strings(slot);
    for (size_t index = 0; index < slot->env_count; index++) free(slot->env[index]);
    free(slot->env);
    memset(slot, 0, sizeof *slot);
}

static int refuse_unless_declarable(lua_State *state, const char *kind) {
    if (fr_lua_declare_chunk_declared_resolver()) {
        return luaL_error(state, "a resolver chunk declares only a resolver");
    }
    fr_lua_declare_mark_chunk_other();
    luaL_checktype(state, 1, LUA_TTABLE);
    if (chunk_label == NULL) {
        return luaL_error(state, "a %s may only be declared by a plugin a manifest or"
                                 " daukle migrate loaded", kind);
    }
    return 0;
}

/* Leaves the field's string on the stack, so the pointer stays valid until the
   caller pops it. */
static const char *push_required_string(lua_State *state, const char *kind, const char *field) {
    fr_lua_raw_getfield(state, 1, field);
    const char *text = lua_type(state, -1) == LUA_TSTRING ? lua_tostring(state, -1) : NULL;
    if (text == NULL || text[0] == '\0') {
        luaL_error(state, "a %s needs a non-empty %s string", kind, field);
    }
    return text;
}

static void require_function_field(lua_State *state, fr_lua_plugin_slot *slot, const char *field,
                                   int *out_reference) {
    fr_lua_raw_getfield(state, 1, field);
    if (!lua_isfunction(state, -1)) {
        char capability[FR_MIGRATE_CAPABILITY_SIZE];
        snprintf(capability, sizeof capability, "%s", slot->capability);
        release_migrate_slot(slot);
        luaL_error(state, "\"%s\" needs %s to be a function", capability, field);
    }
    *out_reference = luaL_ref(state, LUA_REGISTRYINDEX);
}

/* Takes and zeroes the next slot with its capability, label and env copied.
   Raises, after releasing the slot, on any failure. */
static fr_lua_plugin_slot *take_slot(lua_State *state, const char *capability) {
    fr_lua_plugin_slot *slot = fr_lua_declare_next_slot();
    if (slot == NULL) {
        luaL_error(state, "too many declarations in one configuration;"
                          " at most %d are allowed", FR_LUA_MAX_PLUGINS);
    }
    memset(slot, 0, sizeof *slot);
    slot->second_callback = LUA_NOREF;
    slot->capability = fr_dup_string(capability);
    slot->label = fr_dup_string(chunk_label);
    if (slot->capability == NULL || slot->label == NULL
        || fr_lua_verbs_copy_declared_env(&slot->env, &slot->env_count) != 0) {
        release_migrate_slot(slot);
        luaL_error(state, "out of memory recording \"%s\"", capability);
    }
    return slot;
}

static void copy_string_or_raise(lua_State *state, fr_lua_plugin_slot *slot, char **target,
                                 const char *text) {
    *target = fr_dup_string(text);
    if (*target == NULL) {
        release_migrate_slot(slot);
        luaL_error(state, "out of memory recording \"%s\"", text);
    }
}

int fr_lua_declare_migrator(lua_State *state) {
    refuse_unless_declarable(state, "migrator");
    const char *tool = push_required_string(state, "migrator", "tool");
    const char *toolchain = push_required_string(state, "migrator", "toolchain");
    if (!fr_lua_declare_chunk_declares_toolchain(toolchain, strlen(toolchain))) {
        return luaL_error(state, "migrator for \"%s\" names toolchain \"%s\", which is not"
                                 " declared above it in this plugin", tool, toolchain);
    }

    char capability[FR_MIGRATE_CAPABILITY_SIZE];
    int written = snprintf(capability, sizeof capability, "daukle.migrator/%s", chunk_label);
    if (written < 0 || (size_t) written >= sizeof capability) {
        return luaL_error(state, "the plugin label \"%s\" is too long for a migrator", chunk_label);
    }
    if (fr_lua_declare_slot_for(capability) != NULL) {
        return luaL_error(state, "a plugin declares at most one migrator");
    }
    fr_lua_plugin_slot *slot = take_slot(state, capability);
    copy_string_or_raise(state, slot, &slot->tool, tool);
    copy_string_or_raise(state, slot, &slot->toolchain, toolchain);
    lua_pop(state, 2);

    require_function_field(state, slot, "run", &slot->callback);
    fr_lua_raw_getfield(state, 1, "build");
    if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
    } else if (lua_isfunction(state, -1)) {
        slot->second_callback = luaL_ref(state, LUA_REGISTRYINDEX);
    } else {
        release_migrate_slot(slot);
        return luaL_error(state, "\"%s\" needs build to be a function or absent", capability);
    }
    fr_lua_declare_count_slot();

    fr_migrator_plugin plugin = { slot->capability, slot->label, slot->tool, slot->toolchain, slot };
    fr_error err;
    if (fr_registry_add_migrator(fr_lua_registering_registry(), &plugin, &err) != FR_OK) {
        return luaL_error(state, "%s", err.message);
    }
    return 0;
}

int fr_lua_declare_translator(lua_State *state) {
    refuse_unless_declarable(state, "translator");
    const char *tool = push_required_string(state, "translator", "tool");
    const char *id = push_required_string(state, "translator", "id");

    char capability[FR_MIGRATE_CAPABILITY_SIZE];
    int written = snprintf(capability, sizeof capability, "daukle.translator/%s/%s", tool, id);
    if (written < 0 || (size_t) written >= sizeof capability) {
        return luaL_error(state, "the translator \"%s\" of \"%s\" is too long", id, tool);
    }
    if (fr_lua_declare_slot_for(capability) != NULL) {
        return luaL_error(state, "\"%s\" is declared twice", capability);
    }
    fr_lua_plugin_slot *slot = take_slot(state, capability);
    copy_string_or_raise(state, slot, &slot->tool, tool);
    copy_string_or_raise(state, slot, &slot->id, id);
    lua_pop(state, 2);

    require_function_field(state, slot, "translate", &slot->callback);
    fr_lua_declare_count_slot();

    fr_translator_plugin plugin = { slot->capability, slot->label, slot->tool, slot->id, slot };
    fr_error err;
    if (fr_registry_add_translator(fr_lua_registering_registry(), &plugin, &err) != FR_OK) {
        return luaL_error(state, "%s", err.message);
    }
    return 0;
}
