// Host test (#438): "fw mesh chat open|close" parse and the channel-only decision
#include "mesh_chat_cmd.h"

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

static mesh_chat_cmd_t parse(char const *args, char *name) {
    name[0] = '\0';
    return mesh_chat_cmd_parse(args, name, MESH_CHAT_CMD_NAME_MAX);
}

int main(void) {
    char n[MESH_CHAT_CMD_NAME_MAX];
    CHECK(parse("open #badgetest", n) == MESH_CHAT_CMD_OPEN && strcmp(n, "#badgetest") == 0);
    CHECK(parse("  open   badgetest  ", n) == MESH_CHAT_CMD_OPEN && strcmp(n, "badgetest") == 0);
    CHECK(parse("close", n) == MESH_CHAT_CMD_CLOSE);
    CHECK(parse(" close ", n) == MESH_CHAT_CMD_CLOSE);
    CHECK(parse("close now", n) == MESH_CHAT_CMD_USAGE);
    CHECK(parse("", n) == MESH_CHAT_CMD_USAGE);
    CHECK(parse("open", n) == MESH_CHAT_CMD_USAGE);
    CHECK(parse("open  ", n) == MESH_CHAT_CMD_USAGE);
    CHECK(parse("open #a #b", n) == MESH_CHAT_CMD_USAGE);
    CHECK(parse("OPEN #a", n) == MESH_CHAT_CMD_USAGE);
    CHECK(parse("opened #a", n) == MESH_CHAT_CMD_USAGE);
    CHECK(parse("closed", n) == MESH_CHAT_CMD_USAGE);
    // the channel list refuses a contact (@name, key digits); the parse leaves them to it
    CHECK(parse("open @bob", n) == MESH_CHAT_CMD_OPEN && strcmp(n, "@bob") == 0);
    CHECK(parse("open abc", n) == MESH_CHAT_CMD_OPEN); // short hex: a channel name, the list decides
    CHECK(parse("open #abcdef123456", n) == MESH_CHAT_CMD_OPEN);
    // a name too long for a channel
    CHECK(parse("open #0123456789012345678901234567890123", n) == MESH_CHAT_CMD_USAGE);
    CHECK(parse("open #012345678901234567890123456789", n) == MESH_CHAT_CMD_OPEN); // 31 bytes
    if (failures == 0) {
        printf("mesh_chat_cmd_test: ok\n");
    }
    return failures ? 1 : 0;
}
