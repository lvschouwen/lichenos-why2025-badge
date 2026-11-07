// Host tests for the LoRa radio switch (#222): lora reaches the OS switch only through the owner callback main.c
// injects, and without one "fw lora on|off" still switches LoRa (alone, not saved) instead of calling a NULL pointer.
// lora_send refuses while lora_enabled() is false, so every "off" below is a refused TX and every "on" its recovery.
#include "lora_os.h"
#include "lora_switch.h"

#include <stdio.h>

static int failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                    \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static int  requests;
static bool last_request;
static bool saved = true;

// What main.c wires: the OS switch saves the state and its listener (radio_changed) calls lora_set_enabled
static void os_switch(bool on) {
    requests++;
    last_request = on;
    saved        = on;
    lora_set_enabled(on);
}

// An owner that keeps the switch where it is (a save refused, airplane mode deciding otherwise)
static void stubborn_switch(bool on) {
    requests++;
    last_request = on;
}

// Every case starts from the boot state, so the order of the cases does not matter
static void reset(void) {
    lora_set_switch_owner(NULL);
    lora_set_enabled(true);
    requests     = 0;
    last_request = false;
    saved        = true;
}

static void test_default_on(void) {
    CHECK(lora_enabled()); // a boot without saved switches sends; main runs this first, before any reset
}

static void test_no_owner_switches_lora_alone(void) {
    reset();
    CHECK(!lora_switch_request(false));
    CHECK(!lora_enabled()); // TX refused
    CHECK(lora_switch_request(true));
    CHECK(lora_enabled()); // and back
}

static void test_owner_gets_the_request(void) {
    reset();
    lora_set_switch_owner(os_switch);
    CHECK(!lora_switch_request(false));
    CHECK(requests == 1 && !last_request);
    CHECK(!saved);
    CHECK(!lora_enabled()); // the switch-off callback refuses TX
    CHECK(lora_switch_request(true));
    CHECK(requests == 2 && last_request);
    CHECK(saved && lora_enabled()); // recovered
    lora_set_switch_owner(NULL);
}

static void test_owner_decides(void) {
    reset();
    lora_set_enabled(false);
    lora_set_switch_owner(stubborn_switch);
    CHECK(!lora_switch_request(true)); // the reply is what lora_send goes by, not what was asked
    CHECK(requests == 1 && last_request);
    CHECK(!lora_enabled());
    lora_set_switch_owner(NULL); // NULL puts the default back
    CHECK(lora_switch_request(true));
    CHECK(requests == 1);
}

int main(void) {
    test_default_on();
    test_no_owner_switches_lora_alone();
    test_owner_gets_the_request();
    test_owner_decides();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("lora_switch_test: all passed\n");
    return 0;
}
