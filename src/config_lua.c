#include "config_lua.h"

#include "error.h"
#include "luax.h"
#include "lua_sandbox.h"
#include "manifest.h"
#include "resolve.h"

#include "cJSON.h"
#include "lauxlib.h"

#include <stdlib.h>
#include <string.h>

static lua_State *runtime_state = NULL;
static void (*log_sink)(const char *message) = NULL;

static const char *host_os(void) {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
}

static const char *host_arch(void) {
#if defined(__aarch64__) || defined(_M_ARM64)
    return "aarch64";
#elif defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#else
    return "x86";
#endif
}

static int lua_env(lua_State *state) {
    const char *name = luaL_checkstring(state, 1);
    const char *value = getenv(name);
    if (value == NULL) {
        lua_pushnil(state);
        return 1;
    }
    lua_getglobal(state, "daukle");
    lua_getfield(state, -1, "_env_reads");
    lua_pushstring(state, value);
    lua_setfield(state, -2, name);
    lua_pop(state, 2);
    lua_pushstring(state, value);
    return 1;
}

static int lua_log(lua_State *state) {
    const char *message = luaL_checkstring(state, 1);
    if (log_sink != NULL) log_sink(message);
    return 0;
}

static int is_empty_object(const cJSON *value) {
    return cJSON_IsObject(value) && value->child == NULL;
}

static void restore_empty_arrays(const cJSON *original, cJSON *converted);

/* A key the script never touched, or overwrote with another empty table, reads back
   from Lua as an empty object either way: a table has no way to carry "I am an array"
   when it has no elements. The original document still knows, so a key whose original
   value was an array and whose converted value came back as an empty object is put
   back the way it started; a key the original did not have is left exactly as the
   script wrote it, and a non-empty converted object is never touched. */
static void restore_empty_arrays_in_object(const cJSON *original, cJSON *converted) {
    cJSON *child = converted->child;
    while (child != NULL) {
        cJSON *next = child->next;
        const char *key = child->string;
        const cJSON *original_child = cJSON_GetObjectItemCaseSensitive(original, key);
        if (original_child != NULL) {
            if (cJSON_IsArray(original_child) && is_empty_object(child)) {
                cJSON *empty_array = cJSON_CreateArray();
                if (empty_array != NULL) cJSON_ReplaceItemInObjectCaseSensitive(converted, key, empty_array);
            } else {
                restore_empty_arrays(original_child, child);
            }
        }
        child = next;
    }
}

static void restore_empty_arrays_in_array(const cJSON *original, cJSON *converted) {
    int original_size = cJSON_GetArraySize(original);
    int converted_size = cJSON_GetArraySize(converted);
    int common = original_size < converted_size ? original_size : converted_size;
    for (int index = 0; index < common; index++) {
        const cJSON *original_child = cJSON_GetArrayItem(original, index);
        cJSON *converted_child = cJSON_GetArrayItem(converted, index);
        if (cJSON_IsArray(original_child) && is_empty_object(converted_child)) {
            cJSON *empty_array = cJSON_CreateArray();
            if (empty_array != NULL) cJSON_ReplaceItemInArray(converted, index, empty_array);
        } else {
            restore_empty_arrays(original_child, converted_child);
        }
    }
}

static void restore_empty_arrays(const cJSON *original, cJSON *converted) {
    if (original == NULL || converted == NULL) return;
    if (cJSON_IsObject(converted)) {
        restore_empty_arrays_in_object(original, converted);
    } else if (cJSON_IsArray(converted)) {
        restore_empty_arrays_in_array(original, converted);
    }
}

#define FR_LUA_MAX_PLUGINS 32

typedef struct {
    char *capability;
    int callback;
} fr_lua_plugin_slot;

static fr_lua_plugin_slot plugin_slots[FR_LUA_MAX_PLUGINS];
static size_t plugin_slot_count;
static fr_registry *registering_into;

static fr_lua_plugin_slot *slot_for(const char *capability) {
    for (size_t index = 0; index < plugin_slot_count; index++) {
        if (strcmp(plugin_slots[index].capability, capability) == 0) return &plugin_slots[index];
    }
    return NULL;
}

