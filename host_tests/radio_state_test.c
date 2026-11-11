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

// #282: the thermal shed holds Wi-Fi and LoRa off without writing the person's switches (`on`, what NVS keeps)
static void holds(void) {
    radio_state_t s;
    radio_state_init(&s); // Wi-Fi + LoRa on
    CHECK(radio_state_hold(&s, RADIO_HOLD_THERMAL, W | L) == (W | L));
    CHECK(!radio_state_get(&s, RADIO_WIFI) && !radio_state_get(&s, RADIO_LORA));
    CHECK(s.on == (W | L)); // what is saved stays the person's choice
    CHECK(radio_state_held_by(&s, RADIO_WIFI) == RADIO_HOLD_THERMAL);
    CHECK(radio_state_held_by(&s, RADIO_BT) == RADIO_HOLD_NONE);
    CHECK(radio_state_hold(&s, RADIO_HOLD_THERMAL, W | L) == 0); // again: nothing new to tell
    // a reboot while held: what was saved loads without the hold, so the radios come back as the person set them
    radio_state_t boot;
    radio_state_load(&boot, s.on, s.saved, s.airplane ? 1 : 0);
    CHECK(radio_state_get(&boot, RADIO_WIFI) && radio_state_get(&boot, RADIO_LORA));
    CHECK(radio_state_held_by(&boot, RADIO_WIFI) == RADIO_HOLD_NONE);
    // the reason ends: back as before
    CHECK(radio_state_release(&s, RADIO_HOLD_THERMAL) == (W | L));
    CHECK(radio_state_effective(&s) == (W | L));
    CHECK(radio_state_release(&s, RADIO_HOLD_THERMAL) == 0);

    // a radio that was off is held too, but nothing changes for it, and it stays off after the release
    radio_state_set(&s, RADIO_LORA, false);
    CHECK(radio_state_hold(&s, RADIO_HOLD_THERMAL, W | L) == W);
    CHECK(radio_state_release(&s, RADIO_HOLD_THERMAL) == W);
    CHECK(radio_state_effective(&s) == W);

    // the person switches a held radio off: saved off, and it stays off after the release
    radio_state_init(&s);
    radio_state_hold(&s, RADIO_HOLD_THERMAL, W | L);
    CHECK(radio_state_set(&s, RADIO_WIFI, false) == 0); // already off for the radio
    CHECK(s.on == L);
    CHECK(radio_state_release(&s, RADIO_HOLD_THERMAL) == L);
    CHECK(radio_state_effective(&s) == L);

    // the person switches a held radio on: that overrides the hold for that radio only
    radio_state_init(&s);
    radio_state_hold(&s, RADIO_HOLD_THERMAL, W | L);
    CHECK(radio_state_set(&s, RADIO_WIFI, true) == W);
    CHECK(radio_state_get(&s, RADIO_WIFI) && !radio_state_get(&s, RADIO_LORA));
    CHECK(radio_state_held_by(&s, RADIO_WIFI) == RADIO_HOLD_NONE);
    CHECK(radio_state_release(&s, RADIO_HOLD_THERMAL) == L);

    // airplane while held: airplane keeps the person's switches, not the held view; off again, the hold still rules
    radio_state_init(&s);
    radio_state_hold(&s, RADIO_HOLD_THERMAL, W | L);
    CHECK(radio_state_set_airplane(&s, true) == 0);
    CHECK(s.saved == (W | L));
    CHECK(radio_state_set_airplane(&s, false) == 0);
    CHECK(radio_state_effective(&s) == 0);
    CHECK(radio_state_release(&s, RADIO_HOLD_THERMAL) == (W | L));
    // ... and a release during airplane brings nothing on
    radio_state_hold(&s, RADIO_HOLD_THERMAL, W | L);
    radio_state_set_airplane(&s, true);
    CHECK(radio_state_release(&s, RADIO_HOLD_THERMAL) == 0);
    CHECK(radio_state_effective(&s) == 0);

    // no reason, an unknown reason
    radio_state_init(&s);
    CHECK(radio_state_hold(&s, RADIO_HOLD_NONE, W) == 0);
    CHECK(radio_state_hold(&s, RADIO_HOLD_COUNT, W) == 0);
    CHECK(radio_state_release(&s, RADIO_HOLD_COUNT) == 0);
    CHECK(radio_state_effective(&s) == (W | L));
    CHECK(radio_state_held_by(&s, (radio_id_t)RADIO_COUNT) == RADIO_HOLD_NONE);
}

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

    holds();

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("radio_state_test: ok\n");
    return 0;
}
