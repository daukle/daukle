#include "util/version.h"

#ifndef DAUKLE_VERSION
#error "DAUKLE_VERSION is not defined; the build must pass it from project(... VERSION)"
#endif

const char *fr_self_version(void) {
    return DAUKLE_VERSION;
}
