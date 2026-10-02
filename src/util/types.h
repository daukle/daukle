#ifndef DAUKLE_TYPES_H
#define DAUKLE_TYPES_H

#define FR_OK 0
#define FR_ERR 1

typedef struct { char message[512]; } fr_error;

typedef struct { int major, minor, patch; } fr_version;

typedef enum { FR_RANGE_EXACT, FR_RANGE_CARET, FR_RANGE_TILDE } fr_range_kind;

typedef struct { fr_range_kind kind; fr_version base; } fr_range;

#include <stddef.h>

struct cJSON;

typedef struct {
    char *name;
    char **requires;
    size_t requires_count;
    const struct cJSON *blocks;
} fr_module;

typedef struct {
    char *project;
    fr_version version;
    fr_module *modules;
    size_t module_count;
    struct cJSON *document;
} fr_project;

typedef struct {
    char *project;
    fr_range range;
    char **modules;
    size_t module_count;
} fr_dependency;

typedef struct {
    char *id;
    char *language;
    char *file;
    char *configuration;
    fr_dependency *dependencies;
    size_t dependency_count;
} fr_consumer;

typedef struct {
    char *name;
    char *version;
    const struct cJSON *block;
    fr_dependency *dependencies;
    size_t dependency_count;
} fr_toolchain;

/* A publish destination. block is the [publish.<name>] table, borrowed from the
   manifest document; from names a toolchain the same manifest declares, which is
   what gives a publish task a toolchain and therefore a working directory. */
typedef struct {
    char *name;
    char *from;
    const struct cJSON *block;
} fr_publish_target;

/* The project-level escape hatch: a program a manifest task runs itself. tool
   is a bare name resolved on the host's search path, args is handed over as
   argv with no shell between, and cwd is relative to the project directory and
   NULL for the project directory itself. This is the one command daukle starts
   that is not pinned by digest, which D-53's section 2 takes as its cost. */
typedef struct {
    char *tool;
    char **args;
    size_t arg_count;
    char *cwd;
} fr_task_command;

/* part_of is NULL when the task joins no aggregator. run is NULL unless the
   block carries the escape hatch: a manifest block otherwise carries edges
   only, because a body is Lua and a manifest is data. */
typedef struct {
    char *name;
    char *part_of;
    char **depends_on;
    size_t depends_on_count;
    fr_task_command *run;
} fr_task;

/* path is relative to the toolchain's derived directory and uses forward
   slashes only; text is the file's whole content, because a toolchain produces
   a document rather than editing one. */
typedef struct {
    char *path;
    char *text;
} fr_generated_file;

typedef struct {
    char *project;
    char *kind;
    const struct cJSON *block;
} fr_source;

typedef struct {
    fr_project self;
    fr_source *sources;
    size_t source_count;
    fr_consumer *consumers;
    size_t consumer_count;
    fr_toolchain *toolchains;
    size_t toolchain_count;
    fr_task *tasks;
    size_t task_count;
    fr_publish_target *publishes;
    size_t publish_count;
    struct cJSON *document;
} fr_manifest;

#endif
