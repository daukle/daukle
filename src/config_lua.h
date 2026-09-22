#ifndef DAUKLE_CONFIG_LUA_H
#define DAUKLE_CONFIG_LUA_H

#include "registry.h"

#include "lua.h"

#include <stddef.h>

extern const fr_config_plugin FR_CONFIG_LUA;

/* Every daukle.env read the last loaded script made, name and value, as an
   object, or NULL when no script ran. Owned here and freed by the shutdown
   below, so a caller that needs it past then copies it. */
const struct cJSON *fr_lua_env_reads(void);

/* Opens the one state this load phase shares, or confirms the open one belongs
   to registry and is already bounded to base_dir. Every plugin and the
   daukle.lua overlay run in it, so their capability strings share one lifetime
   and fr_lua_runtime_shutdown frees them together, after the registry holding
   them is destroyed. A second base_dir is refused rather than ignored: the
   sandbox is installed once, so reusing the state would silently bound
   daukle.read to the first caller's directory. */
int fr_lua_runtime_begin(const char *base_dir, fr_registry *registry, fr_error *err);

lua_State *fr_lua_runtime_state(void);

/* Runs one plugin chunk in the shared state, in an environment holding the
   registration functions and exactly the verbs named in verbs. */
int fr_lua_plugin_load(const char *text, const char *origin, const char *const *verbs,
                       size_t verb_count, fr_error *err);

/* The registry the runtime is currently loading plugins into, or NULL when no
   load phase is open. */
fr_registry *fr_lua_registering_registry(void);

/* True while a plugin chunk is running, which is exactly when daukle.exec must
   refuse: the checks in daukle.language and daukle.source fire only once a
   plugin declares its kind, and a plugin that execs at the top of its chunk has
   already run the program by then. No plugin kind that may exec exists yet, so
   the whole of a plugin chunk is refused; when toolchain plugins land this
   narrows to the kinds that may not, and the declaration-time checks stay as
   the place spec section 4.4's message is raised. */
int fr_lua_plugin_exec_is_refused(void);

/* Sets language, source and plugin on the table on top of the stack, for
   lua_verbs.c to build a plugin environment around; the underlying functions
   are file statics here, so this is their only way out. */
void fr_lua_verbs_install_registration(lua_State *state);

/* The registry stores plugin structs by value and does not own their capability
   strings, so the runtime that owns them outlives the registry and is torn down
   only after it. */
void fr_lua_runtime_shutdown(void);

/* NULL (the default) discards daukle.log messages; library code never prints,
   so the caller supplies a sink if it wants them surfaced. */
void fr_lua_set_log_sink(void (*sink)(const char *message));

/* Either limit at 0 keeps config_lua_load's own default (64 MiB, 50,000,000
   instructions); a non-zero value overrides it for every state opened after
   this call, until it is called again. */
void fr_lua_set_limits(long instruction_limit, size_t memory_limit);

#endif
