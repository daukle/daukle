#include "config_lua.h"

#include "derived.h"
#include "error.h"
#include "luax.h"
#include "lua_sandbox.h"
#include "lua_verbs.h"
#include "manifest.h"
#include "plugins.h"
#include "resolve.h"

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
    char *part_of;
    char **depends_on;
    size_t depends_on_count;
    int has_run;
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
        fr_error_set(err, "%s", fr_lua_error_text(runtime_state));
        lua_settop(runtime_state, top);
        return FR_ERR;
    }
    lua_settop(runtime_state, top);
    return FR_OK;
}

typedef struct {
    fr_lua_plugin_slot *slot;
    const fr_toolchain *toolchain;
    const char *project;
    const char *version;
    const char *root;
    const fr_resolved *resolved;
    size_t count;
    fr_generated_file **out_files;
    size_t *out_count;
} lua_generate_context;

static lua_generate_context *generate_context;

static int protected_toolchain_generate(lua_State *state) {
    const lua_generate_context *context = generate_context;
    const fr_lua_plugin_slot *slot = context->slot;
    lua_rawgeti(state, LUA_REGISTRYINDEX, slot->callback);

    lua_newtable(state);
    lua_pushstring(state, context->project);
    lua_setfield(state, -2, "project");
    lua_pushstring(state, context->version);
    lua_setfield(state, -2, "version");
    lua_pushstring(state, context->root);
    lua_setfield(state, -2, "root");

    lua_newtable(state);
    lua_pushstring(state, host_os());
    lua_setfield(state, -2, "os");
    lua_pushstring(state, host_arch());
    lua_setfield(state, -2, "arch");
    lua_setfield(state, -2, "host");

    lua_newtable(state);
    const cJSON *block = context->toolchain->block;
    if (block != NULL) {
        const cJSON *member = block->child;
        while (member != NULL) {
            /* dependencies already reach the plugin as their own argument, so
               config must not also carry the raw copy. version is dropped too:
               its raw constraint has no plugin use in this version (spec
               section 7), and the "version" argument now carries the
               project's own version instead. */
            if (strcmp(member->string, "version") != 0 &&
                strcmp(member->string, "dependencies") != 0) {
                fr_error push_err;
                if (fr_lua_push_json(state, member, &push_err) != FR_OK) {
                    return luaL_error(state, "%s", push_err.message);
                }
                lua_setfield(state, -2, member->string);
            }
            member = member->next;
        }
    }
    lua_setfield(state, -2, "config");

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
    lua_setfield(state, -2, "dependencies");

    /* lua_call, not lua_pcall: see protected_language_apply above. */
    lua_call(state, 1, 1);

    if (!lua_istable(state, -1)) {
        return luaL_error(state, "the \"%s\" plugin returned %s, expected a table",
                          slot->capability, luaL_typename(state, -1));
    }

    size_t file_count = 0;
    lua_pushnil(state);
    while (lua_next(state, -2) != 0) {
        file_count++;
        lua_pop(state, 1);
    }

    fr_generated_file *files = NULL;
    if (file_count > 0) {
        files = malloc(file_count * sizeof *files);
        if (files == NULL) {
            return luaL_error(state, "out of memory taking the output of \"%s\"", slot->capability);
        }
    }

    size_t written = 0;
    lua_pushnil(state);
    while (lua_next(state, -2) != 0) {
        if (lua_type(state, -2) != LUA_TSTRING) {
            fr_derived_free_files(files, written);
            return luaL_error(state, "a generated file path must be a string");
        }
        const char *path = lua_tostring(state, -2);
        if (lua_type(state, -1) != LUA_TSTRING) {
            fr_derived_free_files(files, written);
            return luaL_error(state, "generated file \"%s\" must be a string", path);
        }
        const char *text = lua_tostring(state, -1);

        files[written].path = fr_dup_string(path);
        files[written].text = fr_dup_string(text);
        if (files[written].path == NULL || files[written].text == NULL) {
            fr_derived_free_files(files, written + 1);
            return luaL_error(state, "out of memory taking the output of \"%s\"", slot->capability);
        }
        written++;
        lua_pop(state, 1);
    }

    *context->out_files = files;
    *context->out_count = written;
    return 0;
}

static int generation_is_running;

int fr_lua_generation_is_running(void) {
    return generation_is_running;
}

