#ifndef DAUKLE_LUAX_H
#define DAUKLE_LUAX_H

#include "types.h"

#include <stddef.h>

#include "lua.h"

lua_State *fr_lua_open(size_t memory_limit, fr_error *err);
void fr_lua_close(lua_State *state);
int fr_lua_run(lua_State *state, const char *text, const char *chunk_name, fr_error *err);

#endif
