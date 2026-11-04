// Host test: the `fw keys send` tokenizer
#include "ui_keys.h"

#include <stdio.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

int main(void) {
    uint32_t k[16];
    size_t   pos = 99;
    CHECK(ui_keys_parse("down down enter", k, 16, &pos) == 3);
    CHECK(k[0] == 18 && k[1] == 18 && k[2] == 10); // LV_KEY_DOWN, LV_KEY_ENTER
    CHECK(ui_keys_parse("text:Ab c", k, 16, &pos) == 4);
    CHECK(k[0] == 'A' && k[1] == 'b' && k[2] == ' ' && k[3] == 'c');
    CHECK(ui_keys_parse("text:Hi|enter esc", k, 16, &pos) == 4);
    CHECK(k[1] == 'i' && k[2] == 10 && k[3] == 27);
    CHECK(ui_keys_parse("up left right tab bksp wait", k, 16, &pos) == 6);
    CHECK(k[0] == 17 && k[1] == 20 && k[2] == 19 && k[3] == 9 && k[4] == 8 && k[5] == UI_KEY_WAIT);
    CHECK(ui_keys_parse("", k, 16, &pos) == 0);
    CHECK(ui_keys_parse("  enter  ", k, 16, &pos) == 1);
    CHECK(ui_keys_parse("down jump enter", k, 16, &pos) == UI_KEYS_ERR && pos == 5);
    CHECK(ui_keys_parse("entr", k, 16, &pos) == UI_KEYS_ERR && pos == 0);
    CHECK(ui_keys_parse("up text:abcd", k, 3, &pos) == UI_KEYS_ERR); // does not fit
    // Focus guards (#190): one key each, the text up to '|'
    char const *seq = "down row:Set date & time|enter focus:Chat|enter";
    CHECK(ui_keys_parse(seq, k, 16, &pos) == 5);
    CHECK(k[0] == 18 && UI_KEY_IS_GUARD(k[1]) && k[2] == 10 && UI_KEY_IS_GUARD(k[3]) && k[4] == 10);
    CHECK(!UI_KEY_IS_GUARD(k[0]) && !UI_KEY_IS_GUARD(k[2]) && !UI_KEY_IS_GUARD(UI_KEY_WAIT));
    CHECK(!UI_KEY_IS_GUARD(UI_KEY_SHAPE(2)) && !UI_KEY_IS_GUARD('z'));
    CHECK(ui_keys_guard_ok(k[1], seq, "Set date & time"));
    CHECK(ui_keys_guard_ok(k[1], seq, "* Set date & time "));
    CHECK(!ui_keys_guard_ok(k[1], seq, "Set date & time zone"));
    CHECK(!ui_keys_guard_ok(k[1], seq, "Set date"));
    CHECK(ui_keys_guard_ok(k[3], seq, "Chat"));
    CHECK(ui_keys_guard_ok(k[3], seq, "Chats (3 unread)"));
    CHECK(!ui_keys_guard_ok(k[3], seq, "Contacts"));
    CHECK(!ui_keys_guard_ok(k[3], seq, ""));
    CHECK(!ui_keys_guard_ok(k[3], seq, "(none)"));
    CHECK(!ui_keys_guard_ok(k[3], seq, NULL));
    CHECK(!ui_keys_guard_ok(18, seq, "Chat")); // not a guard
    CHECK(ui_keys_parse("row:", k, 16, &pos) == UI_KEYS_ERR && pos == 0); // nothing to compare with
    CHECK(ui_keys_parse("down focus:|enter", k, 16, &pos) == UI_KEYS_ERR && pos == 5);
    CHECK(ui_keys_parse("down row:Wi-Fi", k, 1, &pos) == UI_KEYS_ERR && pos == 5); // does not fit
    // needlock and notfocus (#190)
    char const *any = "needlock notfocus:Remove;Send advert;Add |enter";
    CHECK(ui_keys_parse(any, k, 16, &pos) == 3);
    CHECK(k[0] == UI_KEY_NEEDLOCK && !UI_KEY_IS_GUARD(k[0]) && !UI_KEY_IS_SHAPE(k[0]));
    CHECK(UI_KEY_IS_GUARD(k[1]) && k[2] == 10);
    CHECK(ui_keys_guard_ok(k[1], any, "Alice"));
    CHECK(ui_keys_guard_ok(k[1], any, "Address book")); // "Add " with its space: not in "Address"
    CHECK(!ui_keys_guard_ok(k[1], any, "Remove"));
    CHECK(!ui_keys_guard_ok(k[1], any, "* Send advert now"));
    CHECK(!ui_keys_guard_ok(k[1], any, "Add channel"));
    CHECK(!ui_keys_guard_ok(k[1], any, "(none)"));
    CHECK(!ui_keys_guard_ok(k[1], any, ""));
    CHECK(ui_keys_parse("notfocus:|enter", k, 16, &pos) == UI_KEYS_ERR);
    if (failures) {
        return 1;
    }
    printf("ui_keys_test: ok\n");
    return 0;
}
