/* Exercise malformed PNGs with address/undefined-behavior sanitizers. */
#include "png_hash.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
static uint32_t state = 271828;
static uint32_t next(void) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}
static void be(unsigned char *p, uint32_t n) {
    p[0] = n >> 24;
    p[1] = n >> 16;
    p[2] = n >> 8;
    p[3] = n;
}
static size_t chunk(unsigned char *p, const char *type, const unsigned char *data, size_t n) {
    be(p, (uint32_t)n);
    memcpy(p + 4, type, 4);
    if (n)
        memcpy(p + 8, data, n);
    be(p + 8 + n, (uint32_t)crc32(0, p + 4, (uInt)n + 4));
    return n + 12;
}
int main(void) {
    unsigned char out[32], image[2048], raw[512], packed[1024], header[13];
    for (int trial = 0; trial < 20000; trial++) {
        size_t length = next() % sizeof image;
        for (size_t i = 0; i < length; i++)
            image[i] = (unsigned char)next();
        png_pixel_hash(image, length, out);
        memcpy(image, "\x89PNG\r\n\x1a\n", 8);
        memset(header, 0, sizeof header);
        be(header, 1 + next() % 48);
        be(header + 4, 1 + next() % 48);
        unsigned depths[] = {1, 2, 4, 8, 16};
        header[8] = depths[next() % 5];
        header[9] = next() % 8;
        header[12] = next() % 2;
        size_t n = next() % sizeof raw;
        for (size_t i = 0; i < n; i++)
            raw[i] = (unsigned char)next();
        uLongf packed_len = sizeof packed;
        if (compress(packed, &packed_len, raw, n) != Z_OK)
            return 1;
        size_t pos = 8;
        pos += chunk(image + pos, "IHDR", header, sizeof header);
        pos += chunk(image + pos, "IDAT", packed, packed_len);
        pos += chunk(image + pos, "IEND", NULL, 0);
        png_pixel_hash(image, pos, out);
    }
    return 0;
}
