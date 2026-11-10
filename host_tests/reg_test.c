// Host tests for the settings registry core (#308): parse, range check, format, find, and the set/serve rules the
// peer protocol relies on (an unknown id is UNKNOWN, a TX key is GUARDED, every answer carries the applied value)
#include "reg.h"

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

// A fake driver: what the "hardware" applies, and how often each hook ran
static uint32_t bl_applied   = 30;
static uint32_t bl_clamp_low = 10; // the C6 holds 1..9 % at 10 %, so applied differs from the request
static int      set_calls;
static bool     get_fails;
static bool     set_fails;
static uint32_t freq_applied = 869618000;

static reg_status_t get_bl(reg_entry_t const *e, reg_value_t *out) {
    (void)e;
    if (get_fails) {
        return REG_FAILED;
    }
    out->u = bl_applied;
    return REG_OK;
}

static reg_status_t set_bl(reg_entry_t const *e, reg_value_t const *v) {
    (void)e;
    set_calls++;
    if (set_fails) {
        return REG_FAILED;
    }
    bl_applied = v->u && v->u < bl_clamp_low ? bl_clamp_low : v->u;
    return REG_OK;
}

static reg_status_t get_freq(reg_entry_t const *e, reg_value_t *out) {
    (void)e;
    out->u = freq_applied;
    return REG_OK;
}

static reg_status_t set_freq(reg_entry_t const *e, reg_value_t const *v) {
    (void)e;
    set_calls++;
    freq_applied = v->u;
    return REG_OK;
}

static reg_status_t get_power(reg_entry_t const *e, reg_value_t *out) {
    (void)e;
    out->i = -9;
    return REG_OK;
}

static reg_status_t get_caps(reg_entry_t const *e, reg_value_t *out) {
    (void)e;
    out->u = REG_CAP_BL | REG_CAP_LORA | REG_CAP_C6;
    return REG_OK;
}

static reg_status_t get_country(reg_entry_t const *e, reg_value_t *out) {
    (void)e;
    strcpy(out->s, "NL");
    return REG_OK;
}

static char const *const state_names[] = {"unconfigured", "idle", "rx", "error", "tx"};

static reg_status_t get_state(reg_entry_t const *e, reg_value_t *out) {
    (void)e;
    out->u = 2;
    return REG_OK;
}

static reg_status_t get_flag(reg_entry_t const *e, reg_value_t *out) {
    (void)e;
    out->u = 1;
    return REG_OK;
}

// A setter on a TX key on purpose: the TX flag must refuse before any setter runs
static reg_entry_t const table[] = {
    {REG_ID_BL_DISPLAY, REG_T_U8, 0, "bl.display", "%", 0, 100, NULL, get_bl, set_bl, "bl_disp"},
    {REG_ID_LORA_FREQ, REG_T_U32, REG_F_TX, "lora.freq", "Hz", 862000000, 928000000, NULL, get_freq, set_freq, NULL},
    {REG_ID_LORA_POWER, REG_T_I16, REG_F_RO | REG_F_TX, "lora.power", "dBm", -9, 22, NULL, get_power, NULL, NULL},
    {REG_ID_LORA_STATE, REG_T_ENUM, REG_F_RO, "lora.state", NULL, 0, 4, state_names, get_state, NULL, NULL},
    {REG_ID_WIFI_COUNTRY, REG_T_STR, 0, "wifi.country", NULL, 2, 2, NULL, get_country, NULL, NULL},
    {REG_ID_C6_REG_CAPS, REG_T_U32, REG_F_RO, "c6.reg_caps", NULL, 0, 0, NULL, get_caps, NULL, NULL},
    {0x0FFE, REG_T_BOOL, 0, "test.flag", NULL, 0, 0, NULL, get_flag, NULL, NULL},
};
#define TABLE_N (sizeof(table) / sizeof(table[0]))

static reg_entry_t const *by_id(uint16_t id) {
    reg_entry_t const *e = reg_find_id(table, TABLE_N, id);
    CHECK(e != NULL);
    return e;
}

