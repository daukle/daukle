#include "sha256.h"

#include <stdint.h>
#include <string.h>

static uint32_t rotr(uint32_t word, unsigned int n) {
    return (word >> n) | (word << (32 - n));
}

static const uint32_t SHA256_K[64] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
};

static void sha256_compress(uint32_t state[8], const unsigned char block[64]) {
    uint32_t w[64];
    for (unsigned int i = 0; i < 16; i++) {
        w[i] = ((uint32_t) block[i * 4] << 24) | ((uint32_t) block[i * 4 + 1] << 16)
             | ((uint32_t) block[i * 4 + 2] << 8) | (uint32_t) block[i * 4 + 3];
    }
    for (unsigned int i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];

    for (unsigned int i = 0; i < 64; i++) {
        uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + s1 + ch + SHA256_K[i] + w[i];
        uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = s0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

void fr_sha256_hex(const char *data, size_t length, char out_hex[65]) {
    uint32_t state[8] = {
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U
    };

    const unsigned char *bytes = (const unsigned char *) data;
    size_t full_blocks = length / 64;
    for (size_t i = 0; i < full_blocks; i++) {
        sha256_compress(state, bytes + i * 64);
    }

    unsigned char tail[128];
    size_t remaining = length - full_blocks * 64;
    memcpy(tail, bytes + full_blocks * 64, remaining);
    size_t tail_used = remaining;
    tail[tail_used++] = 0x80;

    size_t pad_to = (tail_used <= 56) ? 56 : 120;
    memset(tail + tail_used, 0, pad_to - tail_used);
    tail_used = pad_to;

    uint64_t bit_length = (uint64_t) length * 8;
    for (int i = 0; i < 8; i++) {
        tail[tail_used + (size_t) i] = (unsigned char) (bit_length >> (56 - i * 8));
    }
    tail_used += 8;

    for (size_t i = 0; i < tail_used; i += 64) {
        sha256_compress(state, tail + i);
    }

    static const char hex_digits[] = "0123456789abcdef";
    for (unsigned int i = 0; i < 8; i++) {
        for (unsigned int b = 0; b < 4; b++) {
            unsigned char byte = (unsigned char) (state[i] >> (24 - b * 8));
            out_hex[i * 8 + b * 2] = hex_digits[byte >> 4];
            out_hex[i * 8 + b * 2 + 1] = hex_digits[byte & 0x0fU];
        }
    }
    out_hex[64] = '\0';
}
