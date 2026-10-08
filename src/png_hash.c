/* Bounded PNG sample decoder for clipboard identity, from the W3C PNG spec.
 * Original PNG bytes are retained for pasting; this never re-encodes images. */
#include "png_hash.h"
#include "sha256.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#define LIMIT (128UL * 1024 * 1024)
#define PIXEL_LIMIT (16UL * 1024 * 1024)
static uint32_t be32(const unsigned char *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static unsigned be16(const unsigned char *p) {
    return (unsigned)p[0] << 8 | p[1];
}
static unsigned sample(const unsigned char *p, size_t index, unsigned depth) {
    if (depth == 16)
        return be16(p + 2 * index);
    if (depth == 8)
        return p[index];
    size_t bit = index * depth;
    return (p[bit / 8] >> (8 - depth - (bit % 8))) & ((1U << depth) - 1);
}
static unsigned paeth(unsigned a, unsigned b, unsigned c) {
    int p = (int)a + (int)b - (int)c;
    int pa = abs(p - (int)a), pb = abs(p - (int)b), pc = abs(p - (int)c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}
static void put16(unsigned char *p, unsigned x) {
    p[0] = (unsigned char)(x >> 8);
    p[1] = (unsigned char)x;
}
int png_pixel_hash(const unsigned char *png, size_t length, unsigned char out[32]) {
    if (length < 33 || length > 64UL * 1024 * 1024 || memcmp(png, "\x89PNG\r\n\x1a\n", 8))
        return 0;
    uint32_t width = 0, height = 0;
    unsigned depth = 0, color = 0, interlace = 0, channels = 0;
    const unsigned char *palette = NULL, *trans = NULL;
    size_t palette_len = 0, trans_len = 0, compressed_len = 0;
    int ihdr = 0, iend = 0, seen_data = 0, data_ended = 0;
    unsigned char *compressed = NULL, *raw = NULL, *pixels = NULL;
    int ok = 0;
    for (size_t pos = 8; pos + 12 <= length;) {
        size_t n = be32(png + pos);
        if (n > length - pos - 12)
            return 0;
        const unsigned char *type = png + pos + 4, *p = png + pos + 8;
        uLong crc = crc32(0L, Z_NULL, 0);
        crc = crc32(crc, type, (uInt)n + 4);
        if ((uint32_t)crc != be32(p + n))
            return 0;
        if (!memcmp(type, "IHDR", 4)) {
            if (ihdr || pos != 8 || n != 13)
                return 0;
            width = be32(p);
            height = be32(p + 4);
            depth = p[8];
            color = p[9];
            interlace = p[12];
            ihdr = 1;
            if (!width || !height || width > 32768 || height > 32768 ||
                (uint64_t)width * height > PIXEL_LIMIT || p[10] || p[11] || interlace > 1)
                return 0;
            channels = color == 0   ? 1
                       : color == 2 ? 3
                       : color == 3 ? 1
                       : color == 4 ? 2
                       : color == 6 ? 4
                                    : 0;
            if (!channels)
                return 0;
            if (color == 3) {
                if (depth != 1 && depth != 2 && depth != 4 && depth != 8)
                    return 0;
            } else if (color == 0) {
                if (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16)
                    return 0;
            } else if (depth != 8 && depth != 16)
                return 0;
        } else if (!ihdr)
            return 0;
        else if (!memcmp(type, "PLTE", 4)) {
            if (palette || seen_data || !n || n > 768 || n % 3)
                return 0;
            palette = p;
            palette_len = n;
        } else if (!memcmp(type, "tRNS", 4)) {
            if (trans || seen_data)
                return 0;
            trans = p;
            trans_len = n;
        } else if (!memcmp(type, "IDAT", 4)) {
            if (data_ended || n > LIMIT - compressed_len)
                return 0;
            compressed_len += n;
            seen_data = 1;
        } else if (!memcmp(type, "IEND", 4)) {
            if (n || !seen_data)
                return 0;
            iend = 1;
            break;
        } else {
            if (!memcmp(type, "acTL", 4))
                return 0;
            if (!(type[0] & 32))
                return 0;
        }
        if (seen_data && memcmp(type, "IDAT", 4))
            data_ended = 1;
        pos += n + 12;
    }
    if (!iend || !compressed_len || (color == 3 && !palette))
        return 0;
    if (trans && ((color == 0 && trans_len != 2) || (color == 2 && trans_len != 6) ||
                  (color == 3 && trans_len > palette_len / 3) || color == 4 || color == 6))
        return 0;
    static const unsigned xs[7] = {0, 4, 0, 2, 0, 1, 0}, ys[7] = {0, 0, 4, 0, 2, 0, 1},
                          dx[7] = {8, 8, 4, 4, 2, 2, 1}, dy[7] = {8, 8, 8, 4, 4, 2, 2};
    unsigned passes = interlace ? 7 : 1;
    size_t raw_len = 0;
    for (unsigned pass = 0; pass < passes; pass++) {
        unsigned x0 = interlace ? xs[pass] : 0, y0 = interlace ? ys[pass] : 0,
                 sx = interlace ? dx[pass] : 1, sy = interlace ? dy[pass] : 1;
        size_t pw = width > x0 ? (width - x0 + sx - 1) / sx : 0,
               ph = height > y0 ? (height - y0 + sy - 1) / sy : 0;
        if (!pw || !ph)
            continue;
        size_t row = (pw * channels * depth + 7) / 8;
        if (row + 1 > LIMIT / ph || (row + 1) * ph > LIMIT - raw_len)
            return 0;
        raw_len += (row + 1) * ph;
    }
    compressed = malloc(compressed_len);
    raw = malloc(raw_len);
    pixels = calloc((size_t)width * height, 8);
    if (!compressed || !raw || !pixels)
        goto done;
    size_t offset = 0;
    for (size_t pos = 8; pos + 12 <= length;) {
        size_t n = be32(png + pos);
        if (!memcmp(png + pos + 4, "IDAT", 4)) {
            memcpy(compressed + offset, png + pos + 8, n);
            offset += n;
        }
        if (!memcmp(png + pos + 4, "IEND", 4))
            break;
        pos += n + 12;
    }
    uLongf dest_len = raw_len;
    if (uncompress(raw, &dest_len, compressed, compressed_len) != Z_OK || dest_len != raw_len)
        goto done;
    free(compressed);
    compressed = NULL;
    offset = 0;
    for (unsigned pass = 0; pass < passes; pass++) {
        unsigned x0 = interlace ? xs[pass] : 0, y0 = interlace ? ys[pass] : 0,
                 sx = interlace ? dx[pass] : 1, sy = interlace ? dy[pass] : 1;
        size_t pw = width > x0 ? (width - x0 + sx - 1) / sx : 0,
               ph = height > y0 ? (height - y0 + sy - 1) / sy : 0;
        if (!pw || !ph)
            continue;
        size_t row_len = (pw * channels * depth + 7) / 8, bpp = (channels * depth + 7) / 8;
        unsigned char *prev = NULL;
        for (size_t y = 0; y < ph; y++) {
            unsigned filter = raw[offset++];
            unsigned char *row = raw + offset;
            offset += row_len;
            if (filter > 4)
                goto done;
            for (size_t i = 0; i < row_len; i++) {
                unsigned a = i >= bpp ? row[i - bpp] : 0, b = prev ? prev[i] : 0,
                         c = prev && i >= bpp ? prev[i - bpp] : 0;
                unsigned pred = filter == 0   ? 0
                                : filter == 1 ? a
                                : filter == 2 ? b
                                : filter == 3 ? (a + b) / 2
                                              : paeth(a, b, c);
                row[i] = (unsigned char)(row[i] + pred);
            }
            for (size_t x = 0; x < pw; x++) {
                unsigned r = 0, g = 0, b = 0, a = 65535,
                         max = depth == 16 ? 65535 : (1U << depth) - 1;
                size_t at = x * channels;
                if (color == 3) {
                    unsigned ix = sample(row, x, depth);
                    if (ix >= palette_len / 3)
                        goto done;
                    r = palette[ix * 3] * 257U;
                    g = palette[ix * 3 + 1] * 257U;
                    b = palette[ix * 3 + 2] * 257U;
                    if (ix < trans_len)
                        a = trans[ix] * 257U;
                } else if (color == 0 || color == 4) {
                    unsigned v = sample(row, at, depth);
                    r = g = b = v * 65535U / max;
                    if (color == 4)
                        a = sample(row, at + 1, depth) * 65535U / max;
                    else if (trans && v == be16(trans))
                        a = 0;
                } else {
                    unsigned vr = sample(row, at, depth), vg = sample(row, at + 1, depth),
                             vb = sample(row, at + 2, depth);
                    r = vr * 65535U / max;
                    g = vg * 65535U / max;
                    b = vb * 65535U / max;
                    if (color == 6)
                        a = sample(row, at + 3, depth) * 65535U / max;
                    else if (trans && vr == be16(trans) && vg == be16(trans + 2) &&
                             vb == be16(trans + 4))
                        a = 0;
                }
                unsigned char *p = pixels + ((y0 + y * sy) * (size_t)width + x0 + x * sx) * 8;
                put16(p, r);
                put16(p + 2, g);
                put16(p + 4, b);
                put16(p + 6, a);
            }
            prev = row;
        }
    }
    Sha256 hash;
    sha256_init(&hash);
    sha256_update(&hash, "CQ-PNG-RGBA16", 13);
    sha256_update(&hash, png + 16, 8);
    sha256_update(&hash, pixels, (size_t)width * height * 8);
    sha256_final(&hash, out);
    ok = 1;
done:
    free(compressed);
    free(raw);
    free(pixels);
    return ok;
}
