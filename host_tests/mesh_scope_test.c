// Host test: the default flood scope's name rules and texts (#163)
#include "mesh_scope.h"

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

static char const *parse(char const *in, char *out) {
    return mesh_scope_parse(in, out, 40);
}

// The channel scope table (#291): put, find, drop, and the file text both ways
static void chscope_checks(void) {
    static mesh_chscope_t t, back;
    static char           text[MESH_CHSCOPE_TEXT_MAX], line[200];
    bool                  whole;
    // The senders' name hash: as channel_same_name compares (ASCII case), FNV-1a
    CHECK(mesh_chscope_name_hash("#ZZ-Chan") == mesh_chscope_name_hash("#zz-chan"));
    CHECK(mesh_chscope_name_hash("#zz-chan") != mesh_chscope_name_hash("#zz-chan2"));
    CHECK(mesh_chscope_name_hash("") == 2166136261u && mesh_chscope_name_hash("a") == 0xe40c292cu);
    uint8_t               a[MESH_CHSCOPE_ID] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef};
    uint8_t               b[MESH_CHSCOPE_ID] = {0xff, 0, 0, 0, 0, 0, 0, 0x10};
    mesh_chscope_clear(&t);
    CHECK(mesh_chscope_find(&t, a)[0] == 0 && mesh_chscope_write(&t, text, sizeof(text)) == 0 && text[0] == 0);
    CHECK(mesh_chscope_put(&t, a, "zz-local") && strcmp(mesh_chscope_find(&t, a), "zz-local") == 0);
    CHECK(mesh_chscope_find(&t, b)[0] == 0);
    CHECK(mesh_chscope_put(&t, a, "zz-other") && t.n == 1 && strcmp(mesh_chscope_find(&t, a), "zz-other") == 0);
    CHECK(mesh_chscope_put(&t, b, "zz-test") && t.n == 2);
    CHECK(mesh_chscope_write(&t, text, sizeof(text)) == strlen(text));
    CHECK(strcmp(text, "0123456789abcdef zz-other\nff00000000000010 zz-test\n") == 0);
    CHECK(mesh_chscope_write(&t, line, 20) == 0 && line[0] == 0); // too small: nothing half written
    CHECK(mesh_chscope_read(&back, text, &whole) == 2 && whole);
    CHECK(back.n == 2 && strcmp(mesh_chscope_find(&back, a), "zz-other") == 0);
    CHECK(strcmp(mesh_chscope_find(&back, b), "zz-test") == 0);
    // "" drops it; dropping one that is not there is fine
    CHECK(mesh_chscope_put(&t, a, "") && t.n == 1 && mesh_chscope_find(&t, a)[0] == 0);
    CHECK(mesh_chscope_put(&t, a, "") && t.n == 1 && strcmp(mesh_chscope_find(&t, b), "zz-test") == 0);
    // A line without a readable id names no channel and is left out; CRLF and upper-case hex are fine. An id with a
    // region that does not read is a tombstone (that channel's screen sends are refused), whatever the region's damage
    CHECK(mesh_chscope_read(&back, "0123456789ABCDEF zz-local\r\nnot a line\n0123456789abcdeg zz-bad\n"
                                   "ff00000000000010 zz bad\n0011 zz-short\n", &whole) == 2 && !whole);
    CHECK(back.n == 2 && strcmp(mesh_chscope_find(&back, a), "zz-local") == 0);
    CHECK(strcmp(mesh_chscope_find(&back, b), MESH_CHSCOPE_DAMAGED) == 0);
    char const *tombs[] = {"ff00000000000010\n", "ff00000000000010 \n", "ff00000000000010_zz-test\n",
                           "ff00000000000010 abcdefghijklmnopqrstuvwxyz01234\n", "ff00000000000010 $zz\n"};
    for (size_t i = 0; i < sizeof(tombs) / sizeof(tombs[0]); i++) {
        CHECK(mesh_chscope_read(&back, tombs[i], &whole) == 1 && whole); // read whole: the tombstone holds it
        CHECK(strcmp(mesh_chscope_find(&back, b), MESH_CHSCOPE_DAMAGED) == 0);
    }
    // A cut write: the last line without its line end may be shorter than written, so never read as a region
    CHECK(mesh_chscope_read(&back, "0123456789abcdef zz-local\nff00000000000010 zz-te", &whole) == 2 && !whole);
    CHECK(strcmp(mesh_chscope_find(&back, a), "zz-local") == 0);
    CHECK(strcmp(mesh_chscope_find(&back, b), MESH_CHSCOPE_DAMAGED) == 0);
    // Unreadable as a whole (#291 review): empty (a 0-byte file), nothing readable, an id-less line among good ones,
    // a short or id-less last line without its line end
    CHECK(mesh_chscope_read(&back, "", &whole) == 0 && back.n == 0 && !whole);
    CHECK(mesh_chscope_read(&back, "garbage\n\n", &whole) == 0 && back.n == 0 && !whole);
    CHECK(mesh_chscope_read(&back, "0123456789abcdef zz-local\nzz\n", &whole) == 1 && !whole);
    CHECK(mesh_chscope_read(&back, "0123456789abcdef zz-local\n0123", &whole) == 1 && !whole);
    CHECK(mesh_chscope_read(&back, "0123456789abcdef zz-local\nff00000000000010 zz-test", &whole) == 2 && !whole);
    // Read whole: blank lines and CRLF are fine
    CHECK(mesh_chscope_read(&back, "\n0123456789abcdef zz-local\r\n\n", &whole) == 1 && whole);
    // Which file a start reads: the file when it is there (a temp file next to it is a cut before the rename), the
    // temp file promoted when only it is (cut between the remove and the rename), none at the first start
    CHECK(mesh_chscope_file_pick(true, false) == MESH_CHSCOPE_FILE_READ);
    CHECK(mesh_chscope_file_pick(true, true) == MESH_CHSCOPE_FILE_READ);
    CHECK(mesh_chscope_file_pick(false, true) == MESH_CHSCOPE_FILE_PROMOTE);
    CHECK(mesh_chscope_file_pick(false, false) == MESH_CHSCOPE_FILE_NONE);
    // Full: the 41st channel is refused, an existing one can still change
    mesh_chscope_clear(&t);
    for (int i = 0; i < MESH_CHSCOPE_MAX; i++) {
        uint8_t id[MESH_CHSCOPE_ID] = {(uint8_t)i, 1};
        CHECK(mesh_chscope_put(&t, id, "abcdefghijklmnopqrstuvwxyz0123"));
    }
    uint8_t extra[MESH_CHSCOPE_ID] = {0xee};
    uint8_t first[MESH_CHSCOPE_ID] = {0, 1};
    CHECK(!mesh_chscope_put(&t, extra, "zz-test") && mesh_chscope_put(&t, first, "zz-test"));
    CHECK(mesh_chscope_put(&t, first, "abcdefghijklmnopqrstuvwxyz0123"));
    CHECK(mesh_chscope_write(&t, text, sizeof(text)) == sizeof(text) - 1); // the longest table fits exactly
    CHECK(mesh_chscope_read(&back, text, &whole) == MESH_CHSCOPE_MAX && whole);
    CHECK(back.n == MESH_CHSCOPE_MAX);
    // One line more than the table holds: not whole (a line left out)
    static char more[MESH_CHSCOPE_TEXT_MAX + 64];
    snprintf(more, sizeof(more), "%seeeeeeeeeeeeeee zz-test\n", text);
    CHECK(mesh_chscope_read(&back, more, &whole) == MESH_CHSCOPE_MAX && !whole);
    mesh_chscope_drop_at(&t, 0);
    CHECK(t.n == MESH_CHSCOPE_MAX - 1 && mesh_chscope_find(&t, first)[0] == 0);
    // The details row and the question
    mesh_scope_channel_text("zz-local", "zz-wide", line, sizeof(line));
    CHECK(strcmp(line, "zz-local") == 0);
    mesh_scope_channel_text("", "zz-wide", line, sizeof(line));
    CHECK(strcmp(line, "Default (zz-wide)") == 0);
    mesh_scope_channel_text(NULL, "", line, sizeof(line));
    CHECK(strcmp(line, "Default (None, everywhere)") == 0);
    mesh_scope_channel_text(MESH_CHSCOPE_DAMAGED, "zz-wide", line, sizeof(line));
    CHECK(strcmp(line, "Damaged: set it again") == 0);
    mesh_scope_channel_ask_text("#zz-chan", "zz-local", line, sizeof(line));
    CHECK(strcmp(line, "Messages you send on #zz-chan are sent to region zz-local. Repeaters outside it do not pass "
                       "them on, so members who only reach you through them stop receiving them.") == 0);
}

