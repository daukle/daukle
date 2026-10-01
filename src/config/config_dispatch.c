#include "config/config_lua_internal.h"

#include "config/config_lua.h"
#include "plugin/resolve.h"
#include "config/manifest.h"
#include "lua/lua_verbs.h"
#include "lua/luax.h"
#include "project/derived.h"
#include "util/error.h"

#include "cJSON.h"
#include "lauxlib.h"

#include <stdlib.h>
#include <string.h>

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

int fr_lua_dispatch_language_apply(void *state, const fr_consumer *consumer,
                              const fr_resolved *resolved, size_t count,
                              const char *original_text, char **out_text, fr_error *err) {
    int top = lua_gettop(fr_lua_runtime_state());
    lua_apply_context context = { state, consumer, resolved, count, original_text, out_text };
    apply_context = &context;
    fr_lua_verbs_set_declared_env(
        (const char *const *) ((const fr_lua_plugin_slot *) state)->env,
        ((const fr_lua_plugin_slot *) state)->env_count);
    lua_pushcfunction(fr_lua_runtime_state(), protected_language_apply);
    int status = lua_pcall(fr_lua_runtime_state(), 0, 0, 0);
    apply_context = NULL;
    fr_lua_verbs_set_declared_env(NULL, 0);

    if (status != LUA_OK) {
        fr_error_set(err, "%s", fr_lua_error_text(fr_lua_runtime_state()));
        lua_settop(fr_lua_runtime_state(), top);
        return FR_ERR;
    }
    lua_settop(fr_lua_runtime_state(), top);
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
    lua_pushstring(state, fr_lua_host_os());
    lua_setfield(state, -2, "os");
    lua_pushstring(state, fr_lua_host_arch());
    lua_setfield(state, -2, "arch");
    lua_setfield(state, -2, "host");

    lua_newtable(state);
    const cJSON *block = context->toolchain->block;
    if (block != NULL) {
        const cJSON *member = block->child;
        while (member != NULL) {
            /* dependencies already reach the plugin as their own argument, so
               config must not also carry the raw copy. version stays: a
               generated file may legitimately depend on the requested
               version, e.g. "engines": { "node": ">=20" }, and core does not
               parse the string to know whether it is a version, a range or a
               codename. */
            if (strcmp(member->string, "dependencies") != 0) {
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

static const char *task_cwd;

const char *fr_lua_task_cwd(void) {
    return task_cwd;
}

void fr_lua_set_task_cwd(const char *directory) {
    task_cwd = directory;
}

int fr_lua_dispatch_toolchain_generate(void *state, const fr_toolchain *toolchain, const char *project,
                                  const char *version, const char *root,
                                  const fr_resolved *resolved, size_t count,
                                  fr_generated_file **out_files, size_t *out_count, fr_error *err) {
    int top = lua_gettop(fr_lua_runtime_state());
    lua_generate_context context = { state, toolchain, project, version, root,
                                     resolved, count, out_files, out_count };
    generate_context = &context;
    fr_lua_verbs_set_declared_env(
        (const char *const *) ((const fr_lua_plugin_slot *) state)->env,
        ((const fr_lua_plugin_slot *) state)->env_count);
    lua_pushcfunction(fr_lua_runtime_state(), protected_toolchain_generate);
    generation_is_running = 1;
    int status = lua_pcall(fr_lua_runtime_state(), 0, 0, 0);
    generation_is_running = 0;
    generate_context = NULL;
    fr_lua_verbs_set_declared_env(NULL, 0);

    if (status != LUA_OK) {
        fr_error_set(err, "%s", fr_lua_error_text(fr_lua_runtime_state()));
        lua_settop(fr_lua_runtime_state(), top);
        return FR_ERR;
    }
    lua_settop(fr_lua_runtime_state(), top);
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

int fr_lua_dispatch_source_load(void *state, const char *project, const cJSON *block,
                           const char *base_dir, fr_project *out, fr_error *err) {
    int top = lua_gettop(fr_lua_runtime_state());
    lua_source_context context = { state, project, block, base_dir, NULL };
    source_context = &context;
    fr_lua_verbs_set_declared_env(
        (const char *const *) ((const fr_lua_plugin_slot *) state)->env,
        ((const fr_lua_plugin_slot *) state)->env_count);
    lua_pushcfunction(fr_lua_runtime_state(), protected_source_load);
    int status = lua_pcall(fr_lua_runtime_state(), 0, 0, 0);
    source_context = NULL;
    fr_lua_verbs_set_declared_env(NULL, 0);

    if (status != LUA_OK) {
        fr_error_set(err, "%s", fr_lua_error_text(fr_lua_runtime_state()));
        lua_settop(fr_lua_runtime_state(), top);
        return FR_ERR;
    }
    lua_settop(fr_lua_runtime_state(), top);

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

typedef struct {
    const char *coordinate;
    const cJSON *block;
    char **out_url;
    char **out_resolved;
    fr_http_headers *out_headers;
} lua_resolver_context;

static lua_resolver_context *resolver_context;

/* lua_next is itself raw, so an __index or __pairs on the returned table is
   never consulted here either. A number key would be coerced to a string by
   reading it, which breaks the traversal, so a non-string key is refused
   rather than read. */
static int read_resolver_headers(lua_State *state, int result, fr_http_headers *out) {
    fr_lua_raw_getfield(state, result, "headers");
    if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        return 0;
    }
    if (!lua_istable(state, -1)) {
        return luaL_error(state, "the resolver returned %s headers, expected a table",
                          luaL_typename(state, -1));
    }

    int table = lua_gettop(state);
    lua_pushnil(state);
    while (lua_next(state, table) != 0) {
        if (lua_type(state, -2) != LUA_TSTRING) {
            return luaL_error(state, "a header name must be a string, not %s",
                              luaL_typename(state, -2));
        }
        if (lua_type(state, -1) != LUA_TSTRING) {
            return luaL_error(state, "the \"%s\" header value must be a string, not %s",
                              lua_tostring(state, -2), luaL_typename(state, -1));
        }
        size_t name_length = 0;
        size_t value_length = 0;
        const char *name = lua_tolstring(state, -2, &name_length);
        const char *value = lua_tolstring(state, -1, &value_length);
        fr_error add_err;
        if (fr_http_headers_add(out, name, name_length, value, value_length, &add_err) != FR_OK) {
            return luaL_error(state, "%s", add_err.message);
        }
        lua_pop(state, 1);
    }
    lua_pop(state, 1);
    return 0;
}

static int protected_resolver_call(lua_State *state) {
    const lua_resolver_context *context = resolver_context;
    lua_rawgeti(state, LUA_REGISTRYINDEX, fr_lua_declare_resolver_callback());
    lua_pushstring(state, context->coordinate);
    fr_error push_err;
    if (fr_lua_push_json(state, context->block, &push_err) != FR_OK) {
        return luaL_error(state, "%s", push_err.message);
    }

    /* lua_call, not lua_pcall: see protected_language_apply above. */
    lua_call(state, 2, 1);

    if (!lua_istable(state, -1)) {
        return luaL_error(state, "the resolver returned %s, expected a table",
                          luaL_typename(state, -1));
    }
    int result = lua_gettop(state);

    fr_lua_raw_getfield(state, result, "url");
    if (lua_type(state, -1) == LUA_TSTRING) {
        *context->out_url = fr_dup_string(lua_tostring(state, -1));
        if (*context->out_url == NULL) return luaL_error(state, "out of memory");
    }
    lua_pop(state, 1);

    fr_lua_raw_getfield(state, result, "resolved");
    if (lua_type(state, -1) == LUA_TSTRING) {
        *context->out_resolved = fr_dup_string(lua_tostring(state, -1));
        if (*context->out_resolved == NULL) return luaL_error(state, "out of memory");
    }
    lua_pop(state, 1);

    return read_resolver_headers(state, result, context->out_headers);
}

int fr_lua_resolver_call(const char *coordinate, const cJSON *block, char **out_url,
                         char **out_resolved, fr_http_headers *out_headers, fr_error *err) {
    *out_url = NULL;
    *out_resolved = NULL;
    out_headers->count = 0;
    if (fr_lua_runtime_state() == NULL) {
        fr_error_set(err, "no lua runtime is open to call a resolver");
        return FR_ERR;
    }
    if (fr_lua_declare_resolver_callback() == LUA_NOREF) {
        fr_error_set(err, "no resolver is declared");
        return FR_ERR;
    }
    int top = lua_gettop(fr_lua_runtime_state());
    lua_resolver_context context = { coordinate, block, out_url, out_resolved, out_headers };
    resolver_context = &context;
    fr_lua_declare_set_resolver_env();
    lua_pushcfunction(fr_lua_runtime_state(), protected_resolver_call);
    int status = lua_pcall(fr_lua_runtime_state(), 0, 0, 0);
    resolver_context = NULL;
    fr_lua_verbs_set_declared_env(NULL, 0);

    if (status != LUA_OK) {
        fr_error_set(err, "%s", fr_lua_error_text(fr_lua_runtime_state()));
        lua_settop(fr_lua_runtime_state(), top);
        free(*out_url);
        *out_url = NULL;
        free(*out_resolved);
        *out_resolved = NULL;
        fr_http_headers_free(out_headers);
        return FR_ERR;
    }
    lua_settop(fr_lua_runtime_state(), top);

    if (*out_url == NULL) {
        free(*out_resolved);
        *out_resolved = NULL;
        fr_http_headers_free(out_headers);
        fr_error_set(err, "the resolver returned no url");
        return FR_ERR;
    }
    return FR_OK;
}

typedef struct {
    fr_lua_plugin_slot *slot;
    const fr_task_run_context *context;
} lua_task_context;

static lua_task_context *task_context;

static int protected_task_run(lua_State *state) {
    const lua_task_context *wrapper = task_context;
    const fr_task_run_context *context = wrapper->context;
    lua_rawgeti(state, LUA_REGISTRYINDEX, wrapper->slot->callback);

    lua_newtable(state);
    lua_pushstring(state, context->name);
    lua_setfield(state, -2, "name");
    lua_pushstring(state, context->project);
    lua_setfield(state, -2, "project");
    lua_pushstring(state, context->version);
    lua_setfield(state, -2, "version");
    lua_pushstring(state, context->root);
    lua_setfield(state, -2, "root");

    lua_newtable(state);
    lua_pushstring(state, fr_lua_host_os());
    lua_setfield(state, -2, "os");
    lua_pushstring(state, fr_lua_host_arch());
    lua_setfield(state, -2, "arch");
    lua_setfield(state, -2, "host");

    lua_newtable(state);
    lua_pushstring(state, context->toolchain->name);
    lua_setfield(state, -2, "name");
    if (context->toolchain->version != NULL) {
        lua_pushstring(state, context->toolchain->version);
        lua_setfield(state, -2, "version");
    }
    lua_newtable(state);
    const cJSON *block = context->toolchain->block;
    if (block != NULL) {
        const cJSON *member = block->child;
        while (member != NULL) {
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
    for (size_t index = 0; index < context->resolved_count; index++) {
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
    lua_setfield(state, -2, "toolchain");

    if (context->publish != NULL) {
        lua_newtable(state);
        lua_pushstring(state, context->publish->name);
        lua_setfield(state, -2, "name");

        lua_newtable(state);
        const cJSON *member = context->publish->block != NULL
            ? context->publish->block->child : NULL;
        while (member != NULL) {
            if (strcmp(member->string, "from") != 0) {
                fr_error push_err;
                if (fr_lua_push_json(state, member, &push_err) != FR_OK) {
                    return luaL_error(state, "%s", push_err.message);
                }
                lua_setfield(state, -2, member->string);
            }
            member = member->next;
        }
        lua_setfield(state, -2, "config");
        lua_setfield(state, -2, "publish");
    }

    /* lua_call, not lua_pcall: see protected_language_apply above. */
    lua_call(state, 1, 0);
    return 0;
}

int fr_lua_dispatch_task_run(void *state, const fr_task_run_context *context, fr_error *err) {
    int top = lua_gettop(fr_lua_runtime_state());
    lua_task_context wrapper = { state, context };
    task_context = &wrapper;
    fr_lua_verbs_set_declared_env(
        (const char *const *) ((const fr_lua_plugin_slot *) state)->env,
        ((const fr_lua_plugin_slot *) state)->env_count);
    fr_lua_set_task_cwd(context->derived_dir_relative);
    lua_pushcfunction(fr_lua_runtime_state(), protected_task_run);
    int status = lua_pcall(fr_lua_runtime_state(), 0, 0, 0);
    fr_lua_set_task_cwd(NULL);
    task_context = NULL;
    fr_lua_verbs_set_declared_env(NULL, 0);

    if (status != LUA_OK) {
        fr_error_set(err, "%s", fr_lua_error_text(fr_lua_runtime_state()));
        lua_settop(fr_lua_runtime_state(), top);
        return FR_ERR;
    }
    lua_settop(fr_lua_runtime_state(), top);
    return FR_OK;
}
