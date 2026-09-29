#ifndef DAUKLE_SHA256_H
#define DAUKLE_SHA256_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t state[8];
    uint64_t total_length;
    unsigned char block[64];
    size_t pending;
} fr_sha256;

void fr_sha256_init(fr_sha256 *context);
void fr_sha256_update(fr_sha256 *context, const void *data, size_t length);
void fr_sha256_final(fr_sha256 *context, char out_hex[65]);

/* Writes 64 lowercase hex characters and a terminating NUL. */
void fr_sha256_hex(const char *data, size_t length, char out_hex[65]);

/* Whether two hex digests name the same bytes. A pin may be written in either
   case and must still match, and stricmp/strcasecmp are not portable C11, so
   the comparison lives beside the function that produces the digests rather
   than as a copy in each module that checks one. */
int fr_sha256_hex_equal(const char *left, const char *right);

#endif
