#ifndef DAUKLE_SEMVER_H
#define DAUKLE_SEMVER_H

#include "types.h"

int fr_version_parse(const char *text, fr_version *out, fr_error *err);
int fr_range_parse(const char *text, fr_range *out, fr_error *err);
int fr_range_satisfies(const fr_range *range, const fr_version *version);

#endif
