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
    if (failures) {
        return 1;
    }
    printf("ui_keys_test: ok\n");
    return 0;
}
