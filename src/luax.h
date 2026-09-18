#ifndef DAUKLE_LUAX_H
#define DAUKLE_LUAX_H

#include "types.h"

#include <stddef.h>

#include "lua.h"

struct cJSON;

lua_State *fr_lua_open(size_t memory_limit, fr_error *err);
void fr_lua_close(lua_State *state);
int fr_lua_run(lua_State *state, const char *text, const char *chunk_name, fr_error *err);
int fr_lua_push_json(lua_State *state, const struct cJSON *value, fr_error *err);
int fr_lua_to_json(lua_State *state, int index, struct cJSON **out, fr_error *err);

/* Compiles text under chunk_name, prefixing it with '@' when the caller has not
   already marked it as a source name (with '@' or '='), so every daukle error
   renders as chunk_name:line rather than [string "chunk_name"]:line. Leaves the
   loaded function or the error object on top of the stack, per luaL_loadbuffer. */
int fr_lua_load_named(lua_State *state, const char *text, size_t length, const char *chunk_name);

/* Arms a per-state instruction counter; the state raises its own error once the
   count is spent, which bounds run time independently of the memory cap. */
void fr_lua_set_instruction_limit(lua_State *state, long limit);

#endif
