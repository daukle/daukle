#ifndef DAUKLE_TOOL_H
#define DAUKLE_TOOL_H

#include "types.h"

/* Discovery only: searches the host's executable path and answers with an
   absolute path, which the two exec backends require to agree on what they
   run. Downloading a tool is a separate path entirely, through
   daukle.provision and the root handle it returns; this function never
   fetches anything and stays a pure search-path lookup.
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

/* The absolute form of path, which the caller frees, or NULL when it cannot be
   resolved. Every program path reaches the exec backends through this, because
   a relative one makes the two disagree about what it names: POSIX chdir()s
   into options.cwd before execv(), so the program would resolve against the
   child's new directory, while CreateProcess resolves it against daukle's own.
   Both a PATH entry and a provisioned root (which inherits DAUKLE_CACHE_DIR
   verbatim) may be relative, which is why this is shared rather than local to
   the search. */
char *fr_tool_absolute_path(const char *path);

#endif
