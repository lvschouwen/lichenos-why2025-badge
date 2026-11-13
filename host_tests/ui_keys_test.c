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
    uint32_t k[32];
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
    // hold:<ms> and phys (#375): the hold goes right after its key, over a phys in between; capped at 10 s
    CHECK(ui_keys_parse("esc phys hold:3000", k, 16, &pos) == 3);
    CHECK(k[0] == 27 && UI_KEY_IS_HOLD(k[1]) && UI_KEY_HOLD_MS(k[1]) == 3000 && k[2] == UI_KEY_PHYS);
    CHECK(ui_keys_parse("esc hold:1500 bksp hold:1300", k, 16, &pos) == 4);
    CHECK(k[0] == 27 && UI_KEY_HOLD_MS(k[1]) == 1500 && k[2] == 8 && UI_KEY_HOLD_MS(k[3]) == 1300);
    CHECK(ui_keys_parse("enter hold:20000", k, 16, &pos) == 2 && UI_KEY_HOLD_MS(k[1]) == UI_KEY_HOLD_MAX_MS);
    CHECK(ui_keys_parse("enter hold:99999999999999999999", k, 16, &pos) == 2 && UI_KEY_HOLD_MS(k[1]) == 10000);
    CHECK(ui_keys_parse("enter hold:10000", k, 16, &pos) == 2 && UI_KEY_HOLD_MS(k[1]) == 10000);
    CHECK(ui_keys_parse("enter hold:0", k, 16, &pos) == 2 && UI_KEY_IS_HOLD(k[1]) && UI_KEY_HOLD_MS(k[1]) == 0);
    CHECK(ui_keys_parse("text:ab|hold:500", k, 16, &pos) == 3 && k[1] == 'b' && UI_KEY_HOLD_MS(k[2]) == 500);
    CHECK(ui_keys_parse("square hold:100", k, 16, &pos) == 2 && UI_KEY_IS_SHAPE(k[0]) && UI_KEY_IS_HOLD(k[1]));
    CHECK(ui_keys_parse("hold:100", k, 16, &pos) == UI_KEYS_ERR && pos == 0);     // no key to hold
    CHECK(ui_keys_parse("phys hold:100", k, 16, &pos) == UI_KEYS_ERR && pos == 5);
    CHECK(ui_keys_parse("wait hold:100", k, 16, &pos) == UI_KEYS_ERR && pos == 5);
    CHECK(ui_keys_parse("needlock hold:100", k, 16, &pos) == UI_KEYS_ERR);
    CHECK(ui_keys_parse("row:A|hold:100", k, 16, &pos) == UI_KEYS_ERR && pos == 6); // a guard is no key
    CHECK(ui_keys_parse("esc hold:1 hold:2", k, 16, &pos) == UI_KEYS_ERR && pos == 11);
    CHECK(ui_keys_parse("esc hold:", k, 16, &pos) == UI_KEYS_ERR && pos == 4);
    CHECK(ui_keys_parse("esc hold:12x", k, 16, &pos) == UI_KEYS_ERR && pos == 4);
    CHECK(ui_keys_parse("esc hold:-5", k, 16, &pos) == UI_KEYS_ERR && pos == 4);
    CHECK(ui_keys_parse("esc phys hold:5", k, 2, &pos) == UI_KEYS_ERR && pos == 9); // does not fit
    CHECK(ui_keys_parse("phys", k, 16, &pos) == 1 && k[0] == UI_KEY_PHYS);
    // None of the new values passes for another kind
    uint32_t const odd[] = {UI_KEY_HOLD_BIT | 3000, UI_KEY_PHYS};
    for (size_t i = 0; i < 2; i++) {
        CHECK(!UI_KEY_IS_GUARD(odd[i]) && !UI_KEY_IS_SHAPE(odd[i]) && odd[i] != UI_KEY_WAIT &&
              odd[i] != UI_KEY_NEEDLOCK && !ui_keys_is_key(odd[i]));
    }
    CHECK(!UI_KEY_IS_HOLD(UI_KEY_WAIT) && !UI_KEY_IS_HOLD(UI_KEY_NEEDLOCK) && !UI_KEY_IS_HOLD(UI_KEY_SHAPE(2)));
    CHECK(!UI_KEY_IS_HOLD(k[0]) && !UI_KEY_IS_HOLD('z') && !UI_KEY_IS_HOLD(27));
    CHECK(ui_keys_is_key(27) && ui_keys_is_key('z') && ui_keys_is_key(UI_KEY_SHAPE(7)));
    CHECK(!ui_keys_is_key(UI_KEY_WAIT) && !ui_keys_is_key(UI_KEY_NEEDLOCK));
    // The stepper's wait (#375 review): a hold after the last key waits for its end, not for a gap long over
    uint32_t until[2] = {5000, 3000};
    CHECK(ui_keys_wait_ms(1000, 1250, true, NULL, 0) == 250);          // the next key
    CHECK(ui_keys_wait_ms(1000, 1250, true, until, 2) == 250);         // ... before either hold ends
    CHECK(ui_keys_wait_ms(2900, 1250, true, until, 2) == 1);           // a key overdue: at once
    CHECK(ui_keys_wait_ms(2000, 1250, false, until, 2) == 1000);       // no key left: the first hold to end
    CHECK(ui_keys_wait_ms(2000, 1250, false, until, 1) == 3000);
    CHECK(ui_keys_wait_ms(1100, 1250, false, until, 2) == 150);        // the last gap still counts while it runs
    CHECK(ui_keys_wait_ms(1300, 1250, false, NULL, 0) == 1);           // nothing held, gap over: the end
    CHECK(ui_keys_wait_ms(4000, 3500, false, until + 1, 1) == 1);      // a hold overdue: at once
    uint32_t wrap[1] = {100};
    CHECK(ui_keys_wait_ms(UINT32_MAX - 99, UINT32_MAX - 200, false, wrap, 1) == 200); // wrap-safe
    // Old "keys send" strings parse as they did (#375 keeps the grammar)
    char const *old = "needlock square wait row:Messages|enter text:Hi there|enter down notfocus:Erase|esc cloud";
    CHECK(ui_keys_parse(old, k, 32, &pos) == 18);
    CHECK(k[0] == UI_KEY_NEEDLOCK && k[1] == UI_KEY_SHAPE(2) && k[2] == UI_KEY_WAIT && UI_KEY_IS_GUARD(k[3]));
    CHECK(k[4] == 10 && k[5] == 'H' && k[7] == ' ' && k[12] == 'e');
    CHECK(k[13] == 10 && k[14] == 18 && UI_KEY_IS_GUARD(k[15]) && k[16] == 27 && k[17] == UI_KEY_SHAPE(6));
    CHECK(ui_keys_parse("text:hold:5|phys", k, 16, &pos) == 7 && k[0] == 'h' && k[6] == UI_KEY_PHYS);
    if (failures) {
        return 1;
    }
    printf("ui_keys_test: ok\n");
    return 0;
}
