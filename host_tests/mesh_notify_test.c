// Host test: what MeshCore tells the notification service (#50)
#include "mesh_notify.h"

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

static mn_switches_t const ON = MN_DEFAULT_SWITCHES;

static void decide_matrix(void) {
    mn_switches_t sw = ON;
    // a DM: only its switch, the chat being open and do-not-disturb matter
    CHECK(mn_decide(MN_DM, MN_MODE_OFF, &sw, false, false, false).verdict == MN_POST);
    CHECK(mn_decide(MN_DM, MN_MODE_ALL, &sw, true, false, false).reason == MN_R_CHAT_OPEN);
    CHECK(mn_decide(MN_DM, MN_MODE_ALL, &sw, false, false, true).verdict == MN_SILENT);
    sw.dm = false;
    CHECK(mn_decide(MN_DM, MN_MODE_ALL, &sw, false, false, false).reason == MN_R_DM_OFF);
    sw = ON;

    // channels
    CHECK(mn_decide(MN_CHANNEL, MN_MODE_ALL, &sw, false, false, false).verdict == MN_POST);
    CHECK(mn_decide(MN_CHANNEL, MN_MODE_ALL, &sw, true, true, false).verdict == MN_SKIP);
    CHECK(mn_decide(MN_CHANNEL, MN_MODE_OFF, &sw, false, true, false).reason == MN_R_MODE_OFF);
    CHECK(mn_decide(MN_CHANNEL, MN_MODE_MENTIONS, &sw, false, false, false).reason == MN_R_NOT_MENTION);
    CHECK(mn_decide(MN_CHANNEL, MN_MODE_MENTIONS, &sw, false, true, false).verdict == MN_POST);
    CHECK(mn_decide(MN_CHANNEL, MN_MODE_MENTIONS, &sw, false, true, true).verdict == MN_SILENT);
    sw.mentions = false;
    CHECK(mn_decide(MN_CHANNEL, MN_MODE_MENTIONS, &sw, false, true, false).reason == MN_R_MENTIONS_OFF);
    CHECK(mn_decide(MN_CHANNEL, MN_MODE_ALL, &sw, false, true, false).verdict == MN_POST); // All needs no mention
    sw = ON;

    // delivery failed: own switch, mode ignored, still skipped while the chat is open
    CHECK(mn_decide(MN_FAILED, MN_MODE_OFF, &sw, false, false, false).verdict == MN_POST);
    CHECK(mn_decide(MN_FAILED, MN_MODE_ALL, &sw, true, false, false).reason == MN_R_CHAT_OPEN);
    sw.fail = false;
    CHECK(mn_decide(MN_FAILED, MN_MODE_ALL, &sw, false, false, false).reason == MN_R_FAIL_OFF);
}

static void defaults(void) {
    mesh_chat_key_t k;
    mesh_chat_key_channel(&k, "Public");
    CHECK(mn_default_mode(&k) == MN_MODE_MENTIONS);
    mesh_chat_key_channel(&k, "#badgetest");
    CHECK(mn_default_mode(&k) == MN_MODE_ALL);
    uint8_t peer[6] = {1, 2, 3, 4, 5, 6};
    mesh_chat_key_dm(&k, peer);
    CHECK(mn_default_mode(&k) == MN_MODE_ALL);
    CHECK(mn_counts_unread(&k, MN_MODE_OFF)); // DMs always count
    mesh_chat_key_channel(&k, "public");
    CHECK(!mn_counts_unread(&k, MN_MODE_MENTIONS));
    CHECK(!mn_counts_unread(&k, MN_MODE_OFF));
    CHECK(mn_counts_unread(&k, MN_MODE_ALL));
}

static void mentions(void) {
    CHECK(mn_is_mention("Bob: hi @[Badge] there", "Badge"));
    CHECK(mn_is_mention("Bob: hi @badge, there", "Badge"));
    CHECK(mn_is_mention("Bob: @BADGE", "badge"));
    CHECK(mn_is_mention("Bob: hello @Lucas van S.", "Lucas van S."));
    CHECK(!mn_is_mention("Bob: hi @badgeX", "Badge"));      // not a whole word
    CHECK(!mn_is_mention("Bob: hi @badge_2", "Badge"));
    CHECK(!mn_is_mention("Bob: hi badge", "Badge"));        // the bare name is not a mention
    CHECK(!mn_is_mention("Bob: hi @[Badge2]", "Badge"));
    CHECK(!mn_is_mention("@Badge: hi", "Badge"));           // in the sender part
    CHECK(!mn_is_mention("Bob: hi @", "Badge"));
    CHECK(!mn_is_mention("Bob: hi @Badge", ""));
    CHECK(!mn_is_mention("Bob: hi @Badge", "  "));
    CHECK(mn_is_mention("Bob: hi @Badge", " Badge "));      // trimmed
    CHECK(mn_is_mention("no sender @Badge", "Badge"));      // no ": " at all: the whole line
    CHECK(!mn_is_mention("Bob: @Badge\xC3\xA9", "Badge"));  // a UTF-8 letter continues the word
    CHECK(mn_is_mention("Bob: x @nobody @badge", "Badge")); // a later one
}

