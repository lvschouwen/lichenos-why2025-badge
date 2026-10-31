// Host test: MeshCore's home tile text (#67)
#include "mesh_tile.h"

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

#define DOT "\xE2\x80\xA2"

static char l1[64], l2[64];

static void text(mesh_tile_t const *t) {
    mesh_tile_text(t, l1, sizeof(l1), l2, sizeof(l2));
}

// a valid UTF-8 string throughout (no sequence cut at the end)
static int utf8_whole(char const *s) {
    size_t n = strlen(s);
    size_t i = 0;
    while (i < n) {
        unsigned char c   = (unsigned char)s[i];
        size_t        len = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
        if (i + len > n) {
            return 0;
        }
        i += len;
    }
    return 1;
}

int main(void) {
    mesh_tile_t t;
    memset(&t, 0, sizeof(t));

    // No messages
    t.contacts = 12;
    text(&t);
    CHECK(strcmp(l1, "12 nodes") == 0);
    CHECK(strcmp(l2, "Listening on Public") == 0);

    // gen 5, seen 2
    t.generation      = 5;
    t.seen_generation = 2;
    text(&t);
    CHECK(strcmp(l1, "3 unread " DOT " 12 nodes") == 0);

    // gen 300, seen 0: capped
    t.generation      = 300;
    t.seen_generation = 0;
    text(&t);
    CHECK(strcmp(l1, "99+ unread " DOT " 12 nodes") == 0);
    t.generation = 99;
    text(&t);
    CHECK(strcmp(l1, "99 unread " DOT " 12 nodes") == 0);

    // One node
    t.contacts = 1;
    t.generation = t.seen_generation = 7;
    text(&t);
    CHECK(strcmp(l1, "1 node") == 0);
    t.contacts = 12;

    // Review focus 5 (unread_counts_from_last_seen_generation): opened at gen 40, one more arrives while open -> 1
    // unread until mesh_mark_seen runs on the next refresh with Messages visible
    t.generation = 40;
    mesh_tile_mark_seen(&t);
    CHECK(t.seen_generation == 40);
    text(&t);
    CHECK(strcmp(l1, "12 nodes") == 0);
    mesh_tile_on_message(&t, "Public", "hello", 41);
    text(&t);
    CHECK(strcmp(l1, "1 unread " DOT " 12 nodes") == 0);
    CHECK(strcmp(l2, "[Public] hello") == 0);
    mesh_tile_mark_seen(&t);
    text(&t);
    CHECK(strcmp(l1, "12 nodes") == 0);
    CHECK(strcmp(l2, "[Public] hello") == 0); // the latest stays

    // Generation wraps: still counts from seen
    t.seen_generation = 0xFFFFFFFEu;
    t.generation      = 1;
    text(&t);
    CHECK(strcmp(l1, "3 unread " DOT " 12 nodes") == 0);

    // The latest 160 bytes: cut to c2 - 1 on a UTF-8 boundary (é = 2 bytes, starting at byte 0, so byte 63 splits one)
    char long_text[MESH_TEXT_MAX + 1];
    for (size_t i = 0; i < MESH_TEXT_MAX; i += 2) {
        long_text[i]     = (char)0xC3;
        long_text[i + 1] = (char)0xA9;
    }
    long_text[MESH_TEXT_MAX] = '\0';
    memcpy(t.latest, long_text, sizeof(long_text));
    text(&t);
    CHECK(strlen(l2) == 62);
    CHECK(utf8_whole(l2));
    char even[64];
    mesh_tile_text(&t, l1, sizeof(l1), even, 63); // 62 bytes fit exactly
    CHECK(strlen(even) == 62);

    // A message longer than the tile's buffer is kept cut, whole characters only
    mesh_tile_on_message(&t, "Public", long_text, 42);
    CHECK(strlen(t.latest) <= MESH_TEXT_MAX);
    CHECK(utf8_whole(t.latest));
    CHECK(strncmp(t.latest, "[Public] ", 9) == 0);

    // Tiny buffers never overrun
    char tiny1[4], tiny2[4];
    mesh_tile_text(&t, tiny1, sizeof(tiny1), tiny2, sizeof(tiny2));
    CHECK(strlen(tiny1) <= 3 && strlen(tiny2) <= 3);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("mesh_tile_test: all passed\n");
    return 0;
}
