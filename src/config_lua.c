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
static long instruction_limit_override = 0;
static size_t memory_limit_override = 0;

#define FR_LUA_DEFAULT_MEMORY_LIMIT (64u * 1024u * 1024u)

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

/* Every function below whose name starts with "protected_" is entered as a plain
   C call (not a Lua call), so no lua_pcall frame is active yet: L->errorJmp is
   NULL. Nothing may touch the Lua C API directly from such an entry point, since
   a raw call that needs to allocate and finds the capped allocator refusing
   throws with no protected frame and no panic handler, which vendor/lua/ldo.c
   resolves with abort(). Each one instead pushes its own zero-upvalue C function
   (lua_pushcfunction never allocates) and runs it under its own lua_pcall, moving
   every raw API call inside that protected frame; the actual arguments travel
   through a file-static context struct rather than closure upvalues, since a
   closure with upvalues allocates on the push itself, before any protection
   exists to catch that allocation failing. */

typedef struct {
    fr_lua_plugin_slot *slot;
    const fr_consumer *consumer;
    const fr_resolved *resolved;
    size_t count;
    const char *original_text;
    char **out_text;
} lua_apply_context;

static lua_apply_context *apply_context;

static int protected_language_apply(lua_State *state) {
    const lua_apply_context *context = apply_context;
    const fr_lua_plugin_slot *slot = context->slot;
    lua_rawgeti(state, LUA_REGISTRYINDEX, slot->callback);

    lua_newtable(state);
    lua_pushstring(state, context->consumer->id);
    lua_setfield(state, -2, "id");
    lua_pushstring(state, context->consumer->configuration);
    lua_setfield(state, -2, "configuration");

    lua_newtable(state);
    for (size_t index = 0; index < context->count; index++) {
        lua_newtable(state);
        lua_pushstring(state, context->resolved[index].project);
        lua_setfield(state, -2, "project");
        lua_pushstring(state, context->resolved[index].module);
        lua_setfield(state, -2, "module");
        fr_error push_err;
        if (fr_lua_push_json(state, context->resolved[index].block, &push_err) != FR_OK) {
            return luaL_error(state, "%s", push_err.message);
        }
        lua_setfield(state, -2, "block");
        lua_rawseti(state, -2, (lua_Integer) index + 1);
    }

    lua_pushstring(state, context->original_text);

    /* lua_call, not lua_pcall: this whole function already runs inside the caller's
       protected frame, so an error the script raises here unwinds straight to it. */
    lua_call(state, 3, 1);

    const char *produced = lua_tostring(state, -1);
    if (produced == NULL) {
        return luaL_error(state, "the \"%s\" plugin returned %s, expected a string",
                          slot->capability, luaL_typename(state, -1));
    }
    size_t length = strlen(produced) + 1;
    char *copy = malloc(length);
    if (copy == NULL) {
        return luaL_error(state, "out of memory taking the output of \"%s\"", slot->capability);
    }
    memcpy(copy, produced, length);
    *context->out_text = copy;
    return 0;
}

static int lua_language_apply(void *state, const fr_consumer *consumer,
                              const fr_resolved *resolved, size_t count,
                              const char *original_text, char **out_text, fr_error *err) {
    int top = lua_gettop(runtime_state);
    lua_apply_context context = { state, consumer, resolved, count, original_text, out_text };
    apply_context = &context;
    lua_pushcfunction(runtime_state, protected_language_apply);
    int status = lua_pcall(runtime_state, 0, 0, 0);
    apply_context = NULL;

    if (status != LUA_OK) {
        fr_error_set(err, "%s", lua_tostring(runtime_state, -1));
        lua_settop(runtime_state, top);
        return FR_ERR;
    }
    lua_settop(runtime_state, top);
    return FR_OK;
}

typedef struct {
    fr_lua_plugin_slot *slot;
    const char *project;
    const cJSON *block;
    const char *base_dir;
    cJSON *document;
} lua_source_context;

static lua_source_context *source_context;

static int protected_source_load(lua_State *state) {
    lua_source_context *context = source_context;
    lua_rawgeti(state, LUA_REGISTRYINDEX, context->slot->callback);
    lua_pushstring(state, context->project);
    fr_error push_err;
    if (fr_lua_push_json(state, context->block, &push_err) != FR_OK) {
        return luaL_error(state, "%s", push_err.message);
    }
    lua_pushstring(state, context->base_dir);

    lua_call(state, 3, 1);

    fr_error to_json_err;
    if (fr_lua_to_json(state, -1, &context->document, &to_json_err) != FR_OK) {
        return luaL_error(state, "%s", to_json_err.message);
    }
    return 0;
}