static void targets(void) {
    char            t[64];
    mesh_chat_key_t k, back;
    bool            failed;
    uint8_t         peer[6] = {0x2a, 0xe9, 0x6c, 0x65, 0x7d, 0x03};

    mesh_chat_key_channel(&k, "#BadgeTest");
    CHECK(mn_target(MN_CHANNEL, &k, t, sizeof(t)) == strlen("ch:#badgetest") && strcmp(t, "ch:#badgetest") == 0);
    CHECK(mn_target_parse(t, &back, &failed) && !failed && back.kind == MESH_CHAT_CHANNEL &&
          strcmp(back.chan, "#badgetest") == 0);
    CHECK(mn_target(MN_FAILED, &k, t, sizeof(t)) && strcmp(t, "fc:#badgetest") == 0);
    CHECK(mn_target_parse(t, &back, &failed) && failed && back.kind == MESH_CHAT_CHANNEL);

    mesh_chat_key_dm(&k, peer);
    CHECK(mn_target(MN_DM, &k, t, sizeof(t)) && strcmp(t, "dm:2ae96c657d03") == 0);
    CHECK(mn_target_parse(t, &back, &failed) && !failed && back.kind == MESH_CHAT_DM &&
          memcmp(back.id, peer, 6) == 0);
    CHECK(mn_target(MN_FAILED, &k, t, sizeof(t)) && strcmp(t, "fd:2ae96c657d03") == 0);
    CHECK(mn_target_parse(t, &back, &failed) && failed && memcmp(back.id, peer, 6) == 0);

    // a long channel name is cut so the target fits what lichen_notify keeps (32 bytes with the NUL)
    mesh_chat_key_channel(&k, "#a-channel-name-of-thirty-one-b");
    CHECK(mn_target(MN_FAILED, &k, t, sizeof(t)) < MN_TARGET_MAX);
    char small[MN_TARGET_MAX];
    CHECK(mn_target(MN_CHANNEL, &k, small, sizeof(small)) == MN_TARGET_MAX - 1);

    CHECK(!mn_target_parse("", &back, &failed));
    CHECK(!mn_target_parse("xx:foo", &back, &failed));
    CHECK(!mn_target_parse("dm:zz", &back, &failed));
    CHECK(!mn_target_parse("dm:2ae96c657d0", &back, &failed));
    CHECK(!mn_target_parse("fx:foo", &back, &failed));
}

static void cache(void) {
    mn_cache_t      c;
    mn_mode_t       mode;
    mesh_chat_key_t a, b;
    memset(&c, 0, sizeof(c));
    mesh_chat_key_channel(&a, "public");
    mesh_chat_key_channel(&b, "#x");
    CHECK(!mn_cache_lookup(&c, &a, &mode));
    mn_cache_store(&c, &a, MN_MODE_OFF);
    CHECK(mn_cache_lookup(&c, &a, &mode) && mode == MN_MODE_OFF);
    mn_cache_store(&c, &a, MN_MODE_ALL);
    CHECK(mn_cache_lookup(&c, &a, &mode) && mode == MN_MODE_ALL && c.n == 1);
    CHECK(!mn_cache_lookup(&c, &b, &mode));
    // full: round robin, never past the table
    for (int i = 0; i < MN_CACHE_MAX + 10; i++) {
        char name[16];
        snprintf(name, sizeof(name), "#c%d", i);
        mesh_chat_key_channel(&b, name);
        mn_cache_store(&c, &b, MN_MODE_OFF);
    }
    CHECK(c.n == MN_CACHE_MAX);
    CHECK(mn_cache_lookup(&c, &b, &mode)); // the newest is in
    char k1[10], k2[10];
    mn_nvs_key(&a, k1);
    mn_nvs_key(&b, k2);
    CHECK(strlen(k1) == 9 && k1[0] == 'm' && strcmp(k1, k2) != 0);
    // DM and channel never share a hash by construction of the kind byte
    uint8_t peer[6] = {0};
    mesh_chat_key_dm(&b, peer);
    CHECK(mn_key_hash(&b) != mn_key_hash(&a));
}

