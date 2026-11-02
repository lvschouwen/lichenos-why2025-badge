// Host test: the advert interval, location and auto-add rules (#52)
#include "mesh_advert_cfg.h"

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

static uint32_t parse(char const *t) {
    uint32_t m = 12345;
    return advcfg_parse_interval(t, &m) ? 12345 : m;
}

static void test_interval(void) {
    CHECK(advcfg_interval_ok(0) && advcfg_interval_ok(180) && advcfg_interval_ok(10080));
    CHECK(!advcfg_interval_ok(1) && !advcfg_interval_ok(179) && !advcfg_interval_ok(10081));
    CHECK(parse("off") == 0 && parse("0") == 0);
    CHECK(parse("180") == 180 && parse("6h") == 360 && parse("2d") == 2880 && parse("200 min") == 200);
    CHECK(parse("3h") == 180 && parse("7d") == 10080);
    CHECK(parse("60") == 12345 && parse("2h") == 12345);   // under the flood limit
    CHECK(parse("8d") == 12345 && parse("99999999999") == 12345);
    CHECK(parse("") == 12345 && parse("x") == 12345 && parse("5q") == 12345 && parse("-5") == 12345);
    uint32_t m;
    CHECK(advcfg_parse_interval("30", &m) != NULL);
    char t[16];
    advcfg_interval_text(0, t, sizeof(t));
    CHECK(strcmp(t, "Off") == 0);
    advcfg_interval_text(720, t, sizeof(t));
    CHECK(strcmp(t, "12 h") == 0);
    advcfg_interval_text(2880, t, sizeof(t));
    CHECK(strcmp(t, "2 d") == 0);
    advcfg_interval_text(200, t, sizeof(t));
    CHECK(strcmp(t, "200 min") == 0);
    // Enter cycles through the presets, all within the limits, and back to off
    uint32_t v = 0;
    for (int i = 0; i < 7; i++) {
        v = advcfg_interval_cycle(v);
        CHECK(advcfg_interval_ok(v));
    }
    CHECK(v == 0);
    CHECK(advcfg_interval_cycle(200) == 360 && advcfg_interval_cycle(10080) == 0 && advcfg_interval_cycle(20000) == 0);
}

static void test_schedule(void) {
    CHECK(advcfg_next_due(0, 1000, 5000) == 0);                        // off
    CHECK(advcfg_next_due(720, 0, 0) == 0);                            // clock not set
    CHECK(advcfg_next_due(720, 0, 5000) == 5000 + ADVCFG_FIRST_DELAY_S); // never advertised
    CHECK(advcfg_next_due(720, 5000, 6000) == 5000 + 720 * 60);
    CHECK(advcfg_next_due(720, 9000, 6000) == 6000 + ADVCFG_FIRST_DELAY_S); // clock went back
    // never sooner than the interval after the last advert, whatever "now" is
    for (uint32_t now = 5000; now < 5000 + 720 * 60; now += 997) {
        CHECK(advcfg_next_due(720, 5000, now) >= 5000 + 180 * 60);
    }
    CHECK(advcfg_next_in_s(0, 100) == UINT32_MAX && advcfg_next_in_s(100, 0) == UINT32_MAX);
    CHECK(advcfg_next_in_s(500, 100) == 400 && advcfg_next_in_s(100, 500) == 0);
    char b[12];
    advcfg_next_badge(UINT32_MAX, b, sizeof(b));
    CHECK(b[0] == '\0');
    advcfg_next_badge(0, b, sizeof(b));
    CHECK(strcmp(b, "soon") == 0);
    advcfg_next_badge(125, b, sizeof(b));
    CHECK(strcmp(b, "in 2m") == 0);
    advcfg_next_badge(11 * 3600 + 3540, b, sizeof(b));
    CHECK(strcmp(b, "in 12h") == 0);
    advcfg_next_badge(3 * 86400, b, sizeof(b));
    CHECK(strcmp(b, "in 3d") == 0);
    CHECK(strlen(b) < 8); // MESH_GRID_BADGE_MAX
    advcfg_next_text(125, b, sizeof(b));
    CHECK(strcmp(b, "In 2 min") == 0);
    advcfg_next_text(11 * 3600 + 3540, b, sizeof(b));
    CHECK(strcmp(b, "In 12 h") == 0);
    advcfg_next_text(3 * 86400, b, sizeof(b));
    CHECK(strcmp(b, "In 3 d") == 0);
    advcfg_next_text(10, b, sizeof(b));
    CHECK(strcmp(b, "Soon") == 0);
    CHECK(advcfg_retry_at(1000, 720, false) == 1000 + ADVCFG_RETRY_S); // soft: again in a minute
    CHECK(advcfg_retry_at(1000, 720, true) == 1000 + 720 * 60);        // hard: next interval
    CHECK(advcfg_gap_ok(0, 5) && !advcfg_gap_ok(100, 129) && advcfg_gap_ok(100, 130));
}