static void find(void) {
    CHECK(reg_find_id(table, TABLE_N, REG_ID_LORA_FREQ) == &table[1]);
    CHECK(reg_find_id(table, TABLE_N, 0x7777) == NULL);
    CHECK(reg_find_name(table, TABLE_N, "lora.state") == &table[3]);
    CHECK(reg_find_name(table, TABLE_N, "lora") == NULL);
    CHECK(reg_find_name(table, TABLE_N, NULL) == NULL);

    CHECK(reg_name_matches("lora.freq", NULL));
    CHECK(reg_name_matches("lora.freq", ""));
    CHECK(reg_name_matches("lora.freq", "lora"));
    CHECK(reg_name_matches("lora.freq", "lora."));
    CHECK(reg_name_matches("lora.freq", "lora.freq"));
    CHECK(!reg_name_matches("lora.freq", "lora.f"));
    CHECK(!reg_name_matches("lora.freq", "lo"));
    CHECK(!reg_name_matches("lora.freq", "lora.freq.x"));
    CHECK(!reg_name_matches("bl.display", "lora"));
}

static void parse(void) {
    reg_value_t        v;
    reg_entry_t const *bl = by_id(REG_ID_BL_DISPLAY), *freq = by_id(REG_ID_LORA_FREQ);
    reg_entry_t const *pwr = by_id(REG_ID_LORA_POWER), *st = by_id(REG_ID_LORA_STATE);
    reg_entry_t const *cc = by_id(REG_ID_WIFI_COUNTRY), *flag = by_id(0x0FFE);

    CHECK(reg_parse(bl, "42", &v) && v.u == 42);
    CHECK(reg_parse(bl, "0x2a", &v) && v.u == 42);
    CHECK(reg_parse(bl, "300", &v) && v.u == 300); // syntax only: the range check says RANGE
    CHECK(!reg_parse(bl, "-1", &v));
    CHECK(!reg_parse(bl, "", &v));
    CHECK(!reg_parse(bl, "4x", &v));
    CHECK(!reg_parse(bl, NULL, &v));
    CHECK(reg_parse(freq, "4294967295", &v) && v.u == 4294967295u);
    CHECK(!reg_parse(freq, "4294967296", &v));
    CHECK(reg_parse(pwr, "-9", &v) && v.i == -9);
    CHECK(reg_parse(pwr, "-2147483648", &v) && v.i == INT32_MIN);
    CHECK(!reg_parse(pwr, "2147483648", &v));
    CHECK(reg_parse(st, "rx", &v) && v.u == 2);
    CHECK(reg_parse(st, "4", &v) && v.u == 4);
    CHECK(!reg_parse(st, "sleep", &v));
    CHECK(reg_parse(flag, "on", &v) && v.u == 1);
    CHECK(reg_parse(flag, "false", &v) && v.u == 0);
    CHECK(reg_parse(flag, "1", &v) && v.u == 1);
    CHECK(!reg_parse(flag, "maybe", &v));
    CHECK(reg_parse(cc, "NL", &v) && strcmp(v.s, "NL") == 0);
    CHECK(reg_parse(cc, "0123456789abcde", &v));   // 15
    CHECK(!reg_parse(cc, "0123456789abcdef", &v)); // 16
}

