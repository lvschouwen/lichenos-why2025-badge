// Host tests for keeping a rebuilt list where the reader was (#67): anchored on the first visible item's identity
#include "scroll_anchor.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

// n items of height h with gap g, keys first_key, first_key+1, ...
static size_t make(scroll_item_t *items, size_t n, uint32_t first_key, int32_t h, int32_t g) {
    for (size_t i = 0; i < n; i++) {
        items[i].key    = first_key + (uint32_t)i;
        items[i].top    = (int32_t)i * (h + g);
        items[i].bottom = items[i].top + h;
    }
    return n;
}

int main(void) {
    scroll_item_t before[60], after[60];
    int32_t       y;

    // A full 50-message window, the reader scrolled to item 20 (top 2000) plus 30 px into it
    size_t          nb = make(before, 50, 100, 90, 10);
    scroll_anchor_t a  = scroll_anchor_take(before, nb, 2030);
    CHECK(a.n > 0 && a.key[0] == 120 && a.offset[0] == -30);

    // A new message: the oldest (key 100) drops off the top, key 150 comes at the bottom. The pixel offset alone would
    // jump one item; the anchor keeps item 120 where it was
    size_t na = make(after, 50, 101, 90, 10);
    CHECK(scroll_anchor_find(&a, after, na, &y));
    CHECK(y == 1930); // item 120 is now at 1900, still 30 px scrolled into it

    // Items of different heights: the anchor follows the item, not the index
    after[0].bottom = 300; // the first (oldest) message is taller
    for (size_t i = 1; i < na; i++) {
        after[i].top    = after[i - 1].bottom + 10;
        after[i].bottom = after[i].top + 90;
    }
    CHECK(scroll_anchor_find(&a, after, na, &y));
    CHECK(y == after[19].top + 30 && after[19].key == 120);

    // Contacts re-ordered: the first visible contact (key 120) moved to the top of the list
    na = make(after, 50, 100, 90, 10);
    after[0].key  = 120;
    after[20].key = 100;
    CHECK(scroll_anchor_find(&a, after, na, &y));
    CHECK(y == 30);

    // The first visible item is gone: the next one anchored is used
    na = make(after, 49, 121, 90, 10);
    CHECK(scroll_anchor_find(&a, after, na, &y));
    CHECK(y == -70); // item 121 at 0 sat 70 px below the view's top; the list clamps a negative scroll itself

    // Nothing anchored is left: not found, y untouched
    y  = 1234;
    na = make(after, 10, 500, 90, 10);
    CHECK(!scroll_anchor_find(&a, after, na, &y) && y == 1234);

    // An empty list or one scrolled past everything anchors nothing
    a = scroll_anchor_take(before, 0, 0);
    CHECK(a.n == 0 && !scroll_anchor_find(&a, after, na, &y));
    a = scroll_anchor_take(before, nb, 999999);
    CHECK(a.n == 0);

    // At most SCROLL_ANCHOR_KEYS keys, in order from the first visible
    a = scroll_anchor_take(before, nb, 0);
    CHECK(a.n == SCROLL_ANCHOR_KEYS && a.key[0] == 100 && a.key[SCROLL_ANCHOR_KEYS - 1] == 100 + SCROLL_ANCHOR_KEYS - 1);
    CHECK(a.offset[1] == 100);

    // An item is visible while its bottom is below the view's top
    a = scroll_anchor_take(before, nb, 90); // item 0 ends exactly at 90: hidden, the gap shows, item 1 is first
    CHECK(a.key[0] == 101 && a.offset[0] == 10);

    // Message identity: the same content gives the same key, a different one (very likely) another
    CHECK(scroll_key_hash("abc", 3, 7) == scroll_key_hash("abc", 3, 7));
    CHECK(scroll_key_hash("abc", 3, 7) != scroll_key_hash("abd", 3, 7));
    CHECK(scroll_key_hash("abc", 3, 7) != scroll_key_hash("abc", 3, 8));

    if (failures) {
        printf("%d failures\n", failures);
        return 1;
    }
    printf("scroll_anchor_test: all passed\n");
    return 0;
}
