#ifndef DAUKLE_LUA_VERBS_H
#define DAUKLE_LUA_VERBS_H

#include "types.h"

#include "lua.h"

#include <stddef.h>

/* Pushes one environment table for a single plugin chunk: the curated base
   names, plus a daukle table holding the registration functions and exactly the
   verbs named in verbs. Reading any other daukle.* name raises an error naming
   it, so a plugin that forgot to declare a verb fails where it asked rather
   than where the nil was finally called.

   daukle.json_set(text, path, key, value) takes three forms by the type of
   value: a string writes it, nil removes key from path, and a table of
   strings writes a json array in the order given, refusing a non-string
   element by naming its position.

   daukle.json_parse(text) reads a plugin's own json data file, such as a
   package.json, and is the only way into json: daukle.parse(text, file_name)
   dispatches on the extension through the config table, which holds toml and
   lua alone, so it refuses json rather than treating it as a manifest. */
int fr_lua_verbs_push_env(lua_State *state, const char *const *verbs, size_t verb_count,
                          fr_error *err);

/* Every verb name daukle understands, for validating a uses list before the
   environment is built. Returns 1 for a known name. */
int fr_lua_verbs_is_known(const char *name);

/* Whether the environment most recently built by fr_lua_verbs_push_env included
   exec. Reset at the top of every push_env call, so it can never carry a stale
   answer from a previously loaded plugin's chunk. */
int fr_lua_verbs_env_declared_exec(void);

/* Whether the environment just pushed installed daukle.tool. Only
   daukle.resolver consults it: tool is otherwise unrestricted, and a language
   plugin declaring it without exec still loads. A resolver turns a string into
   a url, and daukle.tool's version probe starts a process to answer. */
int fr_lua_verbs_env_declared_tool(void);

/* A known name that this version does not implement, such as publish. */
int fr_lua_verbs_is_reserved(const char *name);

/* Threads --verbose into daukle.exec's own reporting, the way fr_cache_set_enabled
   threads --no-cache into the cache: main.c is the one place that reads the CLI
   flag, and lua_verbs.c has no other way to reach it. */
void fr_lua_verbs_set_verbose(int enabled);

#endif
