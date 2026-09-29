#ifndef DAUKLE_TOOLREPORT_H
#define DAUKLE_TOOLREPORT_H

#include <stddef.h>

/* label and url are the two things a plugin can make daukle print; digest is
   always core's own, and empty on a row for an installed tool (there is
   nothing to pin an already-installed binary against). */
typedef struct {
    char label[128];
    char url[1024];
    char digest[65];
    int cached;
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
   line can never be mistaken for something the label supplied. */
void fr_toolreport_provisioned(const char *label, const char *url, const char *digest,
                               int cached);

size_t fr_toolreport_row_count(void);
const fr_toolreport_row *fr_toolreport_row_at(size_t index);

#endif
