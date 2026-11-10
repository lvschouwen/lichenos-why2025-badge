// Host tests for the C6's settings registry (#309): the table against reg_ids.h and the registry rules, the backlight
// duty <-> percent mapping the read-back relies on, and the SX1262 read-back decoders. The drivers are fakes here.
#include "backlight.h"
#include "backlight_cmd.h"
#include "lora_readback.h"
#include "lora_service.h"
#include "reg_c6.h"

#include <stdio.h>
#include <stdlib.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

// Fake LEDC: an 8-bit duty per channel, set through the real backlight_duty and read back through backlight_percent
static uint32_t fake_duty[BL_COUNT];
static bool     fake_at_boot = true;

void backlight_start(void) {
    fake_duty[BL_DISPLAY]  = backlight_duty(30, 8);
    fake_duty[BL_KEYBOARD] = backlight_duty(10, 8);
}

bool backlight_apply(int which, int percent) {
    fake_duty[which] = backlight_duty(percent, 8);
    fake_at_boot     = false;
    return true;
}

bool backlight_applied(int which, uint8_t *percent) {
    *percent = (uint8_t)backlight_percent(fake_duty[which], 8);
    return true;
}

uint32_t backlight_freq_hz(void) {
    return 25000;
}

bool backlight_at_boot(void) {
    return fake_at_boot;
}

// Fake radio
static bool          fake_started;
static lora_config_t fake_radio = {869618000, 62500, 8, 8, 0x12, 16, 22};
static uint8_t       fake_state = LORA_STATE_UNCONFIGURED;
static int           readbacks;

bool lora_service_readback(lora_config_t *out) {
    readbacks++;
    if (!fake_started) {
        return false;
    }
    *out = fake_radio;
    return true;
}

uint8_t lora_service_state(void) {
    return fake_state;
}

uint8_t c6_bt_status(void) {
    return 0; // ESP_BT_CONTROLLER_STATUS_IDLE
}

static reg_status_t serve(uint16_t id, uint32_t const *set, reg_reply_t *r) {
    return reg_serve(reg_c6_table, reg_c6_count, id, set, r);
}

static void test_table(void) {
    CHECK(reg_table_check(reg_c6_table, reg_c6_count) == reg_c6_count);

    // Every key reg_ids.h gives the C6 is served, and nothing else
    static uint16_t const c6_ids[] = {
        REG_ID_BL_DISPLAY,  REG_ID_BL_KEYBOARD, REG_ID_BL_FREQ,      REG_ID_BL_BOOT,       REG_ID_LORA_FREQ,
        REG_ID_LORA_BW,     REG_ID_LORA_SF,     REG_ID_LORA_CR,      REG_ID_LORA_SYNC,     REG_ID_LORA_PREAMBLE,
        REG_ID_LORA_POWER,  REG_ID_LORA_STATE,  REG_ID_BT_STATE,     REG_ID_C6_REG_CAPS,
    };
    CHECK(reg_c6_count == sizeof(c6_ids) / sizeof(c6_ids[0]));
    for (size_t i = 0; i < sizeof(c6_ids) / sizeof(c6_ids[0]); i++) {
        CHECK(reg_find_id(reg_c6_table, reg_c6_count, c6_ids[i]) != NULL);
    }

    // c6.reg_caps names every domain the table serves, so the P4 asks for all of them
    reg_reply_t r;
    CHECK(serve(REG_ID_C6_REG_CAPS, NULL, &r) == REG_OK && r.type == REG_T_U32);
    for (size_t i = 0; i < reg_c6_count; i++) {
        CHECK(r.value & REG_CAP(REG_ID_DOMAIN(reg_c6_table[i].id)));
        // Radio keys are read-only TX keys without a setter (#307); only the two backlights take a set
        bool const lora     = REG_ID_DOMAIN(reg_c6_table[i].id) == 0x02 && reg_c6_table[i].id != REG_ID_LORA_STATE;
        bool const settable = reg_c6_table[i].id == REG_ID_BL_DISPLAY || reg_c6_table[i].id == REG_ID_BL_KEYBOARD;
        CHECK(!lora || ((reg_c6_table[i].flags & (REG_F_RO | REG_F_TX)) == (REG_F_RO | REG_F_TX)));
        CHECK((reg_c6_table[i].set != NULL) == settable);
    }
    CHECK(r.value == (REG_CAP_BL | REG_CAP_LORA | REG_CAP_BT | REG_CAP_C6));

    // lora.state names lora_state_t in its order
    reg_entry_t const *st = reg_find_id(reg_c6_table, reg_c6_count, REG_ID_LORA_STATE);
    CHECK(st && st->max == LORA_STATE_TX);
}

