// Host test: the badge's store of heard channel messages and contacts (#61)
#include "mesh_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static int fail_alloc, warned;
static void *test_alloc(size_t count, size_t size) {
    return fail_alloc ? NULL : calloc(count, size);
}
static void test_warn(char const *what) {
    (void)what;
    warned++;
}

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
    CHECK(mesh_store_update_out(7, 2, 0, 0, 0) && mesh_store_view_generation() != view);
    view = mesh_store_view_generation();
    CHECK(!mesh_store_update_out(7, 2, 0, 0, 0) && mesh_store_view_generation() == view); // unchanged
    CHECK(!mesh_store_update_out(8, 2, 0, 0, 0) && !mesh_store_update_out(0, 2, 0, 0, 0));
    CHECK(mesh_store_message(1, &m) && m.state == 2);

    CHECK(mesh_store_init(8));
    {
        uint8_t const peer[6] = {9, 8, 7, 6, 5, 4};
        mesh_store_add_outgoing_dm("@bob", "me: hi", 9, 3, peer);
        mesh_msg_t m;
        CHECK(mesh_store_message(mesh_store_message_count() - 1, &m) && m.dm && m.outgoing && m.id == 9 &&
              memcmp(m.peer, peer, 6) == 0);
        CHECK(mesh_store_update_out(9, 4, 0, 2, 1500));
        CHECK(mesh_store_message(mesh_store_message_count() - 1, &m) && m.attempt == 2 && m.trip_ms == 1500);
        mesh_store_add_dm("@bob", "hello", -50, 4, peer);
        CHECK(mesh_store_message(mesh_store_message_count() - 1, &m) && m.dm && !m.outgoing && m.peer[0] == 9);
    }
    // #59: a quiet chat keeps its messages however much Public says
    CHECK(mesh_store_init(5));
    {
        uint8_t const bob[6] = {1, 2, 3, 4, 5, 6}, eve[6] = {6, 5, 4, 3, 2, 1};
        mesh_chat_key_t kb, ke, kc, kp;
        mesh_chat_key_dm(&kb, bob);
        mesh_chat_key_dm(&ke, eve);
        mesh_chat_key_channel(&kc, "#BadgeTest");
        mesh_chat_key_channel(&kp, "Public");
        mesh_store_add_dm("@bob", "hi", -50, 1, bob);
        mesh_store_add_message("#badgetest", "x: t1", -60, 2);
        for (int i = 0; i < 300; i++) {
            char t[16];
            snprintf(t, sizeof(t), "p: %d", i);
            mesh_store_add_message("Public", t, -70, 3 + (uint32_t)i);
        }
        mesh_msg_t buf[8];
        CHECK(mesh_store_chat(&kb, buf, 8, false) == 1 && strcmp(buf[0].text, "hi") == 0 && buf[0].dm);
        CHECK(mesh_store_chat(&kc, buf, 8, false) == 1 && strcmp(buf[0].text, "x: t1") == 0); // key ignores case
        CHECK(mesh_store_chat(&kp, buf, 8, false) == 5 && strcmp(buf[4].text, "p: 299") == 0 &&
              strcmp(buf[0].text, "p: 295") == 0);
        CHECK(mesh_store_chat(&kp, buf, 2, false) == 2 && strcmp(buf[0].text, "p: 298") == 0); // newest two
        CHECK(mesh_store_chat(&ke, buf, 8, false) == 0);

        // Order over all chats (the old flat view): oldest first, by arrival
        CHECK(mesh_store_message_count() == 7);
        CHECK(mesh_store_recent(buf, 3) == 3 && strcmp(buf[0].text, "p: 297") == 0 && strcmp(buf[2].text, "p: 299") == 0);
        mesh_msg_t one;
        CHECK(mesh_store_message(0, &one) && strcmp(one.text, "hi") == 0);
        CHECK(mesh_store_message(1, &one) && strcmp(one.text, "x: t1") == 0);

        // Unread per chat, our own messages and mark_read
        CHECK(mesh_store_unread_total() == 1 + 1 + 300);
        mesh_store_add_outgoing("#badgetest", "me: yo", 5, 400);
        CHECK(mesh_store_unread_total() == 302);
        mesh_conv_t cv[MESH_CONV_MAX];
        size_t      n = mesh_store_convs(cv, MESH_CONV_MAX);
        CHECK(n == 3);
        for (size_t i = 0; i < n; i++) {
            if (cv[i].key.kind == MESH_CHAT_DM) {
                CHECK(cv[i].unread == 1 && strcmp(cv[i].title, "@bob") == 0);
            } else if (strcmp(cv[i].key.chan, "#badgetest") == 0) {
                CHECK(cv[i].unread == 1 && cv[i].count == 2 && cv[i].last_outgoing && strcmp(cv[i].last, "me: yo") == 0);
            } else {
                CHECK(cv[i].unread == 300 && cv[i].count == 5);
            }
        }
        CHECK(mesh_store_chat(&kb, buf, 8, true) == 1);
        CHECK(mesh_store_unread_total() == 301);
        CHECK(mesh_store_chat(&kp, buf, 8, true) == 5 && mesh_store_unread_total() == 1);
        mesh_store_add_dm("@bob", "again", -50, 500, bob);
        CHECK(mesh_store_unread_total() == 2);
        CHECK(mesh_store_update_out(5, 3, 1, 0, 0)); // found in its own chat
        CHECK(mesh_store_chat(&kc, buf, 8, false) == 2 && buf[1].state == 3);
    }

    // The chat table: a full table drops the DM chat quiet for longest, never a channel
    CHECK(mesh_store_init(2));
    {
        uint8_t peer[6] = {0};
        char    name[16];
        for (int i = 0; i < 4; i++) {
            snprintf(name, sizeof(name), "#c%d", i);
            mesh_store_add_message(name, "x", 0, 1);
        }
        for (int i = 0; i < MESH_CONV_MAX - 4; i++) {
            peer[0] = (uint8_t)(i + 1);
            mesh_store_add_dm("@d", "x", 0, 1, peer);
        }
        mesh_conv_t cv[MESH_CONV_MAX + 1];
        CHECK(mesh_store_convs(cv, MESH_CONV_MAX + 1) == MESH_CONV_MAX);
        peer[0] = 200; // one more: the first DM (peer 1) goes
        mesh_store_add_dm("@new", "x", 0, 2, peer);
        mesh_chat_key_t k;
        mesh_msg_t      b[2];
        peer[0] = 1;
        mesh_chat_key_dm(&k, peer);
        CHECK(mesh_store_chat(&k, b, 2, false) == 0);
        peer[0] = 200;
        mesh_chat_key_dm(&k, peer);
        CHECK(mesh_store_chat(&k, b, 2, false) == 1);
        mesh_chat_key_channel(&k, "#c0");
        CHECK(mesh_store_chat(&k, b, 2, false) == 1);
        CHECK(mesh_store_convs(cv, MESH_CONV_MAX + 1) == MESH_CONV_MAX);
    }

    // Eviction goes by the newest message of a chat: DM #1 gets a late message, so DM #2 is the one that goes
    CHECK(mesh_store_init(2));
    {
        uint8_t peer[6] = {0};
        char    name[16];
        mesh_chat_key_t k;
        mesh_msg_t      b[2];
        for (int i = 0; i < 3; i++) {
            snprintf(name, sizeof(name), "#c%d", i);
            mesh_store_add_message(name, "x", 0, 1);
        }
        for (int i = 0; i < MESH_CONV_MAX - 3; i++) {
            peer[0] = (uint8_t)(i + 1);
            mesh_store_add_dm("@d", "x", 0, 1, peer);
        }
        peer[0] = 1;
        CHECK(mesh_store_add_dm("@d", "late", 0, 9, peer)); // DM #1 is now the newest
        peer[0] = 200;
        CHECK(mesh_store_add_dm("@new", "x", 0, 10, peer));
        peer[0] = 2;
        mesh_chat_key_dm(&k, peer);
        CHECK(mesh_store_chat(&k, b, 2, false) == 0); // DM #2 was the one quiet for longest
        peer[0] = 1;
        mesh_chat_key_dm(&k, peer);
        CHECK(mesh_store_chat(&k, b, 2, false) == 2);
    }

    // A table of 64 channels: a DM has no room and is dropped, nothing breaks
    CHECK(mesh_store_init(2));
    mesh_store_set_hooks(NULL, test_warn);
    warned = 0;
    {
        char    name[16];
        uint8_t peer[6] = {7, 7, 7, 7, 7, 7};
        for (int i = 0; i < MESH_CONV_MAX; i++) {
            snprintf(name, sizeof(name), "#c%d", i);
            CHECK(mesh_store_add_message(name, "x", 0, 1));
        }
        CHECK(!mesh_store_add_dm("@who", "hi", 0, 2, peer) && !mesh_store_add_outgoing_dm("@who", "me: hi", 3, 2, peer));
        CHECK(!mesh_store_add_message("#one-more", "x", 0, 3));
        CHECK(mesh_store_dropped() == 3 && warned == 3);
        mesh_conv_t cv[MESH_CONV_MAX + 1];
        CHECK(mesh_store_convs(cv, MESH_CONV_MAX + 1) == MESH_CONV_MAX);
        CHECK(mesh_store_unread_total() == MESH_CONV_MAX);
    }

    // No memory for a new ring: the new message is dropped and the DM that would have made room stays, with its messages
    CHECK(mesh_store_init(2));
    mesh_store_set_hooks(test_alloc, test_warn);
    warned = 0;
    {
        uint8_t peer[6] = {0};
        char    name[16];
        for (int i = 0; i < 2; i++) {
            snprintf(name, sizeof(name), "#c%d", i);
            mesh_store_add_message(name, "x", 0, 1);
        }
        for (int i = 0; i < MESH_CONV_MAX - 2; i++) {
            peer[0] = (uint8_t)(i + 1);
            mesh_store_add_dm("@d", "keep me", 0, 1, peer);
        }
        fail_alloc = 1;
        peer[0]    = 200;
        CHECK(!mesh_store_add_dm("@new", "x", 0, 5, peer) && warned == 1 && mesh_store_dropped() == 1);
        fail_alloc = 0;
        mesh_chat_key_t k;
        mesh_msg_t      b[2];
        peer[0] = 1;
        mesh_chat_key_dm(&k, peer);
        CHECK(mesh_store_chat(&k, b, 2, false) == 1 && strcmp(b[0].text, "keep me") == 0); // the victim is still there
        peer[0] = 200;
        mesh_chat_key_dm(&k, peer);
        CHECK(mesh_store_chat(&k, b, 2, false) == 0);
        CHECK(mesh_store_add_dm("@new", "x", 0, 6, peer)); // and it works once memory is back
    }
    mesh_store_set_hooks(NULL, NULL);

    // A DM from a contact whose name could not be looked up ("@?") does not replace the name we know
    CHECK(mesh_store_init(2));
    {
        uint8_t const peer[6] = {4, 4, 4, 4, 4, 4};
        mesh_store_add_dm("@alice", "a", 0, 1, peer);
        mesh_store_add_outgoing_dm("@?", "me: b", 2, 2, peer);
        mesh_conv_t cv[2];
        CHECK(mesh_store_convs(cv, 2) == 1 && strcmp(cv[0].title, "@alice") == 0 && cv[0].count == 2);
    }
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("mesh_store_test: all checks passed\n");
    return 0;
}
