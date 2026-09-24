#ifndef DAUKLE_SHA256_H
#define DAUKLE_SHA256_H

#include <stddef.h>

/* Writes 64 lowercase hex characters and a terminating NUL. */
void fr_sha256_hex(const char *data, size_t length, char out_hex[65]);

/* Whether two hex digests name the same bytes. A pin may be written in either
   case and must still match, and stricmp/strcasecmp are not portable C11, so
   the comparison lives beside the function that produces the digests rather
   than as a copy in each module that checks one. */
int fr_sha256_hex_equal(const char *left, const char *right);

#endif
