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

/* The three outcomes a link member can have, kept apart because they mean
   different things to whoever reads the report: created is the ordinary one,
   copied says the tree is larger than the archive and still complete, and
   unresolved says a link is simply not there. Collapsing them into one count
   is what the previous single `symlinks_skipped` did, and it could not tell a
   degraded tree from an intact one. D-57. */
typedef struct {
    size_t files_written;
    size_t directories_created;
    size_t symlinks_created;
    size_t symlinks_copied;
    size_t symlinks_unresolved;
    char first_symlink_unresolved[256];
} fr_unpack_report;

/* Whether a member name may be joined to a destination at all. Decided from the
   name's own bytes: nothing here canonicalises or stats, so there is no window
   between the check and the write for a link to appear in. */
int fr_unpack_name_is_safe(const char *name);

/* Makes every link member take the copy path, as it does on a filesystem with
   no link support. Without it that path runs only where the OS refuses a link,
   which on Windows depends on whether the account may create one, so the
   fallback would be covered by a runner's privilege level rather than by a
   test. */
void fr_unpack_set_links_supported(int supported);

/* Writes every member of archive under destination, which the caller creates.
   Fails on the first member it will not write and leaves that member unwritten;
   members already written stay. */
int fr_unpack(fr_archive *archive, const char *destination, const fr_unpack_limits *limits,
              fr_unpack_report *report, fr_error *err);

#endif
