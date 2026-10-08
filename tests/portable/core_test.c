#include "core.h"
#include "sha256.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static CqItem *item(const char *s) {
    return cq_item(s, strlen(s), 1, 0);
}
int main(void) {
    CqQueue q = {0};
    assert(cq_push(&q, item("off")) == 0 && q.count == 0);
    cq_enable(&q, 1);
    assert(cq_push(&q, item("A")) == 1);
    assert(cq_push(&q, item("A")) == 0);
    assert(cq_push(&q, item("B")) == 1);
    assert(cq_push(&q, item("A")) == 1);
    const char *expected = "ABA";
    for (int i = 0; i < 3; ++i) {
        assert(cq_peek(&q)->data[0] == expected[i]);
        cq_pop(&q);
    }
    assert(!q.count && !q.bytes && !cq_peek(&q));
    assert(cq_push(&q, item("A")) == 0); /* previous copy survives paste */
    cq_clear(&q);
    assert(cq_push(&q, item("A")) == 1);
    cq_enable(&q, 0);
    assert(q.count == 1);
    cq_enable(&q, 1);
    assert(cq_push(&q, item("A")) == 1);
    cq_clear(&q);
    unsigned char pixels[] = {255, 0, 0, 255, 0, 255, 0, 255};
    CqItem *a = cq_item("encoding one", 12, 2, 1), *b = cq_item("different encoding", 18, 3, 1);
    cq_pixel_hash(a, pixels, 2, 1);
    cq_pixel_hash(b, pixels, 2, 1);
    assert(cq_push(&q, a) == 1 && cq_push(&q, b) == 0);
    b = cq_item("different encoding", 18, 3, 1);
    cq_pixel_hash(b, pixels, 1, 2);
    assert(cq_push(&q, b) == 1); /* dimensions matter */
    cq_clear(&q);
    for (int i = 0; i < CQ_MAX_ITEMS; ++i) {
        char s[16];
        snprintf(s, sizeof(s), "%d", i);
        assert(cq_push(&q, item(s)) == 1);
    }
    assert(cq_push(&q, item("overflow")) == -1 && q.count == CQ_MAX_ITEMS);
    cq_pop(&q);
    assert(cq_push(&q, item("overflow")) == 1); /* rejected copies do not poison dedup */
    cq_clear(&q);
    assert(!q.bytes);
    assert(cq_push(&q, item("first")) == 1);
    assert(cq_push(&q, item("second")) == 1);
    cq_commit(&q);
    assert(q.count == 1 && q.last_paste && q.bytes == 6);
    assert(cq_undo(&q) == 1 && q.count == 2 && q.bytes == 11);
    assert(cq_undo(&q) == 0 && !memcmp(cq_peek(&q)->data, "first", 5));
    cq_commit(&q);
    cq_commit(&q);
    assert(cq_undo(&q) == 1 && !memcmp(cq_peek(&q)->data, "second", 6));
    cq_commit(&q);
    for (int i = 0; i < CQ_MAX_ITEMS; ++i) {
        char s[16];
        snprintf(s, sizeof(s), "%d", i);
        assert(cq_push(&q, item(s)) == 1);
    }
    assert(cq_undo(&q) == -1 && q.last_paste);
    cq_pop(&q);
    assert(cq_undo(&q) == 1);
    cq_clear(&q);
    assert(!q.last_paste && !q.bytes);
    CqRequests r = {0};
    for (unsigned cycle = 0; cycle < 3; ++cycle) {
        for (unsigned i = 0; i < CQ_MAX_ITEMS; ++i)
            assert(cq_request(&r, 100 + i, i));
        assert(!cq_request(&r, 999, 999));
        for (unsigned i = 0; i < CQ_MAX_ITEMS; ++i) {
            assert(cq_next_request(&r)->target == 100 + i && cq_next_request(&r)->key == i);
            cq_finish_request(&r);
        }
        assert(!cq_next_request(&r));
    }
    assert(cq_request(&r, 42, 9));
    cq_cancel_requests(&r);
    assert(!cq_next_request(&r));
    unsigned char hash[32];
    sha256("abc", 3, hash);
    const unsigned char known[32] = {0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
                                     0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
                                     0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
                                     0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
    assert(!memcmp(hash, known, 32));
    puts("PASS: FIFO, consecutive dedup, image identity, limits, pause, reset, undo, rapid "
         "requests, SHA-256");
    return 0;
}
