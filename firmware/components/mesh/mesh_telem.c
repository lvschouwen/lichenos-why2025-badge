// Telemetry request to a contact (#162): see mesh_telem.h
#include "mesh_telem.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define TELEM_CHANNEL_SELF 1 // MeshCore: the node's own battery voltage, temperature and GPS

#define DEG "\xC2\xB0"

// How a value is shown: one scaled number, on/off, three axes, or a location
typedef enum { LPP_SHOW_NUM, LPP_SHOW_ONOFF, LPP_SHOW_XYZ, LPP_SHOW_GPS } lpp_show_t;

// A Cayenne LPP type (electroniccats/CayenneLPP): the ones sensors of MeshCore nodes send. The value is shown as
// raw * mul / div with dec decimals, then unit (which carries its own leading space). One table, so the length a
// type has (which the parser needs to step over it) and how it is shown cannot drift apart (#212).
typedef struct {
    uint8_t     type, size;
    bool        is_signed;
    uint8_t     show; // lpp_show_t
    uint8_t     mul;
    uint8_t     dec : 3; // decimals; 3 bits also tell the compiler the text fits fixed()'s 32 bytes
    uint16_t    div;
    char const *name, *self_name; // self_name: the name on TELEM_CHANNEL_SELF, NULL = name
    char const *unit;
} lpp_type_t;

static lpp_type_t const LPP_TYPES[] = {
    {0, 1, false, LPP_SHOW_ONOFF, 1, 0, 1, "Input", NULL, ""},                // digital input
    {1, 1, false, LPP_SHOW_ONOFF, 1, 0, 1, "Input", NULL, ""},                // digital output
    {2, 2, true, LPP_SHOW_NUM, 1, 2, 100, "Analog value", NULL, ""},          // analog input
    {3, 2, true, LPP_SHOW_NUM, 1, 2, 100, "Analog value", NULL, ""},          // analog output
    {100, 4, false, LPP_SHOW_NUM, 1, 0, 1, "Value", NULL, ""},                // generic sensor
    {101, 2, false, LPP_SHOW_NUM, 1, 0, 1, "Light", NULL, " lux"},            // luminosity
    {102, 1, false, LPP_SHOW_ONOFF, 1, 0, 1, "Presence", NULL, ""},           // presence
    {103, 2, true, LPP_SHOW_NUM, 1, 1, 10, "Temperature", NULL, " " DEG "C"}, // temperature
    {104, 1, false, LPP_SHOW_NUM, 5, 1, 10, "Humidity", NULL, " %"},          // humidity, 0.5 % steps
    {113, 6, true, LPP_SHOW_XYZ, 1, 3, 1000, "Acceleration", NULL, " g"},     // accelerometer
    {115, 2, false, LPP_SHOW_NUM, 1, 1, 10, "Pressure", NULL, " hPa"},        // barometer
    {116, 2, false, LPP_SHOW_NUM, 1, 2, 100, "Voltage", "Battery", " V"},     // voltage
    {117, 2, false, LPP_SHOW_NUM, 1, 3, 1000, "Current", NULL, " A"},         // current
    {118, 4, false, LPP_SHOW_NUM, 1, 0, 1, "Frequency", NULL, " Hz"},         // frequency
    {120, 1, false, LPP_SHOW_NUM, 1, 0, 1, "Level", NULL, " %"},              // percentage
    {121, 2, true, LPP_SHOW_NUM, 1, 0, 1, "Altitude", NULL, " m"},            // altitude
    {125, 2, false, LPP_SHOW_NUM, 1, 0, 1, "Concentration", NULL, " ppm"},    // concentration
    {128, 2, false, LPP_SHOW_NUM, 1, 0, 1, "Power", NULL, " W"},              // power
    {130, 4, false, LPP_SHOW_NUM, 1, 3, 1000, "Distance", NULL, " m"},        // distance
    {131, 4, false, LPP_SHOW_NUM, 1, 3, 1000, "Energy", NULL, " kWh"},        // energy
    {132, 2, false, LPP_SHOW_NUM, 1, 0, 1, "Direction", NULL, DEG},           // direction
    {133, 4, false, LPP_SHOW_NUM, 1, 0, 1, "Time", NULL, " s since 1970"},    // unix time
    {134, 6, true, LPP_SHOW_XYZ, 1, 2, 100, "Rotation", NULL, " " DEG "/s"},  // gyrometer
    {136, 9, true, LPP_SHOW_GPS, 1, 4, 10000, "Location", NULL, " m"},        // GPS: 0.0001 degree steps
    {142, 1, false, LPP_SHOW_ONOFF, 1, 0, 1, "Switch", NULL, ""},             // switch
};

