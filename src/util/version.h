#ifndef DAUKLE_SELF_VERSION_H
#define DAUKLE_SELF_VERSION_H

/* The version this binary was built as, as "major.minor.patch".

   @implNote Named for the program rather than the type: fr_version is already
   the parsed-semver struct in util/types.h.

   @implNote The value comes from project(... VERSION) and reaches this
   translation unit as a compile definition, so CMakeLists.txt is the only place
   it is written. A published binary is identified by what this returns, which
   is why it lives in the core library rather than in main.c: main.c has no test
   binary, and a version nothing can assert on is a version nothing checks. */
const char *fr_self_version(void);

#endif
