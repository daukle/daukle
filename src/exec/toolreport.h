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
    size_t symlinks_skipped;
    char first_symlink_skipped[256];
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

/* Adds the links an unpack left out to the row digest already named, and
   writes them to stderr under it. Nothing is printed when count is zero, and a
   count that IS non-zero is never silent: a toolchain missing the links it
   shipped with is a degradation, and an unreported one is the failure mode
   this whole module exists to prevent. */
void fr_toolreport_symlinks_skipped(const char *digest, size_t count, const char *first_name);

size_t fr_toolreport_row_count(void);
const fr_toolreport_row *fr_toolreport_row_at(size_t index);

#endif
