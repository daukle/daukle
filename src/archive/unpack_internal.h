#ifndef DAUKLE_UNPACK_INTERNAL_H
#define DAUKLE_UNPACK_INTERNAL_H

#include "archive/unpack.h"

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

int fr_unpack_refuse_unsafe_link_target(const fr_archive_member *member, fr_error *err);
int fr_unpack_native_root_of(const char *destination, native_root *out, fr_error *err);
int fr_unpack_join_under_root(const native_root *root, const char *relative, char *out,
                              size_t out_size, fr_error *err);
int fr_unpack_make_directory_tree(char *native, size_t root_length, const char *relative,
                                  fr_unpack_report *report, fr_error *err);
int fr_unpack_make_parent_directories(char *native, size_t root_length, const char *relative,
                                      fr_unpack_report *report, fr_error *err);
int fr_unpack_apply_permissions(const char *native, const fr_archive_member *member, fr_error *err);
int fr_unpack_write_file_bytes(const char *native, const char *bytes, size_t length,
                               const char *relative, fr_error *err);

#endif
