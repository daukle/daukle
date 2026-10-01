#ifndef DAUKLE_UNPACK_H
#define DAUKLE_UNPACK_H

#include "archive/archive.h"
#include "util/types.h"

#include <stddef.h>

typedef struct {
    size_t max_total_bytes;
    size_t max_members;
    size_t max_member_bytes;
    size_t max_path_length;
} fr_unpack_limits;

extern const fr_unpack_limits FR_UNPACK_DEFAULTS;

typedef struct {
    size_t files_written;
    size_t directories_created;
    /* daukle creates no symlink, so every link member in an archive is counted
       here on every platform. There is deliberately no symlinks_created beside
       it: a counter that can only be zero is a field someone will try to use. */
    size_t symlinks_skipped;
    char first_symlink_skipped[256];
} fr_unpack_report;

/* Whether a member name may be joined to a destination at all. Decided from the
   name's own bytes: nothing here canonicalises or stats, so there is no window
   between the check and the write for a link to appear in. */
int fr_unpack_name_is_safe(const char *name);

/* Writes every member of archive under destination, which the caller creates.
   Fails on the first member it will not write and leaves that member unwritten;
   members already written stay. */
int fr_unpack(fr_archive *archive, const char *destination, const fr_unpack_limits *limits,
              fr_unpack_report *report, fr_error *err);

#endif
