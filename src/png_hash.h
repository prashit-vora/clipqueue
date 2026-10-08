#ifndef CQ_PNG_HASH_H
#define CQ_PNG_HASH_H
#include <stddef.h>
/* Hash canonical RGBA16 pixels and dimensions. Returns 0 for malformed,
 * animated, unsupported, or over-limit images; caller hashes original bytes.
 * Uses only the desktop's system zlib for bounded DEFLATE decompression. */
int png_pixel_hash(const unsigned char *png, size_t length, unsigned char out[32]);
#endif