static int lua_language_apply(void *state, const fr_consumer *consumer,
                              const fr_resolved *resolved, size_t count,
                              const char *original_text, char **out_text, fr_error *err) {
    fr_lua_plugin_slot *slot = state;
    int top = lua_gettop(runtime_state);
    lua_rawgeti(runtime_state, LUA_REGISTRYINDEX, slot->callback);

    lua_newtable(runtime_state);
    lua_pushstring(runtime_state, consumer->id);
    lua_setfield(runtime_state, -2, "id");
    lua_pushstring(runtime_state, consumer->configuration);
    lua_setfield(runtime_state, -2, "configuration");

    lua_newtable(runtime_state);
    for (size_t index = 0; index < count; index++) {
        lua_newtable(runtime_state);
        lua_pushstring(runtime_state, resolved[index].project);
        lua_setfield(runtime_state, -2, "project");
        lua_pushstring(runtime_state, resolved[index].module);
        lua_setfield(runtime_state, -2, "module");
        if (fr_lua_push_json(runtime_state, resolved[index].block, err) != FR_OK) {
            lua_settop(runtime_state, top);
            return FR_ERR;
        }
        lua_setfield(runtime_state, -2, "block");
        lua_rawseti(runtime_state, -2, (lua_Integer) index + 1);
    }

    lua_pushstring(runtime_state, original_text);

    if (lua_pcall(runtime_state, 3, 1, 0) != LUA_OK) {
        fr_error_set(err, "%s", lua_tostring(runtime_state, -1));
        lua_settop(runtime_state, top);
        return FR_ERR;
    }
    const char *produced = lua_tostring(runtime_state, -1);
    if (produced == NULL) {
        fr_error_set(err, "the \"%s\" plugin returned %s, expected a string",
                     slot->capability, luaL_typename(runtime_state, -1));
        lua_settop(runtime_state, top);
        return FR_ERR;
    }
    size_t length = strlen(produced) + 1;
    *out_text = malloc(length);
    if (*out_text == NULL) {
        fr_error_set(err, "out of memory taking the output of \"%s\"", slot->capability);
        lua_settop(runtime_state, top);
        return FR_ERR;
    }
    memcpy(*out_text, produced, length);
    lua_settop(runtime_state, top);
    return FR_OK;
}

static int lua_source_load(void *state, const char *project, const cJSON *block,
                           const char *base_dir, fr_project *out, fr_error *err) {
    fr_lua_plugin_slot *slot = state;
    int top = lua_gettop(runtime_state);
    lua_rawgeti(runtime_state, LUA_REGISTRYINDEX, slot->callback);
    lua_pushstring(runtime_state, project);
    if (fr_lua_push_json(runtime_state, block, err) != FR_OK) {
        lua_settop(runtime_state, top);
        return FR_ERR;
    }
    lua_pushstring(runtime_state, base_dir);

    if (lua_pcall(runtime_state, 3, 1, 0) != LUA_OK) {
        fr_error_set(err, "%s", lua_tostring(runtime_state, -1));
        lua_settop(runtime_state, top);
        return FR_ERR;
    }
    cJSON *document = NULL;
    int status = fr_lua_to_json(runtime_state, -1, &document, err);
    lua_settop(runtime_state, top);
    if (status != FR_OK) return FR_ERR;

    char *text = cJSON_PrintUnformatted(document);
    cJSON_Delete(document);
    if (text == NULL) {
        fr_error_set(err, "out of memory reading the project \"%s\" produced", project);
        return FR_ERR;
    }
    status = fr_project_parse(text, project, out, err);
    free(text);
    return status;
}