static void queue(void) {
    mn_queue_t      q;
    mn_note_t       out[MN_QUEUE_MAX];
    mesh_chat_key_t k, other;
    memset(&q, 0, sizeof(q));
    mesh_chat_key_channel(&k, "#a");
    mesh_chat_key_channel(&other, "#b");

    CHECK(mn_queue_push(&q, MN_CHANNEL, &k, "#a", "x: one", false));
    CHECK(mn_queue_push(&q, MN_CHANNEL, &k, "#a", "x: two", false));
    CHECK(q.n == 1 && q.note[0].count == 2 && strcmp(q.note[0].body, "x: two") == 0);
    // a failure in the same chat is its own note
    CHECK(mn_queue_push(&q, MN_FAILED, &k, "#a", "Not delivered", false));
    CHECK(q.n == 2);
    // a mention keeps its line against a plain one after it
    CHECK(mn_queue_push(&q, MN_CHANNEL, &k, "#a", "x: @[me] hi", true));
    CHECK(mn_queue_push(&q, MN_CHANNEL, &k, "#a", "x: plain", false));
    CHECK(q.note[0].mention && q.note[0].count == 4 && strcmp(q.note[0].body, "x: @[me] hi") == 0);

    // overflow: a full queue merges into a waiting chat but drops a new one
    memset(&q, 0, sizeof(q));
    for (int i = 0; i < MN_QUEUE_MAX; i++) {
        char name[8];
        snprintf(name, sizeof(name), "#c%d", i);
        mesh_chat_key_channel(&other, name);
        CHECK(mn_queue_push(&q, MN_CHANNEL, &other, name, "t", false));
    }
    CHECK(!mn_queue_push(&q, MN_CHANNEL, &k, "#a", "x", false) && q.lost == 1);
    mesh_chat_key_channel(&other, "#c3");
    CHECK(mn_queue_push(&q, MN_CHANNEL, &other, "#c3", "again", false) && q.note[3].count == 2);
    CHECK(mn_queue_take(&q, out, MN_QUEUE_MAX) == MN_QUEUE_MAX && q.n == 0);
    CHECK(strcmp(out[0].title, "#c0") == 0);

    // title and body are cut on a character boundary
    char big[400];
    memset(big, 'e', sizeof(big) - 1);
    big[sizeof(big) - 1] = 0;
    big[118] = (char)0xC3; // a 2-byte character that would not fit whole
    big[119] = (char)0xA9;
    memset(&q, 0, sizeof(q));
    CHECK(mn_queue_push(&q, MN_DM, &k, big, big, false));
    CHECK(strlen(q.note[0].title) < sizeof(q.note[0].title) && strlen(q.note[0].body) < sizeof(q.note[0].body));
    CHECK(strlen(q.note[0].body) == 118); // the é at 118..119 is not cut in half
}

static void failtrack(void) {
    mn_failtrack_t t;
    memset(&t, 0, sizeof(t));
    CHECK(!mn_failtrack_step(&t, 7, false));  // queued
    CHECK(mn_failtrack_step(&t, 7, true));    // failed: one note
    CHECK(!mn_failtrack_step(&t, 7, true));   // still failed: none
    CHECK(!mn_failtrack_step(&t, 7, false));  // a Tab retry
    CHECK(mn_failtrack_step(&t, 7, true));    // fails again: one more
    CHECK(mn_failtrack_step(&t, 9, true));    // first seen already failed (a refused channel send)
    CHECK(!mn_failtrack_step(&t, 9, true));
    for (uint32_t i = 100; i < 140; i++) { // the table is round-robin: old ids fall out, nothing crashes
        mn_failtrack_step(&t, i, i % 2);
    }
}

static void channel_match(void) {
    char const names[6][MESH_CHANNEL_MAX + 1] = {"Public", "Public Safety", "team-b", "team", "#a-long-channel-name-of-28-xyz",
                                                 "#a-long-channel-name-of-28-abc"};
    CHECK(mn_channel_match(names, 6, "public") == 0); // exact, not the longer one
    CHECK(mn_channel_match(names, 6, "team") == 3);   // team-b is listed first but is not team
    CHECK(mn_channel_match(names, 6, "team-b") == 2);
    CHECK(mn_channel_match(names, 6, "tea") == -1);   // not cut: no prefix match
    CHECK(mn_channel_match(names, 6, "gone") == -1);
    CHECK(mn_channel_match(names, 6, "#A-LONG-CHANNEL-NAME-OF-28-XYZ") == 4); // exact, without case
    // cut names: 28 bytes
    char const cut[3][MESH_CHANNEL_MAX + 1] = {"#a-long-channel-name-of-28-xy1", "#other", "#a-long-channel-name-of-28-xy2"};
    CHECK(mn_channel_match(cut, 2, "#a-long-channel-name-of") == -1);
    char key[40];
    snprintf(key, sizeof(key), "%.*s", MN_TARGET_NAME_MAX, "#a-long-channel-name-of-28-xy1");
    CHECK(strlen(key) == MN_TARGET_NAME_MAX);
    CHECK(mn_channel_match(cut, 2, key) == 0);  // unique prefix of a cut name
    CHECK(mn_channel_match(cut, 3, key) == -1); // two share the cut: ambiguous, the list opens
}

int main(void) {
    channel_match();
    decide_matrix();
    defaults();
    mentions();
    targets();
    cache();
    queue();
    failtrack();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("mesh_notify_test: ok\n");
    return 0;
}