static void test_coords(void) {
    double v = 99;
    CHECK(advcfg_parse_coord("52.3702", true, &v) == NULL && v > 52.37 && v < 52.371);
    CHECK(advcfg_parse_coord(" -33.5 ", true, &v) == NULL && v == -33.5);
    CHECK(advcfg_parse_coord("90", true, &v) == NULL && advcfg_parse_coord("-90", true, &v) == NULL);
    CHECK(advcfg_parse_coord("90.0001", true, &v) != NULL && advcfg_parse_coord("-91", true, &v) != NULL);
    CHECK(advcfg_parse_coord("180", false, &v) == NULL && v == 180);
    CHECK(advcfg_parse_coord("180.5", false, &v) != NULL && advcfg_parse_coord("-181", false, &v) != NULL);
    CHECK(advcfg_parse_coord("120", true, &v) != NULL && advcfg_parse_coord("120", false, &v) == NULL);
    v = 7;
    CHECK(advcfg_parse_coord("", true, &v) != NULL && advcfg_parse_coord("abc", true, &v) != NULL);
    CHECK(advcfg_parse_coord("52.3N", true, &v) != NULL && advcfg_parse_coord("5 2", true, &v) != NULL);
    CHECK(advcfg_parse_coord("nan", true, &v) != NULL && advcfg_parse_coord("inf", false, &v) != NULL);
    CHECK(v == 7); // a refusal leaves the value alone
    CHECK(!advcfg_location_set(0, 0) && advcfg_location_set(0, 4.9) && advcfg_location_set(52.3, 0));
    char t[40];
    advcfg_coord_text(52.3702, -4.8952, t, sizeof(t));
    CHECK(strcmp(t, "52.370200 -4.895200") == 0);
}

static advcfg_autoadd_t aa(bool on, uint8_t types, bool ow, uint8_t hops) {
    return (advcfg_autoadd_t){.on = on, .types = types, .overwrite = ow, .max_hops = hops};
}

static void test_autoadd_codec(void) {
    advcfg_autoadd_t a;
    advcfg_autoadd_decode(0, 0, 0, &a); // MeshCore's default: add everything
    CHECK(a.on && a.types == ADVCFG_AA_TYPES && !a.overwrite && a.max_hops == 0);
    advcfg_autoadd_decode(1, 0, 0, &a); // manual, no types: off
    CHECK(!a.on);
    advcfg_autoadd_decode(1, ADVCFG_AA_CHAT | ADVCFG_AA_OVERWRITE, 3, &a);
    CHECK(a.on && a.types == ADVCFG_AA_CHAT && a.overwrite && a.max_hops == 3);
    advcfg_autoadd_decode(0, 0, 200, &a);
    CHECK(a.max_hops == ADVCFG_HOPS_MAX);

    uint8_t m, c, h;
    advcfg_autoadd_t all = aa(true, ADVCFG_AA_TYPES, true, 2);
    advcfg_autoadd_encode(&all, &m, &c, &h);
    CHECK(m == 0 && c == (ADVCFG_AA_TYPES | ADVCFG_AA_OVERWRITE) && h == 2);
    advcfg_autoadd_t some = aa(true, ADVCFG_AA_CHAT | ADVCFG_AA_ROOM, false, 0);
    advcfg_autoadd_encode(&some, &m, &c, &h);
    CHECK(m == 1 && c == (ADVCFG_AA_CHAT | ADVCFG_AA_ROOM) && h == 0);
    advcfg_autoadd_t off = aa(false, ADVCFG_AA_TYPES, true, 0);
    advcfg_autoadd_encode(&off, &m, &c, &h);
    CHECK(m == 1 && c == ADVCFG_AA_OVERWRITE); // off: manual, no types (MyMesh adds nothing)
    advcfg_autoadd_t big = aa(true, ADVCFG_AA_TYPES, false, 250);
    advcfg_autoadd_encode(&big, &m, &c, &h);
    CHECK(h == ADVCFG_HOPS_MAX);
    // every state survives encode then decode
    for (int on = 0; on < 2; on++) {
        for (uint8_t t = 0; t < 16; t++) {
            for (int ow = 0; ow < 2; ow++) {
                advcfg_autoadd_t in = aa(on, (uint8_t)(t << 1), ow, 5), out;
                advcfg_autoadd_encode(&in, &m, &c, &h);
                advcfg_autoadd_decode(m, c, h, &out);
                bool want_on = on && in.types != 0;
                CHECK(out.on == want_on && out.overwrite == (bool)ow && out.max_hops == 5);
                if (want_on) {
                    CHECK(out.types == in.types);
                }
            }
        }
    }
}

