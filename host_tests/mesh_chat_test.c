// Host test: what the channel chat screen shows (#46)
#include "mesh_chat.h"

#include "mesh_outbox.h"

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

static mesh_status_kind_t status(int outgoing, int state, int repeats, char *out) {
    mesh_msg_t m;
    memset(&m, 0, sizeof(m));
    m.outgoing = (uint8_t)outgoing, m.state = (uint8_t)state, m.repeats = (uint8_t)repeats;
    return mesh_msg_status(&m, out, 32);
}

// Search inside a chat (#48): hits oldest first, all words, case-insensitive; the newest hit first, Up/Down wrap
static void search_checks(void) {
    static mesh_msg_t m[5];
    memset(m, 0, sizeof(m));
    char const *texts[5] = {"alice: Foo bar", "bob: nothing", "carol: foo", "dave: BAR foo baz", "eve: food"};
    for (int i = 0; i < 5; i++) {
        strcpy(m[i].text, texts[i]);
        m[i].seq = 100 + (uint32_t)i;
    }
    uint16_t hits[8];
    CHECK(mesh_chat_hits(m, 5, "", hits, 8) == 0);   // empty query: nothing
    CHECK(mesh_chat_hits(m, 5, "  ", hits, 8) == 0); // blank too
    CHECK(mesh_chat_hits(m, 5, NULL, hits, 8) == 0);
    CHECK(mesh_chat_hits(m, 5, "zzz", hits, 8) == 0); // no hits
    CHECK(mesh_chat_hits(m, 0, "foo", hits, 8) == 0); // no messages
    size_t n = mesh_chat_hits(m, 5, "FOO", hits, 8);  // case-insensitive substring: food counts
    CHECK(n == 4 && hits[0] == 0 && hits[1] == 2 && hits[2] == 3 && hits[3] == 4);
    n = mesh_chat_hits(m, 5, "bar foo", hits, 8); // every word, any order
    CHECK(n == 2 && hits[0] == 0 && hits[1] == 3);
    CHECK(mesh_chat_hits(m, 5, "foo", hits, 2) == 2 && hits[1] == 2); // never past max

    // stepping: the newest first, Up (-1) older, Down (+1) newer, wrapping at both ends
    CHECK(mesh_chat_hit_step(MESH_CHAT_NO_HIT, 4, 0) == 3);
    CHECK(mesh_chat_hit_step(MESH_CHAT_NO_HIT, 4, -1) == 3);
    CHECK(mesh_chat_hit_step(9, 4, 1) == 3); // stale position
    CHECK(mesh_chat_hit_step(3, 4, -1) == 2);
    CHECK(mesh_chat_hit_step(0, 4, -1) == 3); // wrap past the oldest
    CHECK(mesh_chat_hit_step(3, 4, 1) == 0);  // wrap past the newest
    CHECK(mesh_chat_hit_step(1, 4, 1) == 2);
    CHECK(mesh_chat_hit_step(0, 1, 1) == 0 && mesh_chat_hit_step(0, 1, -1) == 0);
    CHECK(mesh_chat_hit_step(0, 0, 1) == MESH_CHAT_NO_HIT); // no hits

    // the current hit survives a rebuild by its seq
    n = mesh_chat_hits(m, 5, "foo", hits, 8);
    CHECK(mesh_chat_hit_of_seq(m, hits, n, 103) == 2);
    CHECK(mesh_chat_hit_of_seq(m, hits, n, 101) == MESH_CHAT_NO_HIT); // not a hit
    CHECK(mesh_chat_hit_of_seq(m, hits, 0, 100) == MESH_CHAT_NO_HIT);

    char line[64];
    mesh_chat_search_line("", MESH_CHAT_NO_HIT, 0, line, sizeof(line));
    CHECK(strcmp(line, "Search: _  type to search") == 0);
    mesh_chat_search_line("zzz", MESH_CHAT_NO_HIT, 0, line, sizeof(line));
    CHECK(strcmp(line, "Search: zzz_  no match") == 0);
    mesh_chat_search_line("foo", 3, 4, line, sizeof(line)); // the newest is 1 of 4
    CHECK(strcmp(line, "Search: foo_  (1 of 4)") == 0);
    mesh_chat_search_line("foo", 0, 4, line, sizeof(line));
    CHECK(strcmp(line, "Search: foo_  (4 of 4)") == 0);
    mesh_chat_search_line("foo", 0, 4, line, 8); // small buffer stays terminated
    CHECK(strlen(line) == 7);
}