static void test_duty_percent(void) {
    // Every percentage comes back as what backlight_duty drives: 0 off, 1-9 held at 10, 81-100 held at 80
    for (unsigned bits = 8; bits <= 13; bits++) {
        for (int p = 0; p <= 100; p++) {
            int const held = p == 0 ? 0 : p < 10 ? 10 : p > 80 ? 80 : p;
            CHECK(backlight_percent(backlight_duty(p, bits), bits) == held);
        }
    }
    CHECK(backlight_percent(0, 8) == 0);
    CHECK(backlight_percent(256, 8) == 100);
    CHECK(backlight_percent(0xFFFFFFFFu, 8) == 100);
    CHECK(backlight_percent(128, 8) == 50);
}

static void test_backlight_keys(void) {
    backlight_start();
    reg_reply_t r;
    CHECK(serve(REG_ID_BL_BOOT, NULL, &r) == REG_OK && r.value == 1);
    CHECK(serve(REG_ID_BL_DISPLAY, NULL, &r) == REG_OK && r.type == REG_T_U8 && r.value == 30);
    CHECK(serve(REG_ID_BL_KEYBOARD, NULL, &r) == REG_OK && r.value == 10);
    CHECK(serve(REG_ID_BL_FREQ, NULL, &r) == REG_OK && r.value == 25000);

    uint32_t v = 50;
    CHECK(serve(REG_ID_BL_DISPLAY, &v, &r) == REG_OK && r.value == 50);
    CHECK(serve(REG_ID_BL_BOOT, NULL, &r) == REG_OK && r.value == 0);
    CHECK(serve(REG_ID_BL_KEYBOARD, NULL, &r) == REG_OK && r.value == 10); // a set touches its own channel only
    v = 5; // the answer is the applied value, not the request
    CHECK(serve(REG_ID_BL_DISPLAY, &v, &r) == REG_OK && r.value == 10);
    v = 95;
    CHECK(serve(REG_ID_BL_KEYBOARD, &v, &r) == REG_OK && r.value == 80);
    v = 0;
    CHECK(serve(REG_ID_BL_DISPLAY, &v, &r) == REG_OK && r.value == 0);
    v = 101; // out of range: refused, still answered with what runs now
    CHECK(serve(REG_ID_BL_DISPLAY, &v, &r) == REG_RANGE && r.value == 0);
    v = 25000;
    CHECK(serve(REG_ID_BL_FREQ, &v, &r) == REG_RO && r.value == 25000);
}

