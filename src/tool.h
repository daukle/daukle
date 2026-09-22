#ifndef DAUKLE_TOOL_H
#define DAUKLE_TOOL_H

#include "types.h"

/* Discovery only: searches the host's executable path and answers with an
   absolute path, which the two exec backends require to agree on what they
   run. Child spec 4 extends this same function with provisioning, so a caller
   written against it keeps working when a tool can also be downloaded.
   A name that resolves only to a Windows batch file fails naming the file,
   because starting one needs cmd.exe and that is the shell section 4.1 bans. */
int fr_tool_resolve(const char *name, char **out_path, fr_error *err);

#endif
