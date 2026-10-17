#ifndef DAUKLE_TOOLCHAIN_ROOT_H
#define DAUKLE_TOOLCHAIN_ROOT_H

#include "util/types.h"

/* The project directory the toolchain builds, as seen from its derived
   directory: FR_DERIVED_ROOT_RELATIVE, or that joined with the toolchain's dir.
   Generation and a task both hand this to a plugin as "root", so it is computed
   in one place. *out_root is the caller's to free. */
int fr_toolchain_root(const fr_toolchain *toolchain, char **out_root, fr_error *err);

#endif
