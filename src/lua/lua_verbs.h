#ifndef DAUKLE_LUA_VERBS_H
#define DAUKLE_LUA_VERBS_H

#include "util/types.h"

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

/* BASE plus daukle.require and nothing else: what a module reached across a
   plugin boundary runs in. The obvious alternative, push_env with no verbs,
   is NOT this: protected_push_env installs the registration functions
   unconditionally, so a library built that way could declare a toolchain.
   dependent and member name the boundary in the message an absent name raises. */
int fr_lua_verbs_push_library_env(lua_State *state, const char *dependent, const char *member,
                                  fr_error *err);

/* Every verb name daukle understands, for validating a uses list before the
   environment is built. Returns 1 for a known name. */
/* The environment variables the chunk about to run declared, which is both the
   set daukle.env may read of core's own and the set a child it starts inherits
   of them. Borrowed for the length of the chunk; the caller clears it with
   (NULL, 0) afterwards. Governing reading as well as inheritance is what makes
   it a control: a plugin that could read the token would otherwise hand it to a
   child as an ordinary addition, which no filter on inheritance sees. D-32. */
void fr_lua_verbs_set_declared_env(const char *const *names, size_t count);

/* Copies the declared list so a registered plugin can keep it past the chunk,
   which is where every callback that needs it actually runs. Returns 0, or -1
   out of memory with nothing allocated. */
int fr_lua_verbs_copy_declared_env(char ***out, size_t *out_count);

/* Whether name is one core itself reads, and so one a plugin must declare
   before it may see it. daukle cannot know which of the USER's variables are
   secret and does not guess; it knows these because it asks for them by name. */
int fr_lua_verbs_is_core_credential(const char *name);

/* Whether the running chunk declared name in env. */
int fr_lua_verbs_env_is_declared(const char *name);

#define FR_LUA_VERBS_MAX_SCRUB 8

/* Every core credential the running chunk did NOT declare, which is what a
   child it starts must not inherit. Writes up to limit pointers into out and
   returns how many; the strings are static. */
size_t fr_lua_verbs_env_to_scrub(const char **out, size_t limit);

/* Every core credential, regardless of what any chunk declared, for a caller
   that runs outside one. A manifest task's run is such a caller, and asking
   fr_lua_verbs_env_to_scrub there would read whichever plugin chunk happened
   to set the declared list last. */
size_t fr_lua_verbs_core_credentials(const char **out, size_t limit);

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

/* Whether the environment just pushed installed daukle.provision, for the
   caller that decides which plugin kinds may fetch a toolchain at all. */
int fr_lua_verbs_env_declared_provision(void);

/* Whether the environment just pushed installed daukle.artifact, read by the
   same caller and restricted to the same plugin kinds: fetching a pinned file
   onto disk and naming it to a tool is provision's class of power, and a
   capability is easier to widen later than to narrow. */
int fr_lua_verbs_env_declared_artifact(void);

/* A known name that this version does not implement, such as publish. */
int fr_lua_verbs_is_reserved(const char *name);

/* Threads --verbose into daukle.exec's own reporting, the way fr_cache_set_enabled
   threads --no-cache into the cache: main.c is the one place that reads the CLI
   flag, and lua_verbs.c has no other way to reach it. */
void fr_lua_verbs_set_verbose(int enabled);

/* Opens daukle.pin for this run, threaded from --resolve the same way verbose
   is. It is off unless the flag was given, so the unpinned fetch is reachable
   only when the command line asked for it. D-77. */
void fr_lua_verbs_set_resolving(int enabled);

#endif
