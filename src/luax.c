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

/* lua_tostring yields NULL for an error object that is neither a string nor a
   number, and converts a number one in place, which allocates outside any
   protected frame; neither is safe where an error is being reported. */
const char *fr_lua_error_text(lua_State *state) {
    if (lua_type(state, -1) == LUA_TSTRING) return lua_tostring(state, -1);
    if (lua_type(state, -1) == LUA_TNUMBER) {
        static char number_text[64];
        snprintf(number_text, sizeof number_text, "%.14g", (double) lua_tonumber(state, -1));
        return number_text;
    }
    return "<non-string error>";
}

static int add_traceback(lua_State *state) {
    const char *message = lua_tostring(state, 1);
    luaL_traceback(state, state, message == NULL ? "error" : message, 1);
    return 1;
}

#define FR_LUA_TRACEBACK_MAX 4096
static char last_traceback[FR_LUA_TRACEBACK_MAX];
static int has_traceback = 0;

const char *fr_lua_last_traceback(void) {
    return has_traceback ? last_traceback : NULL;
}

static int protected_openlibs(lua_State *state) {
    luaL_openlibs(state);
    return 0;
}

static void instruction_budget_spent(lua_State *state, lua_Debug *activation) {
    (void) activation;
    luaL_error(state, "this configuration ran for too long and was stopped");
}

void fr_lua_set_instruction_limit(lua_State *state, long limit) {
    lua_sethook(state, instruction_budget_spent, LUA_MASKCOUNT, (int) limit);
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
        fr_error_set(err, "%s", fr_lua_error_text(state));
        lua_close(state);
        free(budget);
        return NULL;
    }
    fr_lua_set_instruction_limit(state, 50000000);
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

int fr_lua_load_named(lua_State *state, const char *text, size_t length, const char *chunk_name) {
    char source_name[FR_LUA_CHUNK_NAME_MAX];
    if (chunk_name[0] != '@' && chunk_name[0] != '=') {
        snprintf(source_name, sizeof source_name, "@%s", chunk_name);
        chunk_name = source_name;
    }
    return luaL_loadbuffer(state, text, length, chunk_name);
}

int fr_lua_run(lua_State *state, const char *text, const char *chunk_name, fr_error *err) {
    if (fr_lua_load_named(state, text, strlen(text), chunk_name) != LUA_OK) {
        fr_error_set(err, "%s", fr_lua_error_text(state));
        has_traceback = 0;
        lua_pop(state, 1);
        return FR_ERR;
    }
    lua_pushcfunction(state, add_traceback);
    lua_insert(state, -2);
    if (lua_pcall(state, 0, 0, -2) != LUA_OK) {
        const char *traceback = fr_lua_error_text(state);
        fr_error_set(err, "%s", traceback);
        snprintf(last_traceback, sizeof last_traceback, "%s", traceback);
        has_traceback = 1;
        lua_pop(state, 2);
        return FR_ERR;
    }
    lua_pop(state, 1);
    has_traceback = 0;
    return FR_OK;
}

/* A manifest reaches depth 7 (root, consumers, an entry, dependencies, a
   project, modules, an entry), so 64 leaves ample room while keeping both
   conversions clear of the C stack and of a table that refers to itself. */
#define FR_LUA_MAX_DEPTH 64

static int too_deep(int depth, fr_error *err) {
    if (depth <= FR_LUA_MAX_DEPTH) return 0;
    fr_error_set(err, "a configuration value nests deeper than %d levels, which is either a"
                      " mistake or a cycle", FR_LUA_MAX_DEPTH);
    return 1;
}

/* Lua promises a C function only LUA_MINSTACK free slots, and every level of
   these conversions keeps a table plus a key or a value live across its
   recursive call, so each level reserves its own before pushing anything. */
static int reserve_slots(lua_State *state, int slots, fr_error *err) {
    if (lua_checkstack(state, slots)) return 1;
    fr_error_set(err, "the lua stack cannot grow to hold this configuration");
    return 0;
}

