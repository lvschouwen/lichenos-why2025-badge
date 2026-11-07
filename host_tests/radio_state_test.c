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
    CHECK(radio_state_get(&s, RADIO_WIFI) && radio_state_get(&s, RADIO_LORA) && !radio_state_get(&s, RADIO_BT));
    radio_state_set(&s, RADIO_BT, true); // the cases below use all three radios on
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
    CHECK(radio_state_set_airplane(&s, false) == RADIO_MASK_DEFAULT);
    CHECK(s.on == RADIO_MASK_DEFAULT);

    // off before airplane stays off after it
    radio_state_init(&s);
    radio_state_set(&s, RADIO_BT, true);
    radio_state_set(&s, RADIO_LORA, false);
    radio_state_set_airplane(&s, true);
    radio_state_set_airplane(&s, false);
    CHECK(s.on == (W | B));

    // persistence round trip: every field, mid-airplane included
    radio_state_init(&s);
    radio_state_set(&s, RADIO_BT, true);
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
    CHECK(r.on == RADIO_MASK_DEFAULT && !r.airplane);
    radio_state_load(&r, 1, 1, 7);
    CHECK(r.on == RADIO_MASK_DEFAULT && !r.airplane);

    // #197: Bluetooth is off on a fresh badge, in the saved mask too
    radio_state_init(&s);
    CHECK(!radio_state_get(&s, RADIO_BT) && s.on == (W | L) && s.saved == (W | L));
    radio_state_set_airplane(&s, true);
    radio_state_set_airplane(&s, false);
    CHECK(!radio_state_get(&s, RADIO_BT));

    // a pre-#197 mask (0x07, saved 0x07, mid-airplane) loses BT once; Wi-Fi and LoRa are untouched
    radio_state_load(&r, RADIO_MASK_ALL, RADIO_MASK_ALL, 1);
    CHECK(radio_state_migrate(&r, 0) == RADIO_STATE_VERSION);
    CHECK(r.on == (W | L) && r.saved == (W | L) && r.airplane);
    CHECK(!radio_state_get(&r, RADIO_BT));
    radio_state_load(&r, W, RADIO_MASK_ALL, 0); // user had BT off in a saved mask
    radio_state_migrate(&r, 0);
    CHECK(r.on == W && r.saved == (W | L));

    // after the migration the user turns BT on: a reload at the stored version keeps it
    radio_state_load(&r, RADIO_MASK_ALL, RADIO_MASK_ALL, 0);
    radio_state_migrate(&r, 0);
    radio_state_set(&r, RADIO_BT, true);
    radio_state_t again;
    radio_state_load(&again, r.on, r.saved, 0);
    CHECK(radio_state_migrate(&again, RADIO_STATE_VERSION) == RADIO_STATE_VERSION);
    CHECK(radio_state_get(&again, RADIO_BT) && again.on == RADIO_MASK_ALL);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("radio_state_test: ok\n");
    return 0;
}
