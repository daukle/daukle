#ifndef DAUKLE_CONFIG_DISPATCH_MIGRATE_H
#define DAUKLE_CONFIG_DISPATCH_MIGRATE_H

#include "plugin/registry.h"
#include "util/error.h"

struct cJSON;

/* The context a migrator's callbacks receive, built by migrate and borrowed for
   the call. proof is NULL for run and the proof run returned for build.
   manifest_dir is the sandbox base the derived directory is made under. */
typedef struct {
    const char *manifest_dir;
    const char *tool;
    const char *instance;      /* the toolchain name migrate assigned */
    const char *dir;           /* NULL for the project root */
    const char *root;          /* fr_toolchain_root's answer for that instance */
    const char *derived_dir_relative;  /* "build/daukle/<instance>", exec's default cwd */
    const struct cJSON *proof;
} fr_migrate_call;

/* Calls run and hands back what it returned, converted to JSON and owned by the
   caller. The callback runs inside the task window, so exec and provision work. */
int fr_lua_dispatch_migrator_run(const fr_migrator_plugin *migrator, const fr_migrate_call *call,
                                 struct cJSON **out_result, fr_error *err);

/* Calls build, or does nothing and answers FR_OK when the migrator declared none. */
int fr_lua_dispatch_migrator_build(const fr_migrator_plugin *migrator, const fr_migrate_call *call,
                                   fr_error *err);

/* Calls translate with the foreign object ({ tool, kind, id, config, instance })
   and hands back what it returned. Runs as a process-free callback: exec and
   provision are refused by name, as they are for a source's load. */
int fr_lua_dispatch_translator(const fr_translator_plugin *translator, const struct cJSON *foreign,
                               struct cJSON **out_result, fr_error *err);

#endif
