// Host test: who a chat screen sends to, and what opening a chat does with the one on top (#126)
#include "mesh_chat_target.h"

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

static uint8_t const BOB[6]   = {0xb0, 0xb0, 1, 2, 3, 4};
static uint8_t const CAROL[6] = {0xca, 0x40, 1, 2, 3, 4};

int main(void) {
    mesh_chat_target_t a, b;

    // Channels: the name is kept as given (it is sent with it), the key ignores case
    mesh_chat_target_channel(&a, "#BadgeTest");
    CHECK(!a.dm && strcmp(a.title, "#BadgeTest") == 0 && a.key.kind == MESH_CHAT_CHANNEL);
    CHECK(strcmp(a.key.chan, "#badgetest") == 0);
    mesh_chat_target_channel(&b, "#badgetest");
    CHECK(mesh_chat_target_same(&a, &b));
    mesh_chat_target_channel(&b, "Public");
    CHECK(!mesh_chat_target_same(&a, &b));

    // DMs: the title gets its "@", the key is the prefix
    mesh_chat_target_dm(&a, BOB, "bob");
    CHECK(a.dm && strcmp(a.title, "@bob") == 0 && memcmp(a.peer, BOB, 6) == 0 && a.key.kind == MESH_CHAT_DM);
    CHECK(memcmp(a.key.id, BOB, 6) == 0);
    mesh_chat_target_dm(&b, BOB, "@bob");
    CHECK(strcmp(b.title, "@bob") == 0 && mesh_chat_target_same(&a, &b));
    mesh_chat_target_dm(&b, BOB, "bob (renamed)"); // the same contact under another name is the same chat
    CHECK(mesh_chat_target_same(&a, &b));
    mesh_chat_target_dm(&b, CAROL, "bob"); // another contact with the same name is not
    CHECK(!mesh_chat_target_same(&a, &b));
    mesh_chat_target_dm(&b, CAROL, "");
    CHECK(strcmp(b.title, "@(no name)") == 0);
    mesh_chat_target_dm(&b, CAROL, NULL);
    CHECK(strcmp(b.title, "@(no name)") == 0);

    // A DM and a channel never match, whatever their titles
    mesh_chat_target_channel(&b, "@bob");
    CHECK(!mesh_chat_target_same(&a, &b));

    // A long name is cut, never overflows
    char long_name[200];
    memset(long_name, 'x', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = '\0';
    mesh_chat_target_dm(&b, CAROL, long_name);
    CHECK(strlen(b.title) == MESH_CHAT_TITLE_MAX - 1 && b.title[0] == '@');

    // Channel names at MeshCore's limit (31 chars) are kept whole and still keyed; a longer one is cut, never overflows
    char chan31[MESH_CHANNEL_MAX + 1], chan40[41];
    memset(chan31, 'c', MESH_CHANNEL_MAX);
    chan31[MESH_CHANNEL_MAX] = '\0';
    mesh_chat_target_channel(&a, chan31);
    CHECK(strcmp(a.title, chan31) == 0 && strlen(a.key.chan) == MESH_CHANNEL_MAX);
    CHECK(strncmp(a.key.chan, chan31, MESH_CHANNEL_MAX) == 0);
    memset(chan40, 'c', 40);
    chan40[40] = '\0';
    mesh_chat_target_channel(&b, chan40);
    CHECK(strlen(b.title) < sizeof(b.title) && b.key.kind == MESH_CHAT_CHANNEL && strlen(b.key.chan) <= MESH_CHANNEL_MAX);
    // NULL or empty: an empty title, a channel key all the same (no crash)
    mesh_chat_target_channel(&a, NULL);
    CHECK(a.title[0] == '\0' && a.key.kind == MESH_CHAT_CHANNEL && !a.dm);
    mesh_chat_target_channel(&b, "");
    CHECK(b.title[0] == '\0' && mesh_chat_target_same(&a, &b));

    // Opening: nothing chat-like on top pushes; the same chat keeps (the draft stays); another chat is replaced
    // (the bug of #126: the DM with bob on top, a notice for carol must not leave bob's screen sending to carol)
    mesh_chat_target_t bob, carol, chan, chan_upper;
    mesh_chat_target_dm(&bob, BOB, "bob");
    mesh_chat_target_dm(&carol, CAROL, "carol");
    mesh_chat_target_channel(&chan, "#badgetest");
    mesh_chat_target_channel(&chan_upper, "#BADGETEST");
    CHECK(mesh_chat_open_how(NULL, &bob) == MESH_CHAT_OPEN_PUSH);
    CHECK(mesh_chat_open_how(&bob, &bob) == MESH_CHAT_OPEN_KEEP);
    CHECK(mesh_chat_open_how(&bob, &carol) == MESH_CHAT_OPEN_REPLACE);
    CHECK(mesh_chat_open_how(&chan, &chan_upper) == MESH_CHAT_OPEN_KEEP);
    CHECK(mesh_chat_open_how(&chan, &bob) == MESH_CHAT_OPEN_REPLACE);
    CHECK(mesh_chat_open_how(&bob, &chan) == MESH_CHAT_OPEN_REPLACE);

    // Showing the Chat list (#147): a notice whose chat is gone must not stack a second list
    CHECK(mesh_chats_show_how(-1) == MESH_CHATS_SHOW_OPEN);
    CHECK(mesh_chats_show_how(0) == MESH_CHATS_SHOW_KEEP);
    CHECK(mesh_chats_show_how(3) == MESH_CHATS_SHOW_BACK);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("mesh_chat_target_test: all passed\n");
    return 0;
}
