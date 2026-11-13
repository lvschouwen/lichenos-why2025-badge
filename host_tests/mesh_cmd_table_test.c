// Host test: MeshCore's console command tables (#208). Pins the top-level commands and the "fw mesh" subcommands (word,
// how it takes its arguments, whether MeshCore must run, the order they are tried in), so a drift in the console's
// names or arguments fails here before it changes a bench transcript.
#include "mesh_cmd_table.h"

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

// The arguments a subcommand takes, from its match rule
#define NONE     MESH_SUB_EXACT    // no argument
#define NEEDS    MESH_SUB_PREFIX   // at least one (the word ends in its space)
#define OPTIONAL MESH_SUB_WORD     // none or some, after a space
#define RAW      MESH_SUB_WORD_RAW // none or some, the handler gets the space too

static void test_console_commands(void) {
    static char const *const names[] = {"mesh", "contacts", "msgs", "channels", "channel", "advert"};
    CHECK(MESH_CON_COUNT == sizeof(names) / sizeof(names[0]));
    for (int i = 0; i < MESH_CON_COUNT; i++) {
        CHECK(strcmp(MESH_CONSOLE_CMDS[i].name, names[i]) == 0);
        // each usage starts with its own command, as "fw help" lists it
        CHECK(strncmp(MESH_CONSOLE_CMDS[i].usage, names[i], strlen(names[i])) == 0);
    }
    CHECK(strstr(MESH_CONSOLE_CMDS[MESH_CON_MESH].usage, "mesh notify [test dm|chan <name>|fail|mention]") != NULL);
    CHECK(strcmp(MESH_CONSOLE_CMDS[MESH_CON_MSGS].usage, "msgs [n] (last n messages)") == 0);
}

static void test_subcommand_table(void) {
    static struct {
        char const      *word;
        mesh_sub_match_t args;
        bool             running;
    } const want[] = {
        {"start", NONE, false},     {"stop", NONE, false},         {"autostart", OPTIONAL, false},
        {"hist", OPTIONAL, false},  {"notify", OPTIONAL, false},   {"order", RAW, false},
        {"name ", NEEDS, true},     {"radio", RAW, true},          {"pathhash", RAW, true},
        {"path ", NEEDS, true},     {"send ", NEEDS, true},        {"dm ", NEEDS, true},
        {"out", NONE, true},        {"trace", OPTIONAL, true},     {"discover ", NEEDS, true},
        {"telemetry", OPTIONAL, true}, {"history", OPTIONAL, true}, {"remote", OPTIONAL, true},
        {"link", NONE, true},       {"chats", NONE, true},         {"ident simulate", NONE, true},
        {"advert", OPTIONAL, true}, {"loc", OPTIONAL, true},       {"autoadd", OPTIONAL, true},
        {"scope", OPTIONAL, true},
    };
    CHECK(MESH_SUB_COUNT == sizeof(want) / sizeof(want[0]));
    for (int i = 0; i < MESH_SUB_COUNT; i++) {
        CHECK(MESH_SUBS[i].word && strcmp(MESH_SUBS[i].word, want[i].word) == 0);
        CHECK(MESH_SUBS[i].match == want[i].args);
        CHECK(MESH_SUBS[i].needs_running == want[i].running);
    }
    // the ones that answer while MeshCore is stopped come first: the dispatch relies on it
    for (int i = 1; i < MESH_SUB_COUNT; i++) {
        CHECK(MESH_SUBS[i - 1].needs_running <= MESH_SUBS[i].needs_running);
    }
}

// The id found for line and what its handler gets ("" compares as the empty rest)
static void expect(char const *line, mesh_sub_id_t id, char const *rest) {
    char const   *got_rest = "unset";
    mesh_sub_id_t got      = mesh_sub_find(line, &got_rest);
    if (got != id || (rest ? !got_rest || strcmp(got_rest, rest) != 0 : got_rest != NULL)) {
        printf("FAIL \"%s\": id %d rest \"%s\", want %d \"%s\"\n", line, (int)got, got_rest ? got_rest : "(null)",
               (int)id, rest ? rest : "(null)");
        failures++;
    }
}

static void test_matching(void) {
    expect("", MESH_SUB_COUNT, NULL); // "fw mesh" alone: the status
    expect("start", MESH_SUB_START, "");
    expect("start now", MESH_SUB_COUNT, NULL);
    expect("stop", MESH_SUB_STOP, "");
    expect("autostart", MESH_SUB_AUTOSTART, "");
    expect("autostart on", MESH_SUB_AUTOSTART, "on");
    expect("autostarts", MESH_SUB_COUNT, NULL);
    expect("hist", MESH_SUB_HIST, "");
    expect("hist scan 3 bad", MESH_SUB_HIST, "scan 3 bad");
    expect("history", MESH_SUB_HISTORY, ""); // not "hist"
    expect("history abcd on 30 YES", MESH_SUB_HISTORY, "abcd on 30 YES");
    expect("notify test dm", MESH_SUB_NOTIFY, "test dm");
    expect("order", MESH_SUB_ORDER, "");
    expect("order reset", MESH_SUB_ORDER, " reset"); // with its space
    expect("name Badge", MESH_SUB_NAME, "Badge");
    expect("name", MESH_SUB_COUNT, NULL); // needs an argument
    expect("radio", MESH_SUB_RADIO, "");
    expect("radio 3", MESH_SUB_RADIO, " 3");
    expect("pathhash 2", MESH_SUB_PATHHASH, " 2"); // before "path "
    expect("path abcdef", MESH_SUB_PATH, "abcdef");
    expect("send #badgetest hello there", MESH_SUB_SEND, "#badgetest hello there");
    expect("dm 0123456789ab hi", MESH_SUB_DM, "0123456789ab hi");
    expect("out", MESH_SUB_OUT, "");
    expect("out x", MESH_SUB_COUNT, NULL);
    expect("trace", MESH_SUB_TRACE, "");
    expect("trace 0a1b", MESH_SUB_TRACE, "0a1b");
    expect("discover 0a1b", MESH_SUB_DISCOVER, "0a1b");
    expect("discover", MESH_SUB_COUNT, NULL);
    expect("telemetry 0a1b YES", MESH_SUB_TELEMETRY, "0a1b YES");
    expect("remote rep login", MESH_SUB_REMOTE, "rep login");
    expect("link", MESH_SUB_LINK, "");
    expect("chats", MESH_SUB_CHATS, "");
    expect("ident simulate", MESH_SUB_IDENT_SIMULATE, "");
    expect("ident", MESH_SUB_COUNT, NULL);
    expect("advert interval 6h", MESH_SUB_ADVERT, "interval 6h");
    expect("loc 52.1 4.3", MESH_SUB_LOC, "52.1 4.3");
    expect("autoadd off", MESH_SUB_AUTOADD, "off");
    expect("scope console nl YES", MESH_SUB_SCOPE, "console nl YES");
    expect("bogus", MESH_SUB_COUNT, NULL);
}

int main(void) {
    test_console_commands();
    test_subcommand_table();
    test_matching();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("mesh_cmd_table_test: ok\n");
    return 0;
}