static int lua_toolchain_generate(void *state, const fr_toolchain *toolchain, const char *project,
                                  const char *version, const char *root,
                                  const fr_resolved *resolved, size_t count,
                                  fr_generated_file **out_files, size_t *out_count, fr_error *err) {
    int top = lua_gettop(runtime_state);
    lua_generate_context context = { state, toolchain, project, version, root,
                                     resolved, count, out_files, out_count };
    generate_context = &context;
    lua_pushcfunction(runtime_state, protected_toolchain_generate);
    generation_is_running = 1;
    int status = lua_pcall(runtime_state, 0, 0, 0);
    generation_is_running = 0;
    generate_context = NULL;

    if (status != LUA_OK) {
        fr_error_set(err, "%s", fr_lua_error_text(runtime_state));
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
        fr_error_set(err, "%s", fr_lua_error_text(runtime_state));
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

/* Returns 0 with *out set, or does not return at all: every failure below is
   raised, not reported. Callers still check, because the 0-on-success return
   would otherwise invite a dereference that only longjmp keeps safe. */
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
    if (fr_lua_verbs_env_declared_exec()) {
        return luaL_error(state, "daukle.exec is available only to a toolchain plugin");
    }
    luaL_checktype(state, 1, LUA_TTABLE);
    fr_lua_plugin_slot *slot = NULL;
    if (take_slot(state, "daukle.language/", "apply", &slot) != 0 || slot == NULL) {
        return luaL_error(state, "a language plugin could not be declared");
    }

    fr_language_plugin plugin = { slot->capability, lua_language_apply, slot };
    fr_error err;
    if (fr_registry_add_language(registering_into, &plugin, &err) != FR_OK) {
        return luaL_error(state, "%s", err.message);
    }
    return 0;
}

static int lua_declare_source(lua_State *state) {
    if (fr_lua_verbs_env_declared_exec()) {
        return luaL_error(state, "daukle.exec is available only to a toolchain plugin");
    }
    luaL_checktype(state, 1, LUA_TTABLE);
    fr_lua_plugin_slot *slot = NULL;
    if (take_slot(state, "daukle.source/", "load", &slot) != 0 || slot == NULL) {
        return luaL_error(state, "a source plugin could not be declared");
    }

    fr_source_plugin plugin = { slot->capability, lua_source_load, slot };
    fr_error err;
    if (fr_registry_add_source(registering_into, &plugin, &err) != FR_OK) {
        return luaL_error(state, "%s", err.message);
    }
    return 0;
}

/* Which toolchains the chunk now running has declared, so that a task naming
   "<toolchain>:" can be checked against them. Reset per chunk in
   fr_lua_plugin_load. The check runs at declaration rather than at the end of
   the chunk, which means a plugin must declare its toolchain before the tasks
   that belong to it; that ordering is stated in AUTHORING.md and is the same
   direction Lua already forces on a plugin using a local it defined earlier. */
#define FR_LUA_MAX_CHUNK_TOOLCHAINS 8
static char *chunk_toolchains[FR_LUA_MAX_CHUNK_TOOLCHAINS];
static size_t chunk_toolchain_count;

static void chunk_toolchains_clear(void) {
    for (size_t index = 0; index < chunk_toolchain_count; index++) free(chunk_toolchains[index]);
    chunk_toolchain_count = 0;
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
    luaL_checktype(state, 1, LUA_TTABLE);
    fr_lua_plugin_slot *slot = NULL;
    if (take_slot(state, "daukle.toolchain/", "generate", &slot) != 0 || slot == NULL) {
        return luaL_error(state, "a toolchain plugin could not be declared");
    }

    if (chunk_toolchain_count < FR_LUA_MAX_CHUNK_TOOLCHAINS) {
        lua_getfield(state, 1, "name");
        chunk_toolchains[chunk_toolchain_count] = fr_dup_string(lua_tostring(state, -1));
        lua_pop(state, 1);
        if (chunk_toolchains[chunk_toolchain_count] == NULL) return luaL_error(state, "out of memory");
        chunk_toolchain_count++;
    }

    fr_toolchain_plugin plugin = { slot->capability, lua_toolchain_generate, slot };
    fr_error err;
    if (fr_registry_add_toolchain(registering_into, &plugin, &err) != FR_OK) {
        return luaL_error(state, "%s", err.message);
    }
    return 0;
}

static int lua_task_run(void *state, const fr_task_run_context *context, fr_error *err) {
    (void) state; (void) context;
    fr_error_set(err, "running a task is not implemented yet");
    return FR_ERR;
}

static int lua_declare_task(lua_State *state) {
    luaL_checktype(state, 1, LUA_TTABLE);

    lua_getfield(state, 1, "name");
    const char *name = lua_tostring(state, -1);
    if (name == NULL) return luaL_error(state, "a task needs a name");

    const char *colon = strchr(name, ':');
    if (colon != NULL && !chunk_declares_toolchain(name, (size_t) (colon - name))) {
        char toolchain_name[64];
        size_t toolchain_length = (size_t) (colon - name);
        if (toolchain_length >= sizeof toolchain_name) toolchain_length = sizeof toolchain_name - 1;
        memcpy(toolchain_name, name, toolchain_length);
        toolchain_name[toolchain_length] = '\0';
        /* luaL_error's format string is Lua's own minimal printf (lua_pushvfstring),
           which has no precision specifier: "%.*s" raises "invalid option '%.'"
           at runtime instead of truncating, so the substring is built by hand first. */
        return luaL_error(state, "task \"%s\" cannot be declared here: this plugin declares no"
                                 " toolchain \"%s\"", name, toolchain_name);
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
        return luaL_error(state, "too many plugins declared in one configuration");
    }

    fr_lua_plugin_slot *slot = &plugin_slots[plugin_slot_count];
    memset(slot, 0, sizeof *slot);
    slot->capability = fr_dup_string(capability);
    if (slot->capability == NULL) return luaL_error(state, "out of memory");

    lua_getfield(state, 1, "partOf");
    if (!lua_isnil(state, -1)) {
        const char *part_of = luaL_checkstring(state, -1);
        slot->part_of = fr_dup_string(part_of);
        if (slot->part_of == NULL) return luaL_error(state, "out of memory");
    }
    lua_pop(state, 1);

    lua_getfield(state, 1, "dependsOn");
    if (!lua_isnil(state, -1)) {
        luaL_checktype(state, -1, LUA_TTABLE);
        lua_Integer length = luaL_len(state, -1);
        if (length > 0) {
            slot->depends_on = calloc((size_t) length, sizeof *slot->depends_on);
            if (slot->depends_on == NULL) return luaL_error(state, "out of memory");
        }
        for (lua_Integer index = 1; index <= length; index++) {
            lua_geti(state, -1, index);
            const char *item = luaL_checkstring(state, -1);
            slot->depends_on[index - 1] = fr_dup_string(item);
            if (slot->depends_on[index - 1] == NULL) return luaL_error(state, "out of memory");
            slot->depends_on_count = (size_t) index;
            lua_pop(state, 1);
        }
    }
    lua_pop(state, 1);

    lua_getfield(state, 1, "run");
    if (!lua_isnil(state, -1)) {
        if (!lua_isfunction(state, -1)) {
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
                              slot->has_run ? lua_task_run : NULL, slot };
    fr_error err;
    if (fr_registry_add_task(registering_into, &plugin, &err) != FR_OK) {
        return luaL_error(state, "%s", err.message);
    }
    return 0;
}

static int lua_declare_plugin(lua_State *state) {
    (void) state;
    return 0;
}

void fr_lua_verbs_install_registration(lua_State *state) {
    lua_pushcfunction(state, lua_declare_language);
    lua_setfield(state, -2, "language");
    lua_pushcfunction(state, lua_declare_source);
    lua_setfield(state, -2, "source");
    lua_pushcfunction(state, lua_declare_toolchain);
    lua_setfield(state, -2, "toolchain");
    lua_pushcfunction(state, lua_declare_task);
    lua_setfield(state, -2, "task");
    lua_pushcfunction(state, lua_declare_plugin);
    lua_setfield(state, -2, "plugin");
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
    if (registering_into != registry) {
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
    registering_into = registry;
    return FR_OK;
}

lua_State *fr_lua_runtime_state(void) {
    return runtime_state;
}

fr_registry *fr_lua_registering_registry(void) {
    return registering_into;
}

static int plugin_chunk_running;

int fr_lua_plugin_exec_is_refused(void) {
    return plugin_chunk_running;
}

int fr_lua_plugin_load(const char *text, const char *origin, const char *const *verbs,
                       size_t verb_count, fr_error *err) {
    lua_State *state = fr_lua_runtime_state();
    if (state == NULL) {
        fr_error_set(err, "no lua runtime is open for \"%s\"", origin);
        return FR_ERR;
    }

    int top = lua_gettop(state);
    if (fr_lua_verbs_push_env(state, verbs, verb_count, err) != FR_OK) return FR_ERR;
    int env = lua_gettop(state);

    chunk_toolchains_clear();
    plugin_chunk_running = 1;
    int status = fr_lua_run_in_env(state, text, origin, env, err);
    plugin_chunk_running = 0;
    chunk_toolchains_clear();
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
        fr_lua_close(runtime_state);
        runtime_state = NULL;
    }
    for (size_t index = 0; index < plugin_slot_count; index++) {
        free(plugin_slots[index].capability);
        free(plugin_slots[index].part_of);
        for (size_t d = 0; d < plugin_slots[index].depends_on_count; d++) {
            free(plugin_slots[index].depends_on[d]);
        }
        free(plugin_slots[index].depends_on);
    }
    plugin_slot_count = 0;
    free(runtime_base_dir);
    runtime_base_dir = NULL;
    registering_into = NULL;
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
