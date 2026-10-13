#ifndef DAUKLE_UNPACK_INTERNAL_H
#define DAUKLE_UNPACK_INTERNAL_H

#include <stddef.h>

/* The seam between unpack.c, which walks an archive and writes each member, and
   the files that hold its path safety and its link handling. Everything that
   crosses between them does so through this one. */

#define UNPACK_MAX_NATIVE 4096

#ifdef _WIN32
#define NATIVE_SEPARATOR '\\'
#else
#define NATIVE_SEPARATOR '/'
#endif

typedef struct {
    char text[UNPACK_MAX_NATIVE];
    size_t length;
} native_root;

/* A link whose create was refused, kept until the stream ends. It cannot be
   resolved where it is found: a link's target may arrive later in the archive
   and may itself be a link, which 9 of xpack clang's 52 are. D-57. */
typedef struct {
    char *name;
    char *target;
} deferred_link;

typedef struct {
    deferred_link *items;
    size_t count;
} deferred_links;

#endif