int main(void) {
    char s[32];
    CHECK(status(0, MESH_OUT_FAILED, 0, s) == MESH_STATUS_NONE && s[0] == '\0'); // incoming: no state
    CHECK(status(1, MESH_OUT_QUEUED, 0, s) == MESH_STATUS_PENDING && strcmp(s, "queued") == 0);
    CHECK(status(1, MESH_OUT_SENT, 0, s) == MESH_STATUS_PENDING && strcmp(s, "queued") == 0);
    CHECK(status(1, MESH_OUT_ON_AIR, 0, s) == MESH_STATUS_PENDING && strcmp(s, "on air") == 0);
    CHECK(status(1, MESH_OUT_HEARD, 1, s) == MESH_STATUS_OK && strcmp(s, "heard repeated") == 0);
    CHECK(status(1, MESH_OUT_HEARD, 3, s) == MESH_STATUS_OK && strcmp(s, "heard repeated x3") == 0);
    CHECK(status(1, MESH_OUT_DELIVERED, 0, s) == MESH_STATUS_OK && strcmp(s, "delivered") == 0);
    CHECK(status(1, MESH_OUT_FAILED, 0, s) == MESH_STATUS_FAILED && strcmp(s, "not sent") == 0);

    // The counter and the cut: 10 bytes of room
    char c[64];
    // the clock gate (#8): adverts refused, messages hinted, while the clock is not set
    CHECK(mesh_advert_refusal(false) && strstr(mesh_advert_refusal(false), "clock not set"));
    CHECK(mesh_advert_refusal(true) == NULL);
    CHECK(mesh_clock_hint(false, false) && strstr(mesh_clock_hint(false, false), "Clock not set"));
    CHECK(mesh_clock_hint(false, true) && strstr(mesh_clock_hint(false, true), "Clock not set"));
    CHECK(mesh_clock_hint(true, false) == NULL);
    // #83: a clock set from mesh contacts only is approximate: adverts go out, with a note; the composer says so
    CHECK(mesh_clock_hint(true, true) && strstr(mesh_clock_hint(true, true), "mesh contacts, unverified"));
    CHECK(mesh_advert_note(true) && strstr(mesh_advert_note(true), "approximate"));
    CHECK(mesh_advert_note(false) == NULL);
    CHECK(mesh_compose_status("", 10, c, sizeof(c)) == 0 && strcmp(c, "0 / 10 bytes") == 0);
    CHECK(mesh_compose_status("hello", 10, c, sizeof(c)) == 5 && strcmp(c, "5 / 10 bytes") == 0);
    CHECK(mesh_compose_status("0123456789", 10, c, sizeof(c)) == 10 && strcmp(c, "10 / 10 bytes") == 0);
    CHECK(mesh_compose_status("0123456789ab", 10, c, sizeof(c)) == 10 && strstr(c, "12 / 10") && strstr(c, "last 2"));
    // never inside a UTF-8 character: the é (2 bytes) straddles byte 10
    CHECK(mesh_compose_status("012345678\xc3\xa9z", 10, c, sizeof(c)) == 9 && strstr(c, "last 3"));

    // DMs (#47)
    mesh_msg_t d;
    memset(&d, 0, sizeof(d));
    d.outgoing = 1, d.dm = 1, d.state = MESH_OUT_SENT, d.attempt = 1;
    CHECK(mesh_msg_status(&d, s, sizeof(s)) == MESH_STATUS_PENDING && strcmp(s, "waiting for ACK") == 0);
    d.attempt = 2;
    CHECK(strcmp((mesh_msg_status(&d, s, sizeof(s)), s), "waiting for ACK, try 2/3") == 0);
    d.state = MESH_OUT_QUEUED;
    CHECK(mesh_msg_status(&d, s, sizeof(s)) == MESH_STATUS_PENDING && strcmp(s, "retrying, try 2/3") == 0);
    d.state = MESH_OUT_QUEUED, d.attempt = 0;
    CHECK(strcmp((mesh_msg_status(&d, s, sizeof(s)), s), "queued") == 0);
    d.state = MESH_OUT_DELIVERED, d.attempt = 1, d.trip_ms = 1840;
    CHECK(mesh_msg_status(&d, s, sizeof(s)) == MESH_STATUS_OK && strcmp(s, "delivered, 1.8 s") == 0);
    d.trip_ms = 420;
    CHECK(strcmp((mesh_msg_status(&d, s, sizeof(s)), s), "delivered, 420 ms") == 0);
    d.state = MESH_OUT_FAILED;
    CHECK(mesh_msg_status(&d, s, sizeof(s)) == MESH_STATUS_FAILED && strcmp(s, "not delivered") == 0);
    uint8_t const peer[6] = {1, 2, 3, 4, 5, 6}, other[6] = {1, 2, 3, 4, 5, 7};
    memcpy(d.peer, peer, 6);
    CHECK(mesh_msg_in_dm(&d, peer) && !mesh_msg_in_dm(&d, other));
    d.dm = 0;
    CHECK(!mesh_msg_in_dm(&d, peer));


    // The Messages tab's rows (#48)
    {
        mesh_conv_t cv;
        char        title[40], badge[8], sub[220];
        memset(&cv, 0, sizeof(cv));
        strcpy(cv.title, "#badgetest");
        mesh_conv_row(&cv, 100, title, sizeof(title), badge, sizeof(badge), sub, sizeof(sub));
        CHECK(strcmp(title, "#badgetest") == 0 && badge[0] == '\0' && strcmp(sub, "no messages yet") == 0);
        cv.count = 2, cv.unread = 3, cv.last_heard_s = 40;
        strcpy(cv.last, "bob: hello");
        mesh_conv_row(&cv, 220, title, sizeof(title), badge, sizeof(badge), sub, sizeof(sub));
        CHECK(strcmp(badge, "3") == 0 && strcmp(sub, "3 min ago: bob: hello") == 0);
        cv.unread = 99;
        mesh_conv_row(&cv, 220, title, sizeof(title), badge, sizeof(badge), sub, sizeof(sub));
        CHECK(strcmp(badge, "99") == 0);
        cv.unread = 100;
        mesh_conv_row(&cv, 30, title, sizeof(title), badge, sizeof(badge), sub, sizeof(sub)); // clock before the message
        CHECK(strcmp(badge, "99+") == 0 && strcmp(sub, "just now: bob: hello") == 0);
        mesh_conv_row(&cv, 30, title, 4, badge, 2, sub, 8); // small buffers stay terminated
        CHECK(strlen(title) == 3 && strlen(badge) == 1 && strlen(sub) == 7);
    }
    search_checks();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("mesh_chat_test: all checks passed\n");
    return 0;
}
