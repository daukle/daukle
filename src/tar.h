#ifndef DAUKLE_TAR_H
#define DAUKLE_TAR_H

#include "types.h"

#include <stddef.h>

#define FR_TAR_BLOCK 512
#define FR_TAR_MAX_MEMBERS 64
#define FR_TAR_MAX_MEMBER_BYTES (1024u * 1024u)
#define FR_TAR_MAX_NAME 96

/* A member's bytes point INTO the buffer fr_tar_read was given, which must
   outlive the fr_tar. Nothing in this module allocates, so nothing frees. */
typedef struct {
    char name[FR_TAR_MAX_NAME + 1];
    const char *bytes;
    size_t length;
} fr_tar_member;

typedef struct {
    fr_tar_member members[FR_TAR_MAX_MEMBERS];
    size_t count;
} fr_tar;

/* Whether bytes open with a ustar header whose checksum validates. A plugin
   artifact arrives with no content type, so this is what tells an archive from
   a lua chunk; the checksum is part of the test because the magic alone would
   match a chunk that happened to contain the word. */
int fr_tar_looks_like_archive(const char *bytes, size_t length);

/* Reads the accepted subset: regular files only, octal sizes, no prefix field,
   no extension record. A directory entry is skipped rather than refused, since
   an ordinary producer writes them for a tree and nothing here creates one.
   Anything else is refused naming what was found. */
int fr_tar_read(const char *bytes, size_t length, fr_tar *out, fr_error *err);

const fr_tar_member *fr_tar_find(const fr_tar *archive, const char *name);

#endif
