#ifndef DAUKLE_LUA_SANDBOX_H
#define DAUKLE_LUA_SANDBOX_H

#include "types.h"

#include "lua.h"

/* Replaces the global table with a curated one and publishes daukle.include.
   base_dir bounds every include, and is copied into the state's registry. */
int fr_lua_sandbox_install(lua_State *state, const char *base_dir, fr_error *err);

#endif
