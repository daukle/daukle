#ifndef DAUKLE_TOOLREPORT_H
#define DAUKLE_TOOLREPORT_H

#include <stddef.h>

/* label and url are the two things a plugin can make daukle print; digest is
   always core's own, and empty on a row for an installed tool (there is
   nothing to pin an already-installed binary against). provisioned tells a
   renderer which of those two cases it has: an empty digest alone cannot,
   since it is also what a provisioned row would show if its digest were
   somehow blank, and a report that cannot tell "installed" from
   "provisioned, digest missing" defeats the reason it exists. */
typedef struct {
    char label[128];
    char url[1024];
    char digest[65];
    int cached;
    int provisioned;
    size_t symlinks_copied;
    size_t symlinks_unresolved;
    char first_symlink_unresolved[256];
} fr_toolreport_row;

/* Refuses any control character, including every ANSI escape: a label is
   plugin-supplied text written to a terminal, and one that can reposition
   the cursor can erase the facts printed beside it. */
int fr_toolreport_label_is_safe(const char *label);

void fr_toolreport_reset(void);

/* label may be NULL, in which case the tool's own name is used. Writes
   "using <label> (<path>)" to stderr unconditionally, since a divergence
   between two machines is exactly what this module exists to keep visible. */
void fr_toolreport_used_installed(const char *name, const char *label, const char *path);

/* label may be NULL, in which case the url's last path component is used.
   Writes "provisioning <label> (cached|downloaded)" to stderr, followed by
   the url and "sha256 <digest>" on their own indented lines: the label
   always comes first and the facts always come last, so core's half of the
   line can never be mistaken for something the label supplied.

   @implNote a cached row also says the symlinks were not examined. Silence
   below a provisioned row otherwise means two different things: "unpacked,
   nothing was skipped" on a fresh provision, and "nothing was unpacked, so
   nobody looked" on a cache hit. Since D-38 daukle creates no symlink on any
   platform, so the cached tree really can lack them and the first run is the
   only one that ever said so. D-43. */
void fr_toolreport_provisioned(const char *label, const char *url, const char *digest,
                               int cached);

/* The same acquisition row for a pinned file that is kept rather than unpacked,
   with no symlink clause: there is no tree to have left links out of, so the
   sentence above would be a confident statement about something that does not
   exist. D-52. */
void fr_toolreport_artifact(const char *label, const char *url, const char *digest, int cached);

/* Adds what an unpack did with the archive's links to the row digest already
   named, and writes it to stderr under that row. Silent when the tree came out
   exactly as the archive declared it, which since D-57 is the ordinary case on
   a filesystem with links.

   The two counts say different things and neither implies the other. copied
   means the tree is COMPLETE and larger than the archive, because the platform
   refused a link and the target's bytes were written in its place. unresolved
   means a link is simply ABSENT, which is a degradation and is never silent:
   a toolchain missing the links it shipped with is the failure mode this whole
   module exists to prevent. */
void fr_toolreport_links(const char *digest, size_t copied, size_t unresolved,
                         const char *first_unresolved);

size_t fr_toolreport_row_count(void);
const fr_toolreport_row *fr_toolreport_row_at(size_t index);

#endif