static int push_json(lua_State *state, const cJSON *value, int depth, fr_error *err) {
    if (too_deep(depth, err)) return FR_ERR;
    if (!reserve_slots(state, 3, err)) return FR_ERR;

    int stack_top = lua_gettop(state);
    if (value == NULL || cJSON_IsNull(value)) {
        lua_pushnil(state);
        /* JSON null becomes nil, removing the key from the table (daukle schema has no nullable keys). */
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
            if (push_json(state, item, depth + 1, err) != FR_OK) {
                /* Restore stack to honor the exactly-one-value contract (zero values on error, one on success). */
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
            if (push_json(state, item, depth + 1, err) != FR_OK) {
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

int fr_lua_push_json(lua_State *state, const cJSON *value, fr_error *err) {
    return push_json(state, value, 0, err);
}

/* An empty table is an object, and a table whose keys are exactly 1..n is an
   array: lua cannot tell the two apart and the schema's arrays are never
   empty at the point this runs. */
static int table_sequence_length(lua_State *state, int index, lua_Integer *count) {
    lua_Integer total = 0;
    lua_Integer max_key = 0;
    lua_pushnil(state);
    while (lua_next(state, index) != 0) {
        if (!lua_isinteger(state, -2) || lua_tointeger(state, -2) < 1) {
            lua_pop(state, 2);
            return 0;
        }
        lua_Integer key = lua_tointeger(state, -2);
        if (key > max_key) max_key = key;
        total++;
        lua_pop(state, 1);
    }
    /* total distinct keys with the largest equal to total forces the set to be exactly 1..total. */
    *count = total;
    return total > 0 && max_key == total;
}

static int to_json(lua_State *state, int index, int depth, cJSON **out, fr_error *err) {
    *out = NULL;
    if (too_deep(depth, err)) return FR_ERR;
    if (!reserve_slots(state, 4, err)) return FR_ERR;
    int absolute = lua_absindex(state, index);

    switch (lua_type(state, absolute)) {
        case LUA_TNIL:
            *out = cJSON_CreateNull();
            break;
        case LUA_TBOOLEAN:
            *out = cJSON_CreateBool(lua_toboolean(state, absolute));
            break;
        case LUA_TNUMBER:
            *out = cJSON_CreateNumber(lua_tonumber(state, absolute));
            break;
        case LUA_TSTRING:
            *out = cJSON_CreateString(lua_tostring(state, absolute));
            break;
        case LUA_TTABLE: {
            lua_Integer count = 0;
            int is_array = table_sequence_length(state, absolute, &count);
            cJSON *container = is_array ? cJSON_CreateArray() : cJSON_CreateObject();
            if (container == NULL) {
                fr_error_set(err, "out of memory reading a lua table");
                return FR_ERR;
            }
            if (is_array) {
                /* Indexed reads in written order: lua_next gives no ordering guarantee. */
                for (lua_Integer position = 1; position <= count; position++) {
                    lua_rawgeti(state, absolute, position);
                    cJSON *value = NULL;
                    int status = to_json(state, -1, depth + 1, &value, err);
                    lua_pop(state, 1);
                    if (status != FR_OK) {
                        cJSON_Delete(container);
                        return FR_ERR;
                    }
                    cJSON_AddItemToArray(container, value);
                }
            } else {
                lua_pushnil(state);
                while (lua_next(state, absolute) != 0) {
                    if (lua_type(state, -2) != LUA_TSTRING) {
                        fr_error_set(err, "a config table key must be a string, found %s",
                                     lua_typename(state, lua_type(state, -2)));
                        lua_pop(state, 2);
                        cJSON_Delete(container);
                        return FR_ERR;
                    }
                    cJSON *value = NULL;
                    if (to_json(state, -1, depth + 1, &value, err) != FR_OK) {
                        lua_pop(state, 2);
                        cJSON_Delete(container);
                        return FR_ERR;
                    }
                    cJSON_AddItemToObject(container, lua_tostring(state, -2), value);
                    lua_pop(state, 1);
                }
            }
            *out = container;
            break;
        }
        default:
            fr_error_set(err, "a config value may not be a %s",
                         lua_typename(state, lua_type(state, absolute)));
            return FR_ERR;
    }

    if (*out == NULL) {
        fr_error_set(err, "out of memory reading a lua value");
        return FR_ERR;
    }
    return FR_OK;
}

int fr_lua_to_json(lua_State *state, int index, cJSON **out, fr_error *err) {
    return to_json(state, index, 0, out, err);
}
