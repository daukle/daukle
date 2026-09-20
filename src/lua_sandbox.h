#ifndef DAUKLE_LUA_SANDBOX_H
#define DAUKLE_LUA_SANDBOX_H

#include "types.h"

#include "lua.h"

/* Replaces the global table with a curated one and publishes daukle.include.
   base_dir bounds every include: its canonical form is resolved once here and
   kept in the state's registry, and every include target is re-resolved and
   checked against it, so a symlink or junction inside base_dir cannot reach
   outside it. *out_canonical, when not NULL, receives that canonical form on
   success and the caller owns it, so a caller recording what the state is
   bounded to records what the sandbox actually used rather than resolving the
   same directory a second time. */
int fr_lua_sandbox_install(lua_State *state, const char *base_dir, char **out_canonical,
                           fr_error *err);

/* The canonical form fr_lua_sandbox_install would bound a state to, exposed so
   a caller owning the runtime's lifetime compares two base directories the way
   the sandbox does rather than comparing the two strings it was handed. The
   caller owns *out. */
int fr_lua_sandbox_canonical_dir(const char *path, char **out, fr_error *err);

/* Resolves relative against the base directory recorded at install time and
   fails if the result escapes it. The caller owns *out_path. */
int fr_lua_sandbox_resolve(lua_State *state, const char *relative, char **out_path, fr_error *err);

#endif
