// Host tests for the OS radio switches and airplane mode (#108)
#include "radio_state.h"

#include <stdio.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#define W (1u << RADIO_WIFI)
#define L (1u << RADIO_LORA)
#define B (1u << RADIO_BT)

int main(void) {
    radio_state_t s;
    radio_state_init(&s);
    CHECK(radio_state_get(&s, RADIO_WIFI) && radio_state_get(&s, RADIO_LORA) && radio_state_get(&s, RADIO_BT));
    CHECK(!s.airplane);

    // single switches report what changed, and nothing when already there
    CHECK(radio_state_set(&s, RADIO_WIFI, false) == W);
    CHECK(radio_state_set(&s, RADIO_WIFI, false) == 0);
    CHECK(!radio_state_get(&s, RADIO_WIFI) && radio_state_get(&s, RADIO_LORA));
    CHECK(radio_state_set(&s, RADIO_WIFI, true) == W);
    CHECK(radio_state_set(&s, (radio_id_t)RADIO_COUNT, true) == 0);
    CHECK(!radio_state_get(&s, (radio_id_t)RADIO_COUNT));

    // airplane on: everything off, the previous states kept
    radio_state_set(&s, RADIO_LORA, false); // wifi + bt on, lora off
    CHECK(radio_state_set_airplane(&s, true) == (W | B));
    CHECK(s.airplane && s.on == 0 && s.saved == (W | B));
    CHECK(radio_state_set_airplane(&s, true) == 0);

    // phone style: one radio back on while airplane is on
    CHECK(radio_state_set(&s, RADIO_LORA, true) == L);
    CHECK(s.airplane && s.on == L);

    // airplane off restores wifi + bt; lora, switched on meanwhile, stays on
    CHECK(radio_state_set_airplane(&s, false) == (W | B));
    CHECK(!s.airplane && s.on == (W | L | B));

    // a radio switched off during airplane mode that was on before comes back too
    radio_state_init(&s);
    radio_state_set_airplane(&s, true);
    radio_state_set(&s, RADIO_WIFI, true);
    radio_state_set(&s, RADIO_WIFI, false);
    CHECK(radio_state_set_airplane(&s, false) == RADIO_MASK_ALL);
    CHECK(s.on == RADIO_MASK_ALL);

    // off before airplane stays off after it
    radio_state_init(&s);
    radio_state_set(&s, RADIO_LORA, false);
    radio_state_set_airplane(&s, true);
    radio_state_set_airplane(&s, false);
    CHECK(s.on == (W | B));

    // persistence round trip: every field, mid-airplane included
    radio_state_init(&s);
    radio_state_set(&s, RADIO_BT, false);
    radio_state_set_airplane(&s, true);
    radio_state_set(&s, RADIO_WIFI, true);
    radio_state_t r;
    radio_state_load(&r, s.on, s.saved, s.airplane ? 1 : 0);
    CHECK(r.on == s.on && r.saved == s.saved && r.airplane == s.airplane);
    CHECK(radio_state_set_airplane(&r, false) == (L));
    CHECK(r.on == (W | L));

    // garbage in NVS gives the defaults
    radio_state_load(&r, 0xFF, 1, 0);
    CHECK(r.on == RADIO_MASK_ALL && !r.airplane);
    radio_state_load(&r, 1, 1, 7);
    CHECK(r.on == RADIO_MASK_ALL && !r.airplane);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("radio_state_test: ok\n");
    return 0;
}
