#ifndef CQ_CORE_H
#define CQ_CORE_H
#include <stddef.h>
#include <stdint.h>
#define CQ_MAX_ITEMS 200
#define CQ_MAX_ITEM_BYTES (64u * 1024u * 1024u)
#define CQ_MAX_BYTES (256u * 1024u * 1024u)
typedef struct CqItem {
    unsigned char *data;
    size_t size;
    unsigned format;
    unsigned char hash[32];
    int image;
} CqItem;
typedef struct {
    CqItem *items[CQ_MAX_ITEMS];
    size_t count, bytes;
    CqItem *last_paste;
    unsigned char previous[32];
    int has_previous, previous_image, enabled;
} CqQueue;
CqItem *cq_item(const void *data, size_t size, unsigned format, int image);
void cq_free(CqItem *item);
/* Canonical pixels are tightly packed, straight-alpha RGBA8. */
void cq_pixel_hash(CqItem *item, const void *rgba, uint32_t width, uint32_t height);
/* Takes ownership, including when rejected. 1=added, 0=duplicate/off, -1=full. */
int cq_push(CqQueue *queue, CqItem *item);
CqItem *cq_peek(CqQueue *queue);
void cq_pop(CqQueue *queue);
/* Retain the last dispatched item for one-level undo. */
void cq_commit(CqQueue *queue);
/* 1=restored, 0=nothing to restore, -1=capacity reached; failure preserves undo. */
int cq_undo(CqQueue *queue);
typedef struct {
    uintptr_t target;
    unsigned key;
} CqPasteRequest;
typedef struct {
    CqPasteRequest items[CQ_MAX_ITEMS];
    size_t head, count;
} CqRequests;
int cq_request(CqRequests *requests, uintptr_t target, unsigned key);
CqPasteRequest *cq_next_request(CqRequests *requests);
void cq_finish_request(CqRequests *requests);
void cq_cancel_requests(CqRequests *requests);
void cq_clear(CqQueue *queue);
void cq_enable(CqQueue *queue, int enabled);
#endif