static void check_ranges(void) {
    reg_entry_t const *bl = by_id(REG_ID_BL_DISPLAY), *freq = by_id(REG_ID_LORA_FREQ);
    reg_entry_t const *pwr = by_id(REG_ID_LORA_POWER), *st = by_id(REG_ID_LORA_STATE);
    reg_entry_t const *cc = by_id(REG_ID_WIFI_COUNTRY), *flag = by_id(0x0FFE), *caps = by_id(REG_ID_C6_REG_CAPS);

    CHECK(reg_check(bl, &(reg_value_t){.u = 0}) == REG_OK);
    CHECK(reg_check(bl, &(reg_value_t){.u = 100}) == REG_OK);
    CHECK(reg_check(bl, &(reg_value_t){.u = 101}) == REG_RANGE);
    CHECK(reg_check(bl, &(reg_value_t){.u = 256}) == REG_RANGE);
    CHECK(reg_check(freq, &(reg_value_t){.u = 862000000}) == REG_OK);
    CHECK(reg_check(freq, &(reg_value_t){.u = 861999999}) == REG_RANGE);
    CHECK(reg_check(freq, &(reg_value_t){.u = 928000001}) == REG_RANGE);
    CHECK(reg_check(pwr, &(reg_value_t){.i = -9}) == REG_OK);
    CHECK(reg_check(pwr, &(reg_value_t){.i = -10}) == REG_RANGE);
    CHECK(reg_check(pwr, &(reg_value_t){.i = 22}) == REG_OK);
    CHECK(reg_check(pwr, &(reg_value_t){.i = 65531}) == REG_RANGE); // an I16 not sign-extended
    CHECK(reg_check(st, &(reg_value_t){.u = 4}) == REG_OK);
    CHECK(reg_check(st, &(reg_value_t){.u = 5}) == REG_RANGE);
    CHECK(reg_check(flag, &(reg_value_t){.u = 1}) == REG_OK); // BOOL with no min..max: the type's 0..1
    CHECK(reg_check(flag, &(reg_value_t){.u = 2}) == REG_RANGE);
    CHECK(reg_check(caps, &(reg_value_t){.u = UINT32_MAX}) == REG_OK);
    CHECK(reg_check(cc, &(reg_value_t){.s = "NL"}) == REG_OK);
    CHECK(reg_check(cc, &(reg_value_t){.s = "N"}) == REG_RANGE);
    reg_value_t unterminated;
    memset(unterminated.s, 'x', sizeof(unterminated.s));
    CHECK(reg_check(cc, &unterminated) == REG_RANGE);
}

static void format(void) {
    char               buf[24];
    reg_entry_t const *bl = by_id(REG_ID_BL_DISPLAY), *pwr = by_id(REG_ID_LORA_POWER);
    reg_entry_t const *st = by_id(REG_ID_LORA_STATE), *cc = by_id(REG_ID_WIFI_COUNTRY), *flag = by_id(0x0FFE);
    reg_entry_t const *freq = by_id(REG_ID_LORA_FREQ);

    CHECK(reg_format(bl, &(reg_value_t){.u = 42}, buf, sizeof(buf)) == 2 && strcmp(buf, "42") == 0);
    CHECK(reg_format(freq, &(reg_value_t){.u = 869618000}, buf, sizeof(buf)) == 9 && strcmp(buf, "869618000") == 0);
    CHECK(reg_format(pwr, &(reg_value_t){.i = -9}, buf, sizeof(buf)) == 2 && strcmp(buf, "-9") == 0);
    CHECK(reg_format(st, &(reg_value_t){.u = 2}, buf, sizeof(buf)) == 2 && strcmp(buf, "rx") == 0);
    CHECK(reg_format(st, &(reg_value_t){.u = 9}, buf, sizeof(buf)) == 1 && strcmp(buf, "9") == 0); // no name: number
    CHECK(reg_format(flag, &(reg_value_t){.u = 1}, buf, sizeof(buf)) == 2 && strcmp(buf, "on") == 0);
    CHECK(reg_format(flag, &(reg_value_t){.u = 0}, buf, sizeof(buf)) == 3 && strcmp(buf, "off") == 0);
    CHECK(reg_format(cc, &(reg_value_t){.s = "NL"}, buf, sizeof(buf)) == 2 && strcmp(buf, "NL") == 0);
    CHECK(reg_format(freq, &(reg_value_t){.u = 869618000}, buf, 9) == 0); // no room for the NUL
    CHECK(reg_format(freq, &(reg_value_t){.u = 869618000}, buf, 10) == 9);
}

// A set always reads the applied value back: the clamp shows, not the request
static void set_reads_back(void) {
    reg_entry_t const *bl = by_id(REG_ID_BL_DISPLAY);
    reg_value_t        got;
    set_calls = 0;

    CHECK(reg_set(bl, &(reg_value_t){.u = 5}, &got) == REG_OK && got.u == 10 && set_calls == 1);
    CHECK(reg_set(bl, &(reg_value_t){.u = 101}, &got) == REG_RANGE && got.u == 10 && set_calls == 1);

    set_fails = true;
    CHECK(reg_set(bl, &(reg_value_t){.u = 50}, &got) == REG_FAILED && got.u == 10);
    set_fails = false;

    get_fails = true; // applied but not readable: not OK
    CHECK(reg_set(bl, &(reg_value_t){.u = 60}, &got) == REG_FAILED && got.u == 0);
    CHECK(reg_get(bl, &got) == REG_FAILED && got.u == 0);
    get_fails = false;
    CHECK(reg_get(bl, &got) == REG_OK && got.u == 60);
}

