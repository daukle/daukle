#ifndef DAUKLE_LUAX_H
#define DAUKLE_LUAX_H

#include "types.h"

#include <stddef.h>

#include "lua.h"

struct cJSON;

lua_State *fr_lua_open(size_t memory_limit, fr_error *err);
void fr_lua_close(lua_State *state);
int fr_lua_run(lua_State *state, const char *text, const char *chunk_name, fr_error *err);

/* Runs text with the table at env_index as its _ENV rather than the state's
   globals, so one state can host several chunks that each see a different set
   of names. env_index must be an absolute index. */
int fr_lua_run_in_env(lua_State *state, const char *text, const char *chunk_name,
                      int env_index, fr_error *err);

/* Pushes exactly one value on success and none on failure, leaving the stack as
   it found it either way. Fails rather than overrunning the lua stack when the
   document nests too deeply. */
int fr_lua_push_json(lua_State *state, const struct cJSON *value, fr_error *err);

/* Reads the value at index without popping it. Uses raw access throughout, so a
   metatable cannot fabricate what daukle reads, and fails rather than recursing
   without end on a table that refers to itself. */
int fr_lua_to_json(lua_State *state, int index, struct cJSON **out, fr_error *err);

/* The text of the error object on top of the stack, never NULL, for a caller
   reporting a failed lua_pcall that had no message handler. */
const char *fr_lua_error_text(lua_State *state);

/* Compiles text under chunk_name, prefixing it with '@' when the caller has not
   already marked it as a source name (with '@' or '='), so every daukle error
   renders as chunk_name:line rather than [string "chunk_name"]:line. Leaves the
   loaded function or the error object on top of the stack, per luaL_loadbuffer. */
int fr_lua_load_named(lua_State *state, const char *text, size_t length, const char *chunk_name);

/* Arms a per-state instruction counter; the state raises its own error once the
   count is spent, which bounds run time independently of the memory cap. */
void fr_lua_set_instruction_limit(lua_State *state, long limit);

/* fr_error's message is capped at 512 bytes, too small for a traceback, so a
   caller that wants the untruncated text from the last fr_lua_run failure asks
   here instead; returns NULL when the last run did not fail or left no
   traceback. */
const char *fr_lua_last_traceback(void);

#endif
