/* SHA-256 (FIPS 180-4). Small, allocation-free, no external dependency. */
#ifndef BG_SHA256_H
#define BG_SHA256_H

#include <stddef.h>
#include <stdint.h>

#define BG_SHA256_LEN 32
#define BG_SHA256_HEX_LEN 64

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t buf[64];
    size_t buflen;
} bg_sha256_ctx;

void bg_sha256_init(bg_sha256_ctx *c);
void bg_sha256_update(bg_sha256_ctx *c, const void *data, size_t len);
void bg_sha256_final(bg_sha256_ctx *c, uint8_t out[BG_SHA256_LEN]);
void bg_sha256(const void *data, size_t len, uint8_t out[BG_SHA256_LEN]);
/* Writes 64 lowercase hex chars plus a terminating NUL (65 bytes). */
void bg_hex(const uint8_t *in, size_t len, char *out);

#endif
