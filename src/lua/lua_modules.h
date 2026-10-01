#ifndef DAUKLE_LUA_MODULES_H
#define DAUKLE_LUA_MODULES_H

#include "plugin/plugin_deps.h"
#include "plugin/plugin_modules.h"

#include "lua.h"

/* lua_require_module is a file static in lua_modules.c; this is lua_verbs.c's
   only way to reach it when building the library environment, and the
   registration table's way to install it as daukle.require. */
lua_CFunction fr_lua_verbs_require_function(void);

/* Arms daukle.require for one plugin chunk: env is the chunk's own environment
   table, which is where a module of that plugin runs, and source and deps are
   what a module name resolves against. Both are borrowed for the length of the
   chunk. */
void fr_lua_modules_open(lua_State *state, int env, fr_plugin_source *source,
                         fr_plugin_deps *deps);

/* Drops everything the chunk's modules held, so that nothing a plugin loaded
   outlives the load it was loaded for. */
void fr_lua_modules_close(lua_State *state);

#endif
