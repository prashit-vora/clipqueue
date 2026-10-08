/* Included by each backend after its private clipboard helpers. */
#include "image_fixtures.h"
static int native_clipboard_test(unsigned text_format, unsigned png_format) {
    CqItem *p = cq_item("Hello \xe2\x98\x83", 9, text_format, 0);
    if (!p || !publish(p)) {
        cq_free(p);
        return 1;
    }
#ifdef _WIN32
    if (!OpenClipboard(window)) {
        cq_free(p);
        return 1;
    }
#endif
    CqItem *read = read_clipboard();
#ifdef _WIN32
    CloseClipboard();
#endif
    int ok = read && read->size == p->size && !memcmp(read->data, p->data, p->size);
    cq_free(read);
    cq_free(p);
    CqItem *a = cq_item(image_a, sizeof(image_a), png_format, 1);
    CqItem *b = cq_item(image_b, sizeof(image_b), png_format, 1);
    CqItem *c = cq_item(image_c, sizeof(image_c), png_format, 1);
    if (!a || !b || !c) {
        cq_free(a);
        cq_free(b);
        cq_free(c);
        return 1;
    }
    image_identity(a);
    image_identity(b);
    image_identity(c);
    ok = ok && !memcmp(a->hash, b->hash, 32) && memcmp(a->hash, c->hash, 32);
    if (!publish(a))
        ok = 0;
#ifdef _WIN32
    int opened = OpenClipboard(window);
    read = opened ? read_clipboard() : NULL;
    if (opened)
        CloseClipboard();
#else
    read = read_clipboard();
#endif
    if (read)
        image_identity(read);
    ok = ok && read && read->image && !memcmp(read->hash, a->hash, 32);
    cq_free(read);
    cq_free(a);
    cq_free(b);
    cq_free(c);
    puts(ok ? "PASS: native Unicode + PNG clipboard, decoded image dedup and distinct pixels"
            : "FAIL: native clipboard/image decoder");
    return !ok;
}
