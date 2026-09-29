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

/* The refusal a batch file gets, taking the name asked for and the path it
   came to. Shared with a provisioned root's member, which is refused for the
   identical reason, so the two can never drift into saying different things
   about the same prohibition. */
#define FR_TOOL_BATCH_REFUSAL "daukle cannot run a batch file, and \"%s\" resolved to %s"

/* Whether path names something daukle could start: a regular file with execute
   permission where that is what decides, and any openable file on Windows,
   where it is not. Exposed so a member named inside a provisioned root is held
   to the same test as a name found on the search path. */
int fr_tool_is_executable_file(const char *path);

#endif
