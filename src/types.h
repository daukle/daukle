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

/* part_of is NULL when the task joins no aggregator. A manifest block carries
   edges only: it never carries a body, because a body is Lua and a manifest
   is data. */
typedef struct {
    char *name;
    char *part_of;
    char **depends_on;
    size_t depends_on_count;
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
    struct cJSON *document;
} fr_manifest;

#endif