int main(void) {
    char out[40], text[200];
    // None: empty (also spaces, a lone '#') is no scope
    CHECK(parse("", out) == NULL && out[0] == 0);
    CHECK(parse("   ", out) == NULL && out[0] == 0);
    CHECK(parse("#", out) == NULL && out[0] == 0);
    // A name: lowercased, '#' and spaces gone
    CHECK(parse("NL-Limburg", out) == NULL && strcmp(out, "nl-limburg") == 0);
    CHECK(parse("  #eu  ", out) == NULL && strcmp(out, "eu") == 0);
    CHECK(parse("caf\xc3\xa9", out) == NULL && strcmp(out, "caf\xc3\xa9") == 0);
    // Refused: out is left empty
    CHECK(parse("a b", out) != NULL && out[0] == 0);
    CHECK(parse("a,b", out) != NULL && parse("a:b", out) != NULL && parse("a?b", out) != NULL);
    CHECK(parse("$private", out) != NULL && parse("a#b", out) != NULL && parse("a_b", out) != NULL);
    CHECK(parse("a[b", out) != NULL && parse("a\\b", out) != NULL && parse("a*b", out) != NULL);
    // 30 characters fit, 31 do not; a small buffer never overflows
    CHECK(parse("abcdefghijklmnopqrstuvwxyz0123", out) == NULL && strlen(out) == 30);
    CHECK(parse("abcdefghijklmnopqrstuvwxyz01234", out) != NULL);
    CHECK(mesh_scope_parse("abcdef", out, 4) != NULL && out[0] == 0);
    CHECK(mesh_scope_parse("abc", out, 4) == NULL && strcmp(out, "abc") == 0);
    // Texts
    mesh_scope_text("", text, sizeof(text));
    CHECK(strcmp(text, "None (everywhere)") == 0);
    mesh_scope_text(NULL, text, sizeof(text));
    CHECK(strcmp(text, "None (everywhere)") == 0);
    mesh_scope_text("nl", text, sizeof(text));
    CHECK(strcmp(text, "nl") == 0);
    mesh_scope_ask_text("nl", text, sizeof(text));
    CHECK(strcmp(text, "Adverts and messages will only travel in region nl. Contacts outside it can no longer reach you.") == 0);
    mesh_scope_key_text("nl", text, sizeof(text));
    CHECK(strcmp(text, "#nl") == 0);
    // The console scope (#290): a screen's flood keeps the default, a console line's takes the console scope, and an
    // unset console scope refuses it (never "None (everywhere)")
    CHECK(mesh_scope_choose(false, "", NULL) == MESH_SCOPE_USE_DEFAULT);
    CHECK(mesh_scope_choose(false, "zz-test", NULL) == MESH_SCOPE_USE_DEFAULT);
    CHECK(mesh_scope_choose(true, "zz-test", NULL) == MESH_SCOPE_USE_CONSOLE);
    CHECK(mesh_scope_choose(true, "", NULL) == MESH_SCOPE_USE_REFUSE);
    CHECK(mesh_scope_choose(true, NULL, NULL) == MESH_SCOPE_USE_REFUSE);
    // A channel's own scope (#291): a screen's channel message takes it over the default; a console line's keeps the
    // console scope (or is refused) whatever the channel has; no own scope ("" or NULL) is the default
    CHECK(mesh_scope_choose(false, "", "zz-local") == MESH_SCOPE_USE_CHANNEL);
    CHECK(mesh_scope_choose(false, "zz-test", "zz-local") == MESH_SCOPE_USE_CHANNEL);
    CHECK(mesh_scope_choose(false, "zz-test", "") == MESH_SCOPE_USE_DEFAULT);
    CHECK(mesh_scope_choose(true, "zz-test", "zz-local") == MESH_SCOPE_USE_CONSOLE);
    CHECK(mesh_scope_choose(true, "", "zz-local") == MESH_SCOPE_USE_REFUSE);
    CHECK(mesh_scope_choose(true, NULL, "zz-local") == MESH_SCOPE_USE_REFUSE);
    // A damaged channel scope refuses the screens' messages (never the default: wider than meant); the console's
    // keep the console scope
    CHECK(mesh_scope_choose(false, "", MESH_CHSCOPE_DAMAGED) == MESH_SCOPE_USE_DAMAGED);
    CHECK(mesh_scope_choose(false, "zz-test", MESH_CHSCOPE_DAMAGED) == MESH_SCOPE_USE_DAMAGED);
    CHECK(mesh_scope_choose(true, "zz-test", MESH_CHSCOPE_DAMAGED) == MESH_SCOPE_USE_CONSOLE);
    CHECK(mesh_scope_choose(true, "", MESH_CHSCOPE_DAMAGED) == MESH_SCOPE_USE_REFUSE);
    CHECK(mesh_scope_parse(MESH_CHSCOPE_DAMAGED, text, sizeof(text)) != NULL); // never a region anyone can set
    // "fw mesh scope console <region> YES"
    CHECK(mesh_scope_console_arg("ZZ-Test YES", out, sizeof(out)) == NULL && strcmp(out, "zz-test") == 0);
    CHECK(mesh_scope_console_arg("#zz-test  YES ", out, sizeof(out)) == NULL && strcmp(out, "zz-test") == 0);
    CHECK(mesh_scope_console_arg("zz-test", out, sizeof(out)) != NULL && out[0] == 0);
    CHECK(mesh_scope_console_arg("zz-test yes", out, sizeof(out)) != NULL && out[0] == 0);
    CHECK(mesh_scope_console_arg("zz-testYES", out, sizeof(out)) != NULL && out[0] == 0);
    CHECK(mesh_scope_console_arg("YES", out, sizeof(out)) != NULL && out[0] == 0);
    CHECK(mesh_scope_console_arg("# YES", out, sizeof(out)) != NULL && out[0] == 0);
    CHECK(mesh_scope_console_arg("zz test YES", out, sizeof(out)) != NULL && out[0] == 0);
    CHECK(mesh_scope_console_arg("abcdefghijklmnopqrstuvwxyz0123 YES", out, sizeof(out)) == NULL && strlen(out) == 30);
    CHECK(mesh_scope_console_arg("abcdefghijklmnopqrstuvwxyz01234 YES", out, sizeof(out)) != NULL && out[0] == 0);
    CHECK(mesh_scope_console_arg("abcdefghijklmnopqrstuvwxyz0123456789 YES", out, sizeof(out)) != NULL);
    CHECK(mesh_scope_console_arg("", out, sizeof(out)) != NULL && out[0] == 0);
    mesh_scope_status_text("", "", text, sizeof(text));
    CHECK(strcmp(text, "default None | console not set") == 0);
    mesh_scope_status_text("zz-wide", "zz-test", text, sizeof(text));
    CHECK(strcmp(text, "default zz-wide | console zz-test") == 0);
    mesh_scope_status_text(NULL, NULL, text, sizeof(text));
    CHECK(strcmp(text, "default None | console not set") == 0);
    chscope_checks();
    if (failures == 0) {
        printf("mesh_scope_test: ok\n");
    }
    return failures ? 1 : 0;
}