static void test_radio_keys(void) {
    reg_reply_t r;
    fake_started = false;
    CHECK(serve(REG_ID_LORA_FREQ, NULL, &r) == REG_FAILED && r.type == REG_T_U32 && r.value == 0);
    CHECK(serve(REG_ID_LORA_STATE, NULL, &r) == REG_OK && r.type == REG_T_ENUM && r.value == LORA_STATE_UNCONFIGURED);

    fake_started = true;
    fake_state   = LORA_STATE_RX;
    CHECK(serve(REG_ID_LORA_FREQ, NULL, &r) == REG_OK && r.value == 869618000);
    CHECK(serve(REG_ID_LORA_BW, NULL, &r) == REG_OK && r.value == 62500);
    CHECK(serve(REG_ID_LORA_SF, NULL, &r) == REG_OK && r.value == 8);
    CHECK(serve(REG_ID_LORA_CR, NULL, &r) == REG_OK && r.value == 8);
    CHECK(serve(REG_ID_LORA_SYNC, NULL, &r) == REG_OK && r.value == 0x12);
    CHECK(serve(REG_ID_LORA_PREAMBLE, NULL, &r) == REG_OK && r.value == 16);
    CHECK(serve(REG_ID_LORA_STATE, NULL, &r) == REG_OK && r.value == LORA_STATE_RX);
    fake_radio.tx_power_dbm = -9; // I16 travels sign-extended
    CHECK(serve(REG_ID_LORA_POWER, NULL, &r) == REG_OK && r.type == REG_T_I16 && r.value == (uint32_t)-9);

    // A radio key is guarded: no set, the answer is what the SX1262 runs
    uint32_t v = 868000000;
    CHECK(serve(REG_ID_LORA_FREQ, &v, &r) == REG_GUARDED && r.value == 869618000);
    v = 7;
    CHECK(serve(REG_ID_LORA_SF, &v, &r) == REG_GUARDED && r.value == 8);
    CHECK(fake_radio.sf == 8 && fake_radio.freq_hz == 869618000);

    CHECK(serve(REG_ID_BT_STATE, NULL, &r) == REG_OK && r.type == REG_T_ENUM && r.value == 0);
    v = 2;
    CHECK(serve(REG_ID_BT_STATE, &v, &r) == REG_RO && r.value == 0);

    // An unknown id, also one in a C6 domain, is answered UNKNOWN with nothing else
    CHECK(serve(0x01FF, NULL, &r) == REG_UNKNOWN && r.type == 0 && r.value == 0);
    CHECK(serve(REG_ID_WIFI_COUNTRY, NULL, &r) == REG_UNKNOWN);
}

static void test_readback_decoders(void) {
    CHECK(lora_rb_bw_hz(0x00) == 7812);
    CHECK(lora_rb_bw_hz(0x08) == 10417);
    CHECK(lora_rb_bw_hz(0x03) == 62500);
    CHECK(lora_rb_bw_hz(0x04) == 125000);
    CHECK(lora_rb_bw_hz(0x06) == 500000);
    CHECK(lora_rb_bw_hz(0x07) == 0);
    CHECK(lora_rb_bw_hz(0xFF) == 0);

    // RadioLib's setSyncWord(0x12) with its default control bits 0x44 writes 0x14 0x24
    CHECK(lora_rb_sync(0x14, 0x24) == 0x12);
    CHECK(lora_rb_sync(0x34, 0x44) == 0x34);
    CHECK(lora_rb_sync(0xF4, 0xF4) == 0xFF);

    CHECK(lora_rb_cr(1) == 5 && lora_rb_cr(4) == 8);
    CHECK(lora_rb_cr(0) == 0 && lora_rb_cr(5) == 0);

    // The synthesizer frequency is within the float steps of the request, never exact by luck alone
    static float const mhz[]  = {869.618f, 869.525f, 868.1f, 863.0f, 927.9f};
    static long const  want[] = {869618000, 869525000, 868100000, 863000000, 927900000};
    for (size_t i = 0; i < sizeof(mhz) / sizeof(mhz[0]); i++) {
        long const got = (long)lora_rb_freq_hz(mhz[i]);
        CHECK(labs(got - want[i]) <= 64);
    }
    CHECK(lora_rb_freq_hz(0.0f) == 0);
    CHECK(lora_rb_freq_hz(-1.0f) == 0);
}

int main(void) {
    test_table();
    test_duty_percent();
    test_backlight_keys();
    test_radio_keys();
    test_readback_decoders();
    CHECK(readbacks > 0);
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("c6_reg_test: all passed\n");
    return 0;
}
