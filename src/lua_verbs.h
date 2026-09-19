#ifndef DAUKLE_LUA_VERBS_H
#define DAUKLE_LUA_VERBS_H

#include "types.h"

#include "lua.h"

#include <stddef.h>

/* Pushes one environment table for a single plugin chunk: the curated base
   names, plus a daukle table holding the registration functions and exactly the
   verbs named in verbs. Reading any other daukle.* name raises an error naming
   it, so a plugin that forgot to declare a verb fails where it asked rather
   than where the nil was finally called. */
int fr_lua_verbs_push_env(lua_State *state, const char *const *verbs, size_t verb_count,
                          fr_error *err);

/* Every verb name daukle understands, for validating a uses list before the
   environment is built. Returns 1 for a known name. */
int fr_lua_verbs_is_known(const char *name);

/* A known name that this version does not implement, such as exec. */
int fr_lua_verbs_is_reserved(const char *name);

#endif
