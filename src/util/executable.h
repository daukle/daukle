#ifndef DAUKLE_EXECUTABLE_H
#define DAUKLE_EXECUTABLE_H

/* The directory holding the running executable, symbolic links resolved, with no trailing
   separator, or NULL when the platform cannot say. The caller frees it. */
char *fr_executable_directory(void);

#endif
