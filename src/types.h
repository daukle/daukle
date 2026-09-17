#ifndef TIESTONE_TYPES_H
#define TIESTONE_TYPES_H

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
    struct cJSON *document;
} fr_manifest;

#endif
