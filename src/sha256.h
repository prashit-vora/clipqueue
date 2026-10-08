#ifndef CQ_SHA256_H
#define CQ_SHA256_H
#include <stddef.h>
#include <stdint.h>
typedef struct {
    uint32_t h[8];
    uint64_t count;
    unsigned char buf[64];
    size_t used;
} Sha256;
void sha256_init(Sha256 *s);
void sha256_update(Sha256 *s, const void *p, size_t n);
void sha256_final(Sha256 *s, unsigned char out[32]);
void sha256(const void *p, size_t n, unsigned char out[32]);
#endif