static void test_autoadd_decide(void) {
    advcfg_autoadd_t a = aa(true, ADVCFG_AA_TYPES, false, 0);
    CHECK(advcfg_autoadd_decide(&a, 1, 0, false, true) == ADVCFG_ADD);
    CHECK(advcfg_autoadd_decide(&a, 9, 0, false, true) == ADVCFG_ADD); // add-all takes unknown types too
    a = aa(true, ADVCFG_AA_CHAT | ADVCFG_AA_REPEATER, false, 0);
    CHECK(advcfg_autoadd_decide(&a, 1, 0, false, true) == ADVCFG_ADD);
    CHECK(advcfg_autoadd_decide(&a, 3, 0, false, true) == ADVCFG_SKIP_TYPE);
    CHECK(advcfg_autoadd_decide(&a, 9, 0, false, true) == ADVCFG_SKIP_TYPE);
    a = aa(false, ADVCFG_AA_TYPES, true, 0);
    CHECK(advcfg_autoadd_decide(&a, 1, 0, false, true) == ADVCFG_SKIP_TYPE);
    a = aa(true, ADVCFG_AA_TYPES, false, 1); // direct only
    CHECK(advcfg_autoadd_decide(&a, 1, 0, false, true) == ADVCFG_ADD);
    CHECK(advcfg_autoadd_decide(&a, 1, 1, false, true) == ADVCFG_SKIP_HOPS);
    a.max_hops = 3; // up to 2 hops
    CHECK(advcfg_autoadd_decide(&a, 1, 2, false, true) == ADVCFG_ADD);
    CHECK(advcfg_autoadd_decide(&a, 1, 3, false, true) == ADVCFG_SKIP_HOPS);
    a.max_hops = 0;
    CHECK(advcfg_autoadd_decide(&a, 1, 60, true, true) == ADVCFG_FULL);        // full, no overwrite: the note
    a.overwrite = true;
    CHECK(advcfg_autoadd_decide(&a, 1, 60, true, true) == ADVCFG_REPLACE_OLDEST);
    CHECK(advcfg_autoadd_decide(&a, 1, 60, true, false) == ADVCFG_FULL);       // all favourites
    CHECK(advcfg_type_bit(0) == 0 && advcfg_type_bit(2) == ADVCFG_AA_REPEATER && advcfg_type_bit(5) == 0);
}

static void test_texts(void) {
    char t[40];
    advcfg_hops_text(0, t, sizeof(t));
    CHECK(strcmp(t, "Any") == 0);
    advcfg_hops_text(1, t, sizeof(t));
    CHECK(strcmp(t, "Direct only") == 0);
    advcfg_hops_text(4, t, sizeof(t));
    CHECK(strcmp(t, "Up to 3 hops") == 0);
    uint8_t h = 0;
    for (int i = 0; i < 8; i++) {
        h = advcfg_hops_cycle(h);
        CHECK(h <= ADVCFG_HOPS_MAX);
    }
    CHECK(h == 0 && advcfg_hops_cycle(5) == 6);
    advcfg_types_text(ADVCFG_AA_TYPES, t, sizeof(t));
    CHECK(strcmp(t, "Chat, Repeaters, Rooms, Sensors") == 0);
    advcfg_types_text(ADVCFG_AA_ROOM, t, sizeof(t));
    CHECK(strcmp(t, "Rooms") == 0);
    advcfg_types_text(0, t, sizeof(t));
    CHECK(strcmp(t, "no types") == 0);
    char tiny[6];
    advcfg_types_text(ADVCFG_AA_TYPES, tiny, sizeof(tiny)); // truncated, still terminated
    CHECK(strlen(tiny) < sizeof(tiny));
}

static void test_parse(void) {
    uint8_t v = 99;
    CHECK(advcfg_parse_hops("any", &v) == NULL && v == 0);
    CHECK(advcfg_parse_hops("direct", &v) == NULL && v == 1);
    CHECK(advcfg_parse_hops("0", &v) == NULL && v == 1);
    CHECK(advcfg_parse_hops("3", &v) == NULL && v == 4);
    CHECK(advcfg_parse_hops("63", &v) == NULL && v == 64);
    v = 99;
    CHECK(advcfg_parse_hops("64", &v) != NULL && advcfg_parse_hops("-1", &v) != NULL);
    CHECK(advcfg_parse_hops("x", &v) != NULL && advcfg_parse_hops("", &v) != NULL && v == 99);
    CHECK(advcfg_parse_types("all", &v) == NULL && v == ADVCFG_AA_TYPES);
    CHECK(advcfg_parse_types("chat,room", &v) == NULL && v == (ADVCFG_AA_CHAT | ADVCFG_AA_ROOM));
    CHECK(advcfg_parse_types("repeaters sensor", &v) == NULL && v == (ADVCFG_AA_REPEATER | ADVCFG_AA_SENSOR));
    v = 99;
    CHECK(advcfg_parse_types("", &v) != NULL && advcfg_parse_types("chat,phone", &v) != NULL && v == 99);
    CHECK(advcfg_parse_types("averyveryverylongword", &v) != NULL);
}

int main(void) {
    test_interval();
    test_schedule();
    test_coords();
    test_autoadd_codec();
    test_autoadd_decide();
    test_texts();
    test_parse();
    if (failures) {
        printf("mesh_advert_cfg_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("mesh_advert_cfg_test: all passed\n");
    return 0;
}
