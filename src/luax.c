#include "luax.h"

#include "error.h"
#include "cJSON.h"

#include "lauxlib.h"
#include "lualib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    size_t limit;
    size_t used;
} fr_lua_budget;

static size_t subtract_capped(size_t used, size_t amount) {
    return amount > used ? 0 : used - amount;
}

static void *capped_alloc(void *ud, void *pointer, size_t old_size, size_t new_size) {
    fr_lua_budget *budget = ud;
    /* lua_Alloc: when pointer is NULL, old_size carries a type tag, not a byte count. */
    size_t previous = pointer == NULL ? 0 : old_size;
    if (new_size == 0) {
        free(pointer);
        budget->used = subtract_capped(budget->used, previous);
        return NULL;
    }
    if (new_size > previous && budget->used + (new_size - previous) > budget->limit) return NULL;
    void *moved = realloc(pointer, new_size);
    if (moved == NULL) return NULL;
    if (new_size >= previous) {
        budget->used += new_size - previous;
    } else {
        budget->used = subtract_capped(budget->used, previous - new_size);
    }
    return moved;
}

static int add_traceback(lua_State *state) {
    const char *message = lua_tostring(state, 1);
    luaL_traceback(state, state, message == NULL ? "error" : message, 1);
    return 1;
}

static int protected_openlibs(lua_State *state) {
    luaL_openlibs(state);
    return 0;
}

lua_State *fr_lua_open(size_t memory_limit, fr_error *err) {
    fr_lua_budget *budget = calloc(1, sizeof *budget);
    if (budget == NULL) {
        fr_error_set(err, "out of memory creating the lua budget");
        return NULL;
    }
    budget->limit = memory_limit;
    lua_State *state = lua_newstate(capped_alloc, budget);
    if (state == NULL) {
        free(budget);
        fr_error_set(err, "could not create a lua state");
        return NULL;
    }
    /* lua_newstate's own bootstrap is protected, but nothing protects luaL_openlibs
       once it returns: a budget that is big enough for the state but too small for
       the standard libraries raises LUA_ERRMEM with no panic handler installed
       (that is luaL_newstate's job, not lua_newstate's), which falls through to
       abort(). Run it under our own pcall so a tight cap fails gracefully instead. */
    lua_pushcfunction(state, protected_openlibs);
    if (lua_pcall(state, 0, 0, 0) != LUA_OK) {
        fr_error_set(err, "%s", lua_tostring(state, -1));
        lua_close(state);
        free(budget);
        return NULL;
    }
    return state;
}

void fr_lua_close(lua_State *state) {
    if (state == NULL) return;
    void *budget = NULL;
    lua_getallocf(state, &budget);
    lua_close(state);
    free(budget);
}

/* A chunk name longer than this truncates in error output; Lua's own display
   width (LUA_IDSIZE) is smaller still, so nothing meaningful is lost. */
#define FR_LUA_CHUNK_NAME_MAX 300

int fr_lua_run(lua_State *state, const char *text, const char *chunk_name, fr_error *err) {
    /* Without a '@' or '=' prefix, Lua treats the name as literal source text and
       reports errors as [string "..."], not as chunk_name:line. */
    char source_name[FR_LUA_CHUNK_NAME_MAX];
    if (chunk_name[0] != '@' && chunk_name[0] != '=') {
        snprintf(source_name, sizeof source_name, "@%s", chunk_name);
        chunk_name = source_name;
    }
    if (luaL_loadbuffer(state, text, strlen(text), chunk_name) != LUA_OK) {
        fr_error_set(err, "%s", lua_tostring(state, -1));
        lua_pop(state, 1);
        return FR_ERR;
    }
    lua_pushcfunction(state, add_traceback);
    lua_insert(state, -2);
    if (lua_pcall(state, 0, 0, -2) != LUA_OK) {
        fr_error_set(err, "%s", lua_tostring(state, -1));
        lua_pop(state, 2);
        return FR_ERR;
    }
    lua_pop(state, 1);
    return FR_OK;
}

int fr_lua_push_json(lua_State *state, const cJSON *value, fr_error *err) {
    int stack_top = lua_gettop(state);
    if (value == NULL || cJSON_IsNull(value)) {
        lua_pushnil(state);
        return FR_OK;
    }
    if (cJSON_IsBool(value)) {
        lua_pushboolean(state, cJSON_IsTrue(value));
        return FR_OK;
    }
    if (cJSON_IsNumber(value)) {
        lua_pushnumber(state, value->valuedouble);
        return FR_OK;
    }
    if (cJSON_IsString(value)) {
        lua_pushstring(state, value->valuestring);
        return FR_OK;
    }
    if (cJSON_IsArray(value)) {
        lua_newtable(state);
        int position = 1;
        const cJSON *item = NULL;
        cJSON_ArrayForEach(item, value) {
            if (fr_lua_push_json(state, item, err) != FR_OK) {
                lua_settop(state, stack_top);
                return FR_ERR;
            }
            lua_rawseti(state, -2, position++);
        }
        return FR_OK;
    }
    if (cJSON_IsObject(value)) {
        lua_newtable(state);
        const cJSON *item = NULL;
        cJSON_ArrayForEach(item, value) {
            if (fr_lua_push_json(state, item, err) != FR_OK) {
                lua_settop(state, stack_top);
                return FR_ERR;
            }
            lua_setfield(state, -2, item->string);
        }
        return FR_OK;
    }
    fr_error_set(err, "cannot represent a json value of an unknown type in lua");
    lua_settop(state, stack_top);
    return FR_ERR;
}

