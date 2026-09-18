#ifndef DAUKLE_TOMLEDIT_H
#define DAUKLE_TOMLEDIT_H

#include "types.h"

#include <stddef.h>

/* Edits toml by splicing spans of its text, so comments, ordering and spacing
   outside the edited span survive. jsonedit.c does the same for json and the
   two stay separate: only the span finding differs, and it has nothing in
   common between the two grammars.

   A dependency that is already there has only its version spliced, so passing
   modules for one is an error rather than a silent discard. The text handed
   back is always read again through the toml reader first, so a caller never
   receives something it would be wrong to write. */
int fr_toml_edit_set_dependency(const char *text, const char *consumer_id, const char *project,
                                const char *range, const char *const *modules, size_t module_count,
                                char **out, fr_error *err);

#endif