static int lua_source_load(void *state, const char *project, const cJSON *block,
                           const char *base_dir, fr_project *out, fr_error *err) {
    int top = lua_gettop(runtime_state);
    lua_source_context context = { state, project, block, base_dir, NULL };
    source_context = &context;
    lua_pushcfunction(runtime_state, protected_source_load);
    int status = lua_pcall(runtime_state, 0, 0, 0);
    source_context = NULL;

    if (status != LUA_OK) {
        fr_error_set(err, "%s", lua_tostring(runtime_state, -1));
        lua_settop(runtime_state, top);
        return FR_ERR;
    }
    lua_settop(runtime_state, top);

    char *text = cJSON_PrintUnformatted(context.document);
    cJSON_Delete(context.document);
    if (text == NULL) {
        fr_error_set(err, "out of memory reading the project \"%s\" produced", project);
        return FR_ERR;
    }
    int parse_status = fr_project_parse(text, project, out, err);
    free(text);
    return parse_status;
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
    int written = snprintf(capability, sizeof capability, "%s%s", prefix, name);
    if (written < 0 || (size_t) written >= sizeof capability) {
        return luaL_error(state, "the plugin name \"%s\" is too long", name);
    }
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

static const cJSON *publish_document;

static int protected_publish_daukle_table(lua_State *state) {
    lua_getglobal(state, "daukle");

    if (publish_document != NULL) {
        fr_error push_err;
        if (fr_lua_push_json(state, publish_document, &push_err) != FR_OK) {
            return luaL_error(state, "%s", push_err.message);
        }
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
    return 0;
}

static int publish_daukle_table(lua_State *state, const cJSON *document, fr_error *err) {
    int top = lua_gettop(state);
    publish_document = document;
    lua_pushcfunction(state, protected_publish_daukle_table);
    int status = lua_pcall(state, 0, 0, 0);
    publish_document = NULL;

    if (status != LUA_OK) {
        fr_error_set(err, "%s", lua_tostring(state, -1));
        lua_settop(state, top);
        return FR_ERR;
    }
    lua_settop(state, top);
    return FR_OK;
}

static cJSON **extract_config_out;

static int protected_extract_config(lua_State *state) {
    lua_getglobal(state, "daukle");
    lua_getfield(state, -1, "config");
    fr_error to_json_err;
    if (fr_lua_to_json(state, -1, extract_config_out, &to_json_err) != FR_OK) {
        return luaL_error(state, "%s", to_json_err.message);
    }
    return 0;
}

static int config_lua_load(void *state_unused, const char *text, const char *origin,
                           const char *base_dir, fr_registry *registry, const cJSON *document,
                           cJSON **out, fr_error *err) {
    (void) state_unused;

    fr_lua_runtime_shutdown();
    registering_into = registry;
    size_t memory_limit = memory_limit_override != 0 ? memory_limit_override : FR_LUA_DEFAULT_MEMORY_LIMIT;
    runtime_state = fr_lua_open(memory_limit, err);
    if (runtime_state == NULL) return FR_ERR;
    if (instruction_limit_override != 0) {
        fr_lua_set_instruction_limit(runtime_state, instruction_limit_override);
    }
    if (fr_lua_sandbox_install(runtime_state, base_dir, err) != FR_OK) {
        fr_lua_runtime_shutdown();
        return FR_ERR;
    }
    if (publish_daukle_table(runtime_state, document, err) != FR_OK) return FR_ERR;

    if (fr_lua_run(runtime_state, text, origin, err) != FR_OK) return FR_ERR;

    int top = lua_gettop(runtime_state);
    extract_config_out = out;
    lua_pushcfunction(runtime_state, protected_extract_config);
    int status = lua_pcall(runtime_state, 0, 0, 0);
    extract_config_out = NULL;

    if (status != LUA_OK) {
        fr_error_set(err, "%s", lua_tostring(runtime_state, -1));
        lua_settop(runtime_state, top);
        return FR_ERR;
    }
    lua_settop(runtime_state, top);

    if (document != NULL) restore_empty_arrays(document, *out);
    return FR_OK;
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

void fr_lua_set_limits(long instruction_limit, size_t memory_limit) {
    instruction_limit_override = instruction_limit;
    memory_limit_override = memory_limit;
}

const fr_config_plugin FR_CONFIG_LUA = { "daukle.config/lua", "daukle.lua", 1, config_lua_load, NULL };
