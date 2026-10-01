#include "config/config_lua.h"

#include "config/config_lua_internal.h"
#include "archive/tar.h"
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

static lua_State *runtime_state = NULL;
static char *runtime_base_dir = NULL;
static void (*log_sink)(const char *message) = NULL;
static long instruction_limit_override = 0;
static size_t memory_limit_override = 0;

#define FR_LUA_DEFAULT_MEMORY_LIMIT (64u * 1024u * 1024u)

const char *fr_lua_host_os(void) {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
}

const char *fr_lua_host_arch(void) {
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
    lua_pushstring(state, fr_lua_host_os());
    lua_setfield(state, -2, "os");
    lua_pushstring(state, fr_lua_host_arch());
    lua_setfield(state, -2, "arch");
    lua_setfield(state, -2, "host");

    lua_newtable(state);
    lua_setfield(state, -2, "_env_reads");

    lua_pushcfunction(state, lua_env);
    lua_setfield(state, -2, "env");
    lua_pushcfunction(state, lua_log);
    lua_setfield(state, -2, "log");
    fr_lua_declare_install_config_surface(state);

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
        fr_error_set(err, "%s", fr_lua_error_text(state));
        lua_settop(state, top);
        return FR_ERR;
    }
    lua_settop(state, top);
    return FR_OK;
}

static cJSON **extract_config_out;
static cJSON *env_reads_document;

static int protected_extract_config(lua_State *state) {
    lua_getglobal(state, "daukle");
    lua_getfield(state, -1, "config");
    fr_error to_json_err;
    if (fr_lua_to_json(state, -1, extract_config_out, &to_json_err) != FR_OK) {
        return luaL_error(state, "%s", to_json_err.message);
    }
    /* A script that overwrote daukle._env_reads loses the record, not the load:
       raising here would report a diagnostic's failure as the config's, and
       would leak the document already converted into *extract_config_out. */
    lua_getfield(state, -2, "_env_reads");
    cJSON_Delete(env_reads_document);
    env_reads_document = NULL;
    if (lua_type(state, -1) == LUA_TTABLE &&
        fr_lua_to_json(state, -1, &env_reads_document, &to_json_err) != FR_OK) {
        cJSON_Delete(env_reads_document);
        env_reads_document = NULL;
    }
    return 0;
}

const cJSON *fr_lua_env_reads(void) {
    return env_reads_document;
}

static int open_runtime_serves(const fr_registry *registry, const char *canonical_base,
                               fr_error *err) {
    if (fr_lua_registering_registry() != registry) {
        fr_error_set(err, "another registry's plugins are still registered: destroy that"
                          " registry before loading into a different one");
        return 0;
    }
    if (strcmp(runtime_base_dir, canonical_base) != 0) {
        fr_error_set(err, "a lua runtime is already bound to \"%s\" and cannot be reused for"
                          " \"%s\": shut it down first", runtime_base_dir, canonical_base);
        return 0;
    }
    return 1;
}

int fr_lua_runtime_begin(const char *base_dir, fr_registry *registry, fr_error *err) {
    if (runtime_state != NULL) {
        char *canonical = NULL;
        if (fr_lua_sandbox_canonical_dir(base_dir, &canonical, err) != FR_OK) return FR_ERR;
        int serves = open_runtime_serves(registry, canonical, err);
        free(canonical);
        return serves ? FR_OK : FR_ERR;
    }

    size_t memory_limit = memory_limit_override != 0 ? memory_limit_override : FR_LUA_DEFAULT_MEMORY_LIMIT;
    runtime_state = fr_lua_open(memory_limit, err);
    if (runtime_state == NULL) return FR_ERR;
    if (instruction_limit_override != 0) {
        fr_lua_set_instruction_limit(runtime_state, instruction_limit_override);
    }
    if (fr_lua_sandbox_install(runtime_state, base_dir, &runtime_base_dir, err) != FR_OK) {
        fr_lua_runtime_shutdown();
        return FR_ERR;
    }
    fr_lua_declare_set_registry(registry);
    return FR_OK;
}

lua_State *fr_lua_runtime_state(void) {
    return runtime_state;
}

static int plugin_chunk_running;

int fr_lua_plugin_exec_is_refused(void) {
    return plugin_chunk_running;
}

int fr_lua_plugin_load(const char *text, size_t length, const char *origin,
                       const char *const *verbs, size_t verb_count, fr_plugin_source *source,
                       fr_plugin_deps *deps, fr_error *err) {
    lua_State *state = fr_lua_runtime_state();
    if (state == NULL) {
        fr_error_set(err, "no lua runtime is open for \"%s\"", origin);
        return FR_ERR;
    }

    int top = lua_gettop(state);
    if (fr_lua_verbs_push_env(state, verbs, verb_count, err) != FR_OK) return FR_ERR;
    int env = lua_gettop(state);

    int backup_resolver_callback = fr_lua_declare_chunk_begin(state);
    fr_lua_modules_open(state, env, source, deps);

    plugin_chunk_running = 1;
    int status = fr_lua_run_in_env_bytes(state, text, length, origin, env, err);
    plugin_chunk_running = 0;

    fr_lua_modules_close(state);

    fr_lua_declare_chunk_end(state, status == FR_OK, backup_resolver_callback);
    lua_settop(state, top);
    return status;
}

static int config_lua_load(void *state_unused, const char *text, const char *origin,
                           const char *base_dir, fr_registry *registry, const cJSON *document,
                           cJSON **out, fr_error *err) {
    (void) state_unused;

    if (fr_lua_runtime_begin(base_dir, registry, err) != FR_OK) return FR_ERR;

    if (publish_daukle_table(runtime_state, document, err) != FR_OK) return FR_ERR;

    if (fr_lua_run(runtime_state, text, origin, err) != FR_OK) return FR_ERR;

    int top = lua_gettop(runtime_state);
    extract_config_out = out;
    lua_pushcfunction(runtime_state, protected_extract_config);
    int status = lua_pcall(runtime_state, 0, 0, 0);
    extract_config_out = NULL;

    if (status != LUA_OK) {
        fr_error_set(err, "%s", fr_lua_error_text(runtime_state));
        lua_settop(runtime_state, top);
        return FR_ERR;
    }
    lua_settop(runtime_state, top);

    if (document != NULL) restore_empty_arrays(document, *out);
    return FR_OK;
}

void fr_lua_runtime_shutdown(void) {
    if (runtime_state != NULL) {
        fr_lua_modules_close(runtime_state);
        fr_lua_close(runtime_state);
        runtime_state = NULL;
    }
    fr_lua_declare_reset();
    free(runtime_base_dir);
    runtime_base_dir = NULL;
    cJSON_Delete(env_reads_document);
    env_reads_document = NULL;
}

void fr_lua_set_log_sink(void (*sink)(const char *message)) {
    log_sink = sink;
}

void fr_lua_set_limits(long instruction_limit, size_t memory_limit) {
    instruction_limit_override = instruction_limit;
    memory_limit_override = memory_limit;
}

const fr_config_plugin FR_CONFIG_LUA = { "daukle.config/lua", "daukle.lua", 1, config_lua_load, NULL };