// NULL for a type we do not know (its length is unknown too)
static lpp_type_t const *lpp_type(uint8_t t) {
    for (size_t i = 0; i < sizeof(LPP_TYPES) / sizeof(LPP_TYPES[0]); i++) {
        if (LPP_TYPES[i].type == t) {
            return &LPP_TYPES[i];
        }
    }
    return NULL;
}

static int32_t be(uint8_t const *p, int n, bool is_signed) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) {
        v = v << 8 | p[i];
    }
    if (is_signed && n < 4 && (v & (1u << (8 * n - 1)))) {
        return (int32_t)(v | (0xFFFFFFFFu << (8 * n)));
    }
    return (int32_t)v;
}

// value / div with `dec` decimals, sign kept for values between -1 and 0 ("-0.5")
static void fixed(char *out, size_t cap, int64_t v, int div, int dec) {
    bool     neg = v < 0;
    uint64_t a   = (uint64_t)(neg ? -v : v);
    if (dec == 0) {
        snprintf(out, cap, "%s%llu", neg ? "-" : "", (unsigned long long)(a / (uint64_t)div));
    } else {
        snprintf(out, cap, "%s%llu.%0*llu", neg ? "-" : "", (unsigned long long)(a / (uint64_t)div), dec,
                 (unsigned long long)(a % (uint64_t)div));
    }
}

static void put(tl_line_t *out, size_t max, size_t *n, char const *fmt, ...) __attribute__((format(printf, 4, 5)));
static void put(tl_line_t *out, size_t max, size_t *n, char const *fmt, ...) {
    if (*n >= max) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out[*n].text, TL_LINE_MAX, fmt, ap);
    va_end(ap);
    (*n)++;
}

// "Battery" or "Temperature (channel 3)"
static void label(char *out, size_t cap, lpp_type_t const *lt, uint8_t ch) {
    if (ch == TELEM_CHANNEL_SELF) {
        snprintf(out, cap, "%s", lt->self_name ? lt->self_name : lt->name);
    } else {
        snprintf(out, cap, "%s (channel %u)", lt->name, ch);
    }
}

// One line for one value of type lt at d (lt->size bytes, all there): "Battery: 4.12 V", "Switch: on". put() cuts it
// at TL_LINE_MAX, as it cuts every line.
static void one_value(tl_line_t *out, size_t max, size_t *n, uint8_t ch, lpp_type_t const *lt, uint8_t const *d) {
    char name[40], v[3][32];
    label(name, sizeof(name), lt, ch);
    switch (lt->show) {
    case LPP_SHOW_ONOFF:
        put(out, max, n, "%s: %s", name, be(d, lt->size, lt->is_signed) ? "on" : "off");
        break;
    case LPP_SHOW_XYZ:
        for (int i = 0; i < 3; i++) {
            fixed(v[i], sizeof(v[i]), be(d + 2 * i, 2, true), lt->div, lt->dec);
        }
        put(out, max, n, "%s: x %s, y %s, z %s%s", name, v[0], v[1], v[2], lt->unit);
        break;
    case LPP_SHOW_GPS:
        fixed(v[0], sizeof(v[0]), be(d, 3, true), lt->div, lt->dec);
        fixed(v[1], sizeof(v[1]), be(d + 3, 3, true), lt->div, lt->dec);
        fixed(v[2], sizeof(v[2]), be(d + 6, 3, true), 100, 0); // 0.01 m steps, shown in whole metres
        put(out, max, n, "%s: %s, %s, %s%s", name, v[0], v[1], v[2], lt->unit);
        break;
    default: { // LPP_SHOW_NUM: at most 4 bytes; a 4-byte one is unsigned, kept whole above INT32_MAX
        int64_t x = lt->size == 4 && !lt->is_signed
                        ? (int64_t)(uint32_t)be(d, 4, false) : be(d, lt->size, lt->is_signed);
        fixed(v[0], sizeof(v[0]), x * lt->mul, lt->div, lt->dec);
        put(out, max, n, "%s: %s%s", name, v[0], lt->unit);
        break;
    }
    }
}

