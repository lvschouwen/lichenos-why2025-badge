// Host test: status bar link looks (#66, Task 9d)
#include "link_state.h"

#include <stdio.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#define TEAL  0x3FC1B0
#define AMBER 0xF5B700
#define RED   0xE5484D

int main(void) {
    link_look_t l = wifi_look(true, false, true, true, -50);
    CHECK(l.state == LINK_UP && l.bars == 4 && l.color == TEAL);
    l = wifi_look(true, false, true, true, -70);
    CHECK(l.state == LINK_UP && l.bars == 2 && l.color == TEAL);
    l = wifi_look(true, false, true, false, 0);
    CHECK(l.state == LINK_UP && l.bars == 1 && l.color == TEAL);
    l = wifi_look(true, false, true, true, -95);
    CHECK(l.state == LINK_UP && l.bars == 0 && l.color == TEAL);
    l = wifi_look(true, true, false, false, 0);
    CHECK(l.state == LINK_CONNECTING && l.bars == 0 && l.color == AMBER);
    l = wifi_look(true, false, false, false, 0);
    CHECK(l.state == LINK_DOWN && l.bars == 0 && l.color == RED);
    l = wifi_look(false, false, false, false, 0);
    CHECK(l.state == LINK_OFF && l.bars == 0 && l.color == RED);
    l = lora_look(true);
    CHECK(l.state == LINK_UP && l.color == TEAL);
    l = lora_look(false);
    CHECK(l.state == LINK_DOWN && l.color == RED);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("link_state_test ok\n");
    return 0;
}
