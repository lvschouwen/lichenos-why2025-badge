// Host test: the badge's store of heard channel messages and contacts (#61)
#include "mesh_store.h"

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

int main(void) {
    CHECK(mesh_store_init(3));
    mesh_msg_t m;

    // Oldest messages drop when the ring is full
    mesh_store_add_message("Public", "a: one", -600, 1);
    mesh_store_add_message("Public", "b: two", -610, 2);
    mesh_store_add_message("Public", "c: three", -620, 3);
    mesh_store_add_message("Public", "d: four", -630, 4);
    CHECK(mesh_store_message_count() == 3);
    CHECK(mesh_store_message(0, &m) && strcmp(m.text, "b: two") == 0 && m.heard_s == 2);
    CHECK(mesh_store_message(2, &m) && strcmp(m.text, "d: four") == 0 && m.rssi_x10 == -630);
    CHECK(!mesh_store_message(3, &m));

    // A long message is cut to MESH_TEXT_MAX on a character boundary (é is two bytes)
    char long_text[300];
    memset(long_text, 'x', sizeof(long_text));
    memcpy(long_text + MESH_TEXT_MAX - 1, "\xc3\xa9", 2); // é straddles the limit
    long_text[sizeof(long_text) - 1] = '\0';
    mesh_store_add_message("Public", long_text, 0, 5);
    CHECK(mesh_store_message(2, &m) && strlen(m.text) == MESH_TEXT_MAX - 1 && m.text[MESH_TEXT_MAX - 2] == 'x');

    // Invalid UTF-8 and control characters never reach the screen
    char out[32];
    CHECK(mesh_sanitize("ok\xff\xfe!", 6, out, sizeof(out)) == 5 && strcmp(out, "ok?" "?!") == 0);
    CHECK(mesh_sanitize("a\nb\tc\x1b", 6, out, sizeof(out)) == 6 && strcmp(out, "a b c ") == 0);
    CHECK(mesh_sanitize("\xf0\x9f\x8c\x9e sun", 8, out, sizeof(out)) == 8 && strcmp(out, "\xf0\x9f\x8c\x9e sun") == 0);
    CHECK(mesh_sanitize("\xf0\x9f\x8c", 3, out, sizeof(out)) == 3 && strcmp(out, "???") == 0); // truncated sequence
    CHECK(mesh_sanitize("abc", 3, out, 1) == 0 && out[0] == '\0');

    // Not UTF-8: overlongs, surrogates and code points above U+10FFFF are one '?' per byte
    CHECK(mesh_sanitize("\xe0\x80\x80", 3, out, sizeof(out)) == 3 && strcmp(out, "???") == 0);
    CHECK(mesh_sanitize("\xed\xa0\x80", 3, out, sizeof(out)) == 3 && strcmp(out, "???") == 0);
    CHECK(mesh_sanitize("\xf0\x80\x80\x80", 4, out, sizeof(out)) == 4 && strcmp(out, "????") == 0);
    CHECK(mesh_sanitize("\xf4\x90\x80\x80", 4, out, sizeof(out)) == 4 && strcmp(out, "????") == 0);
    // C1 controls (U+0080..U+009F, e.g. CSI) are control characters too: one ' '
    CHECK(mesh_sanitize("a\xc2\x9b" "b", 4, out, sizeof(out)) == 3 && strcmp(out, "a b") == 0);
    // The edges that are fine: U+00A0, U+D7FF, U+10FFFF
    CHECK(mesh_sanitize("\xc2\xa0", 2, out, sizeof(out)) == 2 && strcmp(out, "\xc2\xa0") == 0);
    CHECK(mesh_sanitize("\xed\x9f\xbf", 3, out, sizeof(out)) == 3 && strcmp(out, "\xed\x9f\xbf") == 0);
    CHECK(mesh_sanitize("\xf4\x8f\xbf\xbf", 4, out, sizeof(out)) == 4 && strcmp(out, "\xf4\x8f\xbf\xbf") == 0);

    // Every new message changes the generation, also once the ring is full (the count stays the same then)
    uint32_t gen = mesh_store_generation();
    mesh_store_add_message("Public", "e: five", -600, 6);
    CHECK(mesh_store_generation() != gen && mesh_store_message_count() == 3u);
    gen = mesh_store_generation();
    mesh_store_add_message("Public", "f: six", -600, 7);
    CHECK(mesh_store_generation() != gen && mesh_store_message_count() == 3u);

    // Our own messages (#46): in the ring with their state, but never unread
    CHECK(mesh_store_init(4));
    mesh_store_add_message("Public", "a: hi", -600, 1);
    uint32_t in_gen = mesh_store_generation(), view = mesh_store_view_generation();
    mesh_store_add_outgoing("#t", "me: yo", 7, 2);
    CHECK(mesh_store_generation() == in_gen && mesh_store_view_generation() != view && mesh_store_message_count() == 2);
    CHECK(mesh_store_message(1, &m) && m.outgoing && m.id == 7 && m.state == 0 && strcmp(m.text, "me: yo") == 0);
    CHECK(mesh_store_message(0, &m) && !m.outgoing && m.id == 0);
    view = mesh_store_view_generation();
    CHECK(mesh_store_update_out(7, 2, 0) && mesh_store_view_generation() != view);
    view = mesh_store_view_generation();
    CHECK(!mesh_store_update_out(7, 2, 0) && mesh_store_view_generation() == view); // unchanged
    CHECK(!mesh_store_update_out(8, 2, 0) && !mesh_store_update_out(0, 2, 0));
    CHECK(mesh_store_message(1, &m) && m.state == 2);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("mesh_store_test: all checks passed\n");
    return 0;
}