size_t tl_lpp_lines(uint8_t const *lpp, size_t len, tl_line_t *out, size_t max) {
    size_t n = 0, at = 0;
    while (at < len) {
        if (n == max) {
            // more values than lines: count the rest (a value we cannot size ends the count) and say so
            size_t more = 0;
            for (; at + 2 <= len; more++) {
                lpp_type_t const *u = lpp_type(lpp[at + 1]);
                if (!u || len - at - 2 < u->size) {
                    more++;
                    break;
                }
                at += 2u + u->size;
            }
            if (max > 0 && more > 0) {
                snprintf(out[max - 1].text, TL_LINE_MAX, "%u more values not shown", (unsigned)more + 1u); // replaces the last
            }
            break;
        }
        if (len - at < 2) {
            put(out, max, &n, "(the answer is cut off)");
            break;
        }
        uint8_t           ch = lpp[at], type = lpp[at + 1];
        lpp_type_t const *lt = lpp_type(type);
        if (!lt) {
            put(out, max, &n, "(a value of type %u could not be read)", type);
            break;
        }
        if (len - at - 2 < lt->size) {
            put(out, max, &n, "(the answer is cut off)");
            break;
        }
        one_value(out, max, &n, ch, lt, lpp + at + 2);
        at += 2u + lt->size;
    }
    return n;
}

void tl_rate_init(tl_rate_t *t) {
    memset(t, 0, sizeof(*t));
}

uint32_t tl_rate_wait_s(tl_rate_t const *t, uint8_t const prefix[MF_PREFIX], uint32_t now_ms) {
    for (int i = 0; i < TL_RATE_SLOTS; i++) {
        tl_slot_t const *s = &t->slot[i];
        if (s->used && memcmp(s->prefix, prefix, MF_PREFIX) == 0) {
            uint32_t since = now_ms - s->at_ms;
            return since >= TL_RATE_MS ? 0 : (TL_RATE_MS - since + 999) / 1000;
        }
    }
    return 0;
}

void tl_rate_note(tl_rate_t *t, uint8_t const prefix[MF_PREFIX], uint32_t now_ms) {
    tl_slot_t *pick = &t->slot[0];
    for (int i = 0; i < TL_RATE_SLOTS; i++) {
        tl_slot_t *s = &t->slot[i];
        if (s->used && memcmp(s->prefix, prefix, MF_PREFIX) == 0) {
            pick = s;
            break;
        }
        if (!s->used) {
            pick = s; // a free slot beats the oldest one, but keep looking for this contact's own
        } else if (pick->used && (int32_t)(now_ms - s->at_ms) > (int32_t)(now_ms - pick->at_ms)) {
            pick = s;
        }
    }
    memcpy(pick->prefix, prefix, MF_PREFIX);
    pick->at_ms = now_ms;
    pick->used  = true;
}

size_t tl_view_text(rr_t const *r, uint8_t const target[MF_PREFIX], char *out, size_t cap) {
    size_t at = 0;
    if (cap == 0) {
        return 0;
    }
    out[0] = 0;
    bool       have = r->have_telem && memcmp(r->telem_prefix, target, MF_PREFIX) == 0;
    tl_line_t *lines = NULL;
    tl_line_t  buf[TL_LINES_MAX];
    size_t     n = 0;
    lines        = buf;
    if (have) {
        n = tl_lpp_lines(r->telem, r->telem_len, lines, TL_LINES_MAX);
        if (n == 0) {
            put(lines, TL_LINES_MAX, &n, "The answer holds no values.");
        }
    }
    for (size_t i = 0; i < n && at + 1 < cap; i++) {
        at += (size_t)snprintf(out + at, cap - at, "%s%s", i ? "\n" : "", lines[i].text);
    }
    if (at >= cap) {
        at = cap - 1;
    }
    if (r->op == RR_TELEMETRY && r->state != RR_DONE) {
        char state[128];
        rr_state_text(r, state, sizeof(state));
        at += (size_t)snprintf(out + at, cap - at, "%s%s%s", at ? "\n\n" : "", have ? "(last answer above) " : "", state);
        if (at >= cap) {
            at = cap - 1;
        }
    }
    return at;
}
