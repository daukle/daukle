#ifndef DAUKLE_ARCHIVE_H
#define DAUKLE_ARCHIVE_H

#include "types.h"

#include <stddef.h>

#define FR_ARCHIVE_MAX_NAME 255

typedef enum { FR_ARCHIVE_TAR, FR_ARCHIVE_TAR_GZ, FR_ARCHIVE_ZIP } fr_archive_kind;
typedef enum { FR_MEMBER_FILE, FR_MEMBER_DIRECTORY, FR_MEMBER_SYMLINK } fr_member_kind;

/* Everything here is borrowed and stays valid only until the next
   fr_archive_next, because one member at a time is the point: the iterator
   holds a single buffer and never the whole tree. */
typedef struct {
    const char *name;
    fr_member_kind kind;
    const char *bytes;
    size_t length;
    const char *link_target;
    int executable;
} fr_archive_member;

typedef struct fr_archive fr_archive;

/* What the bytes are, never what a download called itself: a name travels with
   the attacker and the first two bytes do not. */
int fr_archive_kind_of(const char *bytes, size_t length, fr_archive_kind *out, fr_error *err);

/* max_member_bytes sizes the one buffer every member is read into, so the cap
   is agreed before a member can be extracted rather than measured after. */
int fr_archive_open(const char *path, size_t max_member_bytes, fr_archive **out, fr_error *err);

/* FR_OK with *out_member set, or FR_OK with *out_member NULL at the end. */
int fr_archive_next(fr_archive *archive, const fr_archive_member **out_member, fr_error *err);

void fr_archive_close(fr_archive *archive);

#endif
