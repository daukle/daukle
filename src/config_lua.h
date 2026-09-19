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
   to registry. Every plugin and the daukle.lua overlay run in it, so their
   capability strings share one lifetime and fr_lua_runtime_shutdown frees them
   together, after the registry holding them is destroyed. */
int fr_lua_runtime_begin(const char *base_dir, fr_registry *registry, fr_error *err);

lua_State *fr_lua_runtime_state(void);

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
