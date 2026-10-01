#ifndef DAUKLE_CONFIG_LUA_INTERNAL_H
#define DAUKLE_CONFIG_LUA_INTERNAL_H

#include "config/manifest.h"
#include "plugin/registry.h"
#include "project/tasks.h"
#include "util/error.h"

#include "lua.h"

#include <stddef.h>

/* The seam between the runtime in config_lua.c, which owns the one lua state and
   the load phase, and the declaration verbs in config_declare.c, which own the
   plugin slots every declaration fills. Neither file reaches into the other's
   storage: everything that crosses does so through this file. */

/* One registered plugin: the capability it answers to, the lua reference of
   the callback it registered, and, for a task, what it is part of and what it
   depends on. config_declare.c fills these; config_dispatch.c is handed one as
   the plugin struct's state pointer and calls the callback it names. */
typedef struct {
    char *capability;
    int callback;
    char *part_of;
    char **depends_on;
    size_t depends_on_count;
    int has_run;
    /* The env allowlist the declaring chunk carried, copied because the
       declaration is freed when the load ends while a callback runs long after.
       A per-chunk window was the first shape tried and it was wrong for exactly
       that reason: a source plugin reads its token inside "load", not inside its
       chunk. D-32. */
    char **env;
    size_t env_count;
} fr_lua_plugin_slot;

/* What each registered plugin kind is dispatched through. Every one takes the
   slot above as its state pointer, and each is what config_declare.c stores in
   the registry when a plugin of that kind declares itself. */
int fr_lua_dispatch_language_apply(void *state, const fr_consumer *consumer,
                                  const fr_resolved *resolved, size_t count,
                                  const char *original_text, char **out_text,
                                  fr_error *err);
int fr_lua_dispatch_toolchain_generate(void *state, const fr_toolchain *toolchain,
                                      const char *project, const char *version,
                                      const char *root, const fr_resolved *resolved,
                                      size_t count, fr_generated_file **out_files,
                                      size_t *out_count, fr_error *err);
int fr_lua_dispatch_source_load(void *state, const char *project, const struct cJSON *block,
                                const char *base_dir, fr_project *out, fr_error *err);
int fr_lua_dispatch_task_run(void *state, const fr_task_run_context *context, fr_error *err);

/* Points the env allowlist at whatever the chunk that declared the resolver
   carried. A resolver is held by lua reference rather than in a slot, so it is
   the one dispatch with no struct to read the list off. */
void fr_lua_declare_set_resolver_env(void);

/* The lua reference of the resolver callback the running chunk, or the last
   chunk to declare one, installed. LUA_NOREF when none has been. */
int fr_lua_declare_resolver_callback(void);

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
