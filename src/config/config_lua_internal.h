#ifndef DAUKLE_CONFIG_LUA_INTERNAL_H
#define DAUKLE_CONFIG_LUA_INTERNAL_H

#include "plugin/registry.h"
#include "util/error.h"

#include "lua.h"

/* The seam between the runtime in config_lua.c, which owns the one lua state and
   the load phase, and the declaration verbs in config_declare.c, which own the
   plugin slots every declaration fills. Neither file reaches into the other's
   storage: everything that crosses does so through this file. */

/* daukle's own spellings, which are not any vendor's: x86_64 rather than x64,
   and linux for every platform that is not Windows or Apple. */
const char *fr_lua_host_os(void);
const char *fr_lua_host_arch(void);

/* Points every later declaration at registry, or at NULL once the load phase is
   over. */
void fr_lua_declare_set_registry(fr_registry *registry);

/* Frees every capability string the slots own and empties them. The registry
   holding those strings is destroyed first, which is why this is part of the
   runtime's shutdown rather than the registry's. */
void fr_lua_declare_reset(void);

/* Arms the declaration verbs for one plugin chunk and hands back the reference
   that fr_lua_declare_chunk_end needs: a resolver declared by this chunk
   replaces the one a previous chunk left installed, so the previous callback is
   held across the run and restored if the chunk fails. */
int fr_lua_declare_chunk_begin(lua_State *state);

/* Settles what the chunk declared, keeping it when succeeded and restoring
   backup when it did not. */
void fr_lua_declare_chunk_end(lua_State *state, int succeeded, int backup);

/* Puts the two verbs a configuration script may call, language and source, on
   the daukle table on top of the stack. The plugin environment gets the whole
   set through fr_lua_verbs_install_registration instead. */
void fr_lua_declare_install_config_surface(lua_State *state);

#endif