static int take_slot(lua_State *state, const char *prefix, const char *field,
                     fr_lua_plugin_slot **out) {
    lua_getfield(state, 1, "name");
    const char *name = lua_tostring(state, -1);
    if (name == NULL) return luaL_error(state, "a %s needs a name", prefix);

    if (plugin_slot_count == FR_LUA_MAX_PLUGINS) {
        return luaL_error(state, "too many plugins declared in one configuration");
    }

    char capability[128];
    snprintf(capability, sizeof capability, "%s%s", prefix, name);
    lua_pop(state, 1);

    if (slot_for(capability) != NULL) {
        return luaL_error(state, "\"%s\" is declared twice", capability);
    }

    fr_lua_plugin_slot *slot = &plugin_slots[plugin_slot_count];
    size_t length = strlen(capability) + 1;
    slot->capability = malloc(length);
    if (slot->capability == NULL) return luaL_error(state, "out of memory");
    memcpy(slot->capability, capability, length);

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

static int lua_declare_language(lua_State *state) {
    luaL_checktype(state, 1, LUA_TTABLE);
    fr_lua_plugin_slot *slot = NULL;
    take_slot(state, "daukle.language/", "apply", &slot);

    fr_language_plugin plugin = { slot->capability, lua_language_apply, slot };
    fr_error err;
    if (fr_registry_add_language(registering_into, &plugin, &err) != FR_OK) {
        return luaL_error(state, "%s", err.message);
    }
    return 0;
}

static int lua_declare_source(lua_State *state) {
    luaL_checktype(state, 1, LUA_TTABLE);
    fr_lua_plugin_slot *slot = NULL;
    take_slot(state, "daukle.source/", "load", &slot);

    fr_source_plugin plugin = { slot->capability, lua_source_load, slot };
    fr_error err;
    if (fr_registry_add_source(registering_into, &plugin, &err) != FR_OK) {
        return luaL_error(state, "%s", err.message);
    }
    return 0;
}

static int publish_daukle_table(lua_State *state, const cJSON *document, fr_error *err) {
    lua_getglobal(state, "daukle");

    if (document != NULL) {
        if (fr_lua_push_json(state, document, err) != FR_OK) return FR_ERR;
    } else {
        lua_newtable(state);
    }
    lua_setfield(state, -2, "config");

    lua_newtable(state);
    lua_pushstring(state, host_os());
    lua_setfield(state, -2, "os");
    lua_pushstring(state, host_arch());
    lua_setfield(state, -2, "arch");
    lua_setfield(state, -2, "host");

    lua_newtable(state);
    lua_setfield(state, -2, "_env_reads");

    lua_pushcfunction(state, lua_env);
    lua_setfield(state, -2, "env");
    lua_pushcfunction(state, lua_log);
    lua_setfield(state, -2, "log");
    lua_pushcfunction(state, lua_declare_language);
    lua_setfield(state, -2, "language");
    lua_pushcfunction(state, lua_declare_source);
    lua_setfield(state, -2, "source");

    lua_pop(state, 1);
    return FR_OK;
}

static int config_lua_load(void *state_unused, const char *text, const char *origin,
                           const char *base_dir, fr_registry *registry, const cJSON *document,
                           cJSON **out, fr_error *err) {
    (void) state_unused;

    fr_lua_runtime_shutdown();
    registering_into = registry;
    runtime_state = fr_lua_open(64u * 1024u * 1024u, err);
    if (runtime_state == NULL) return FR_ERR;
    if (fr_lua_sandbox_install(runtime_state, base_dir, err) != FR_OK) {
        fr_lua_runtime_shutdown();
        return FR_ERR;
    }
    if (publish_daukle_table(runtime_state, document, err) != FR_OK) return FR_ERR;

    if (fr_lua_run(runtime_state, text, origin, err) != FR_OK) return FR_ERR;

    lua_getglobal(runtime_state, "daukle");
    lua_getfield(runtime_state, -1, "config");
    int status = fr_lua_to_json(runtime_state, -1, out, err);
    lua_pop(runtime_state, 2);
    if (status == FR_OK && document != NULL) restore_empty_arrays(document, *out);
    return status;
}

void fr_lua_runtime_shutdown(void) {
    if (runtime_state != NULL) {
        fr_lua_close(runtime_state);
        runtime_state = NULL;
    }
    for (size_t index = 0; index < plugin_slot_count; index++) free(plugin_slots[index].capability);
    plugin_slot_count = 0;
    registering_into = NULL;
}

void fr_lua_set_log_sink(void (*sink)(const char *message)) {
    log_sink = sink;
}

const fr_config_plugin FR_CONFIG_LUA = { "daukle.config/lua", "daukle.lua", 1, config_lua_load, NULL };
