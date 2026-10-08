#include "core.h"
#include "sha256.h"
#include <stdlib.h>
#include <string.h>
CqItem *cq_item(const void *data, size_t size, unsigned format, int image) {
    if (!size || size > CQ_MAX_ITEM_BYTES)
        return NULL;
    CqItem *p = calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    p->data = malloc(size);
    if (!p->data) {
        free(p);
        return NULL;
    }
    memcpy(p->data, data, size);
    p->size = size;
    p->format = format;
    p->image = image;
    sha256(data, size, p->hash);
    return p;
}
void cq_free(CqItem *p) {
    if (p) {
        free(p->data);
        free(p);
    }
}
void cq_pixel_hash(CqItem *p, const void *rgba, uint32_t w, uint32_t h) {
    unsigned char dims[8];
    for (int n = 0; n < 4; n++) {
        dims[n] = (unsigned char)(w >> (n * 8));
        dims[n + 4] = (unsigned char)(h >> (n * 8));
    }
    Sha256 s;
    sha256_init(&s);
    sha256_update(&s, "ClipQueue RGBA8", 15);
    sha256_update(&s, dims, sizeof(dims));
    sha256_update(&s, rgba, (size_t)w * h * 4);
    sha256_final(&s, p->hash);
}
int cq_push(CqQueue *q, CqItem *p) {
    if (!p)
        return -1;
    if (!q->enabled ||
        (q->has_previous && q->previous_image == p->image && !memcmp(q->previous, p->hash, 32))) {
        cq_free(p);
        return 0;
    }
    if (q->count == CQ_MAX_ITEMS || p->size > CQ_MAX_BYTES - q->bytes) {
        cq_free(p);
        return -1;
    }
    q->items[q->count++] = p;
    q->bytes += p->size;
    memcpy(q->previous, p->hash, 32);
    q->previous_image = p->image;
    q->has_previous = 1;
    return 1;
}
CqItem *cq_peek(CqQueue *q) {
    return q->count ? q->items[0] : NULL;
}
void cq_pop(CqQueue *q) {
    if (!q->count)
        return;
    q->bytes -= q->items[0]->size;
    cq_free(q->items[0]);
    memmove(q->items, q->items + 1, --q->count * sizeof(*q->items));
}
void cq_clear(CqQueue *q) {
    while (q->count)
        cq_pop(q);
    q->has_previous = 0;
}
void cq_enable(CqQueue *q, int enabled) {
    q->enabled = !!enabled;
    q->has_previous = 0;
}