// #307 rule: radio keys are read-only; a set on a TX key is GUARDED, its setter never runs, the answer is the radio's
static void tx_key_is_guarded(void) {
    reg_value_t got;
    set_calls = 0;

    CHECK(reg_set(by_id(REG_ID_LORA_FREQ), &(reg_value_t){.u = 868000000}, &got) == REG_GUARDED);
    CHECK(set_calls == 0 && freq_applied == 869618000 && got.u == 869618000);
    CHECK(reg_set(by_id(REG_ID_LORA_POWER), &(reg_value_t){.i = 10}, &got) == REG_GUARDED && got.i == -9);
    CHECK(reg_set(by_id(REG_ID_LORA_STATE), &(reg_value_t){.u = 1}, &got) == REG_RO && got.u == 2);
    CHECK(reg_set(by_id(0x0FFE), &(reg_value_t){.u = 0}, &got) == REG_RO && got.u == 1); // no setter
}

// What the C6 answers to REG_GET / REG_SET, in wire terms
static void serve(void) {
    reg_reply_t    r;
    uint32_t const freq = 868000000, bl80 = 80, too_big = 0x100, five = 5;

    CHECK(reg_serve(table, TABLE_N, 0x7777, NULL, &r) == REG_UNKNOWN && r.type == 0 && r.value == 0);
    CHECK(reg_serve(table, TABLE_N, 0x7777, &five, &r) == REG_UNKNOWN && r.type == 0 && r.value == 0);

    CHECK(reg_serve(table, TABLE_N, REG_ID_C6_REG_CAPS, NULL, &r) == REG_OK);
    CHECK(r.type == REG_T_U32 && r.value == (REG_CAP_BL | REG_CAP_LORA | REG_CAP_C6));

    CHECK(reg_serve(table, TABLE_N, REG_ID_LORA_POWER, NULL, &r) == REG_OK);
    CHECK(r.type == REG_T_I16 && r.value == 0xFFFFFFF7u); // -9 sign-extended

    set_calls = 0;
    CHECK(reg_serve(table, TABLE_N, REG_ID_LORA_FREQ, &freq, &r) == REG_GUARDED);
    CHECK(set_calls == 0 && r.type == REG_T_U32 && r.value == 869618000);

    CHECK(reg_serve(table, TABLE_N, REG_ID_BL_DISPLAY, &bl80, &r) == REG_OK && r.type == REG_T_U8 && r.value == 80);
    CHECK(reg_serve(table, TABLE_N, REG_ID_BL_DISPLAY, &too_big, &r) == REG_RANGE && r.value == 80);

    CHECK(reg_serve(table, TABLE_N, REG_ID_WIFI_COUNTRY, NULL, &r) == REG_FAILED);
    CHECK(r.type == REG_T_STR && r.value == 0); // STR never goes on the wire
}

// The numbers are wire format and named in docs/architecture.md: append-only
static void wire_values_pinned(void) {
    CHECK(
        REG_T_U8 == 1 && REG_T_I16 == 2 && REG_T_U32 == 3 && REG_T_I32 == 4 && REG_T_BOOL == 5 && REG_T_ENUM == 6 &&
        REG_T_STR == 7
    );
    CHECK(REG_OK == 0 && REG_UNKNOWN == 1 && REG_RANGE == 2 && REG_GUARDED == 3 && REG_RO == 4 && REG_FAILED == 5);
    CHECK(REG_ID_DOMAIN(REG_ID_LORA_STATE) == 0x02 && REG_CAP_LORA == 0x4 && REG_CAP_C6 == 0x8000);
}

int main(void) {
    find();
    parse();
    check_ranges();
    format();
    set_reads_back();
    tx_key_is_guarded();
    serve();
    wire_values_pinned();

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("reg_test: all passed\n");
    return 0;
}
