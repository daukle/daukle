#ifndef DAUKLE_TOOL_H
#define DAUKLE_TOOL_H

#include "types.h"

/* Discovery only: searches the host's executable path. Child spec 4 extends
   this same function with provisioning, so a caller written against it keeps
   working when a tool can also be downloaded. */
int fr_tool_resolve(const char *name, char **out_path, fr_error *err);

#endif
