#ifndef DAUKLE_SHA256_H
#define DAUKLE_SHA256_H

#include <stddef.h>

/* Writes 64 lowercase hex characters and a terminating NUL. */
void fr_sha256_hex(const char *data, size_t length, char out_hex[65]);

#endif
