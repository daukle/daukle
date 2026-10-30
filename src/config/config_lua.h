#ifndef DAUKLE_CONFIG_LUA_H
#define DAUKLE_CONFIG_LUA_H

#include "net/http.h"
#include "plugin/plugin_deps.h"
#include "plugin/plugin_modules.h"
#include "plugin/registry.h"

#include "lua.h"

#include <stddef.h>

extern const fr_config_plugin FR_CONFIG_LUA;

/* Every daukle.env read the last loaded script made, name and value, as an
   object, or NULL when no script ran. Owned here and freed by the shutdown
   below, so a caller that needs it past then copies it. */
const struct cJSON *fr_lua_env_reads(void);

/* While on, a load ADDS its reads to the record instead of replacing it, so the
   pin files loaded after an overlay do not erase what the overlay read. */
void fr_lua_env_reads_accumulate(int on);

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
   registration functions and exactly the verbs named in verbs. deps is what
   fr_plugin_deps_acquire produced for this chunk's own declaration, or NULL
   when it declared no requires: it is what daukle.require("<alias>:<module>")
   resolves an alias against, and it is borrowed for the length of the call, so
   the caller closes it only once the load has returned. */
int fr_lua_plugin_load(const char *text, size_t length, const char *origin,
                       const char *const *verbs, size_t verb_count, fr_plugin_source *source,
                       fr_plugin_deps *deps, fr_error *err);

/* The aliases this chunk's daukle.plugin{} declared under "requires", which is
   what a colon-qualified dependsOn or partOf is checked against: a plugin may
   name a task of a toolchain it declares itself or of a plugin it requires,
   and nothing else. Armed around fr_lua_plugin_load and disarmed with
   (NULL, 0) after it, exactly as fr_lua_verbs_set_declared_env is, because
   requires is borrowed from a declaration the loader frees. */
void fr_lua_declare_set_required_aliases(const fr_plugin_requirement *requires, size_t count);

/* The [plugins] label of the entry whose chunk is loading, which is the label a
   migrator or translator declared in that chunk registers under. Armed around
   fr_lua_plugin_load beside the required aliases and disarmed with NULL after
   it. Borrowed: the entry outlives the load. A migrator or translator in a
   module or a required chunk registers under the owning entry's label. */
void fr_lua_declare_set_chunk_label(const char *label);

/* The registry the runtime is currently loading plugins into, or NULL when no
   load phase is open. */
fr_registry *fr_lua_registering_registry(void);

/* True while a plugin chunk is running, so daukle.exec can name that phase in
   its refusal. Generation, see fr_lua_generation_is_running, and a source's
   load or a language's apply, see fr_lua_process_free_callback_phase, are the
   other named windows. None of them is the refusal of record: that is
   fr_lua_task_dispatch_is_running, checked after them. */
int fr_lua_plugin_exec_is_refused(void);

/* daukle.exec and daukle.provision run only while this is true, which is only
   inside fr_lua_dispatch_task_run, a task's run or a publisher's publish, or a
   migrator's run or build, which open the same window. A
   floor rather than a list of refused windows, so code running where nobody
   named a window, as a finalizer once did, is refused by default. D-135.
   enter and leave are public so a verb's unit test can open the window. The
   window also lifts the instruction budget, re-armed fresh when it closes. */
int fr_lua_task_dispatch_is_running(void);
void fr_lua_enter_task_dispatch(void);
void fr_lua_leave_task_dispatch(void);

/* A source's load and a language's apply run on every command, often for a
   dependency the user did not write, so neither may start a process even when
   its chunk's `uses` names exec for a task, publish or migrator callback beside it. A
   depth, not a flag, so a load nested inside another load cannot clear the
   gate when it returns. The phase names the outermost callback. D-133. */
void fr_lua_enter_process_free_callback(const char *phase);
void fr_lua_leave_process_free_callback(void);
const char *fr_lua_process_free_callback_phase(void);

/* True only while a toolchain's generate callback is running inside
   lua_pcall, so daukle.exec and daukle.tool can refuse there: generation
   must stay a pure function of the manifest, or `daukle check` would run
   the user's compiler. */
int fr_lua_generation_is_running(void);

/* Whether the chunk that fr_lua_plugin_load most recently ran declared a
   resolver. */
int fr_lua_resolver_declared(void);

/* Marks the chunk about to run as one being acquired as a resolver, which is
   the only state in which daukle.resolver may be called. Set around the
   fr_lua_plugin_load that runs a [resolvers] entry's own chunk and cleared
   immediately after, so an ordinary plugin chunk can neither install a
   resolver nor replace the one a later entry resolves through. */
void fr_lua_set_acquiring_resolver(int acquiring);

/* Calls the most recently declared resolver's resolve function with
   coordinate and block, and reads url, resolved and headers (if present) from
   the table it returns. Every read of that table is raw, because a plugin
   keeps setmetatable and an __index could otherwise answer for a key the
   resolver never wrote. headers are copied out as opaque strings for the
   fetch to send verbatim; the caller owns them and frees them with
   fr_http_headers_free. The caller adds the label and coordinate to any error
   this raises. */
int fr_lua_resolver_call(const char *coordinate, const struct cJSON *block, char **out_url,
                         char **out_resolved, fr_http_headers *out_headers, fr_error *err);

/* The directory a running task's exec defaults to, or NULL when no task is
   running. This is what discharges the exec verb's cwd default: child spec 2
   claimed to and did not, leaving a child inheriting daukle's own working
   directory. */
const char *fr_lua_task_cwd(void);
void fr_lua_set_task_cwd(const char *directory);

/* The committed generated directory for the running task, or NULL outside
   one. Separate from the task cwd because a tool's cwd must stay the derived
   directory: only daukle.write{ committed = true } looks here. */
const char *fr_lua_task_generated_dir(void);
void fr_lua_set_task_generated_dir(const char *directory);

/* Sets language, source, toolchain, task, resolver and plugin on the table on
   top of the stack, for lua_verbs.c to build a plugin environment around; the
   underlying functions are file statics here, so this is their only way out. */
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
