// Telemetry request to a contact (#162): see mesh_telem.h
#include "mesh_telem.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// Cayenne LPP types (electroniccats/CayenneLPP): the ones sensors of MeshCore nodes send
enum {
    LPP_DIGITAL_IN  = 0,
    LPP_DIGITAL_OUT = 1,
    LPP_ANALOG_IN   = 2,
    LPP_ANALOG_OUT  = 3,
    LPP_GENERIC     = 100,
    LPP_LUMINOSITY  = 101,
    LPP_PRESENCE    = 102,
    LPP_TEMPERATURE = 103,
    LPP_HUMIDITY    = 104,
    LPP_ACCEL       = 113,
    LPP_BAROMETER   = 115,
    LPP_VOLTAGE     = 116,
    LPP_CURRENT     = 117,
    LPP_FREQUENCY   = 118,
    LPP_PERCENTAGE  = 120,
    LPP_ALTITUDE    = 121,
    LPP_CONCENTR    = 125,
    LPP_POWER       = 128,
    LPP_DISTANCE    = 130,
    LPP_ENERGY      = 131,
    LPP_DIRECTION   = 132,
    LPP_UNIXTIME    = 133,
    LPP_GYRO        = 134,
    LPP_GPS         = 136,
    LPP_SWITCH      = 142,
};

#define TELEM_CHANNEL_SELF 1 // MeshCore: the node's own battery voltage, temperature and GPS

typedef struct {
    uint8_t size;
    bool    is_signed;
} lpp_type_t;

static bool lpp_type(uint8_t t, lpp_type_t *o) {
    switch (t) {
    case LPP_DIGITAL_IN: case LPP_DIGITAL_OUT: case LPP_PRESENCE: case LPP_SWITCH: case LPP_PERCENTAGE:
    case LPP_HUMIDITY:
        *o = (lpp_type_t){1, false}; return true;
    case LPP_ANALOG_IN: case LPP_ANALOG_OUT: case LPP_TEMPERATURE: case LPP_ALTITUDE:
        *o = (lpp_type_t){2, true}; return true;
    case LPP_LUMINOSITY: case LPP_BAROMETER: case LPP_VOLTAGE: case LPP_CURRENT: case LPP_CONCENTR: case LPP_POWER:
    case LPP_DIRECTION:
        *o = (lpp_type_t){2, false}; return true;
    case LPP_GENERIC: case LPP_FREQUENCY: case LPP_DISTANCE: case LPP_ENERGY: case LPP_UNIXTIME:
        *o = (lpp_type_t){4, false}; return true;
    case LPP_ACCEL: case LPP_GYRO:
        *o = (lpp_type_t){6, true}; return true;
    case LPP_GPS:
        *o = (lpp_type_t){9, true}; return true;
    default: return false;
    }
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
static void label(char *out, size_t cap, char const *name, uint8_t ch) {
    if (ch == TELEM_CHANNEL_SELF) {
        snprintf(out, cap, "%s", name);
    } else {
        snprintf(out, cap, "%s (channel %u)", name, ch);
    }
}

static void one_value(tl_line_t *out, size_t max, size_t *n, uint8_t ch, uint8_t type, uint8_t const *d, bool is_signed) {
    char     name[40], v[32], v2[32], v3[32];
    int      sz = 0;
    lpp_type_t lt;
    lpp_type(type, &lt);
    sz = lt.size;
    int64_t x = be(d, sz > 4 ? 4 : sz, is_signed);
    if (sz == 4 && !is_signed) {
        x = (int64_t)(uint32_t)be(d, 4, false);
    }
    switch (type) {
    case LPP_VOLTAGE:
        label(name, sizeof(name), ch == TELEM_CHANNEL_SELF ? "Battery" : "Voltage", ch);
        fixed(v, sizeof(v), x, 100, 2);
        put(out, max, n, "%s: %s V", name, v);
        break;
    case LPP_TEMPERATURE:
        label(name, sizeof(name), "Temperature", ch);
        fixed(v, sizeof(v), x, 10, 1);
        put(out, max, n, "%s: %s \xC2\xB0""C", name, v);
        break;
    case LPP_HUMIDITY:
        label(name, sizeof(name), "Humidity", ch);
        fixed(v, sizeof(v), x * 5, 10, 1); // 0.5 % steps
        put(out, max, n, "%s: %s %%", name, v);
        break;
    case LPP_BAROMETER:
        label(name, sizeof(name), "Pressure", ch);
        fixed(v, sizeof(v), x, 10, 1);
        put(out, max, n, "%s: %s hPa", name, v);
        break;
    case LPP_LUMINOSITY:
        label(name, sizeof(name), "Light", ch);
        put(out, max, n, "%s: %lu lux", name, (unsigned long)x);
        break;
    case LPP_CURRENT:
        label(name, sizeof(name), "Current", ch);
        fixed(v, sizeof(v), x, 1000, 3);
        put(out, max, n, "%s: %s A", name, v);
        break;
    case LPP_POWER:
        label(name, sizeof(name), "Power", ch);
        put(out, max, n, "%s: %ld W", name, (long)x);
        break;
    case LPP_ENERGY:
        label(name, sizeof(name), "Energy", ch);
        fixed(v, sizeof(v), x, 1000, 3);
        put(out, max, n, "%s: %s kWh", name, v);
        break;
    case LPP_DISTANCE:
        label(name, sizeof(name), "Distance", ch);
        fixed(v, sizeof(v), x, 1000, 3);
        put(out, max, n, "%s: %s m", name, v);
        break;
    case LPP_ALTITUDE:
        label(name, sizeof(name), "Altitude", ch);
        put(out, max, n, "%s: %ld m", name, (long)x);
        break;
    case LPP_FREQUENCY:
        label(name, sizeof(name), "Frequency", ch);
        put(out, max, n, "%s: %lu Hz", name, (unsigned long)x);
        break;
    case LPP_PERCENTAGE:
        label(name, sizeof(name), "Level", ch);
        put(out, max, n, "%s: %ld %%", name, (long)x);
        break;
    case LPP_CONCENTR:
        label(name, sizeof(name), "Concentration", ch);
        put(out, max, n, "%s: %ld ppm", name, (long)x);
        break;
    case LPP_DIRECTION:
        label(name, sizeof(name), "Direction", ch);
        put(out, max, n, "%s: %ld\xC2\xB0", name, (long)x);
        break;
    case LPP_PRESENCE:
    case LPP_SWITCH:
    case LPP_DIGITAL_IN:
    case LPP_DIGITAL_OUT:
        label(name, sizeof(name), type == LPP_PRESENCE ? "Presence" : type == LPP_SWITCH ? "Switch" : "Input", ch);
        put(out, max, n, "%s: %s", name, x ? "on" : "off");
        break;
    case LPP_ANALOG_IN:
    case LPP_ANALOG_OUT:
        label(name, sizeof(name), "Analog value", ch);
        fixed(v, sizeof(v), x, 100, 2);
        put(out, max, n, "%s: %s", name, v);
        break;
    case LPP_UNIXTIME:
        label(name, sizeof(name), "Time", ch);
        put(out, max, n, "%s: %lu s since 1970", name, (unsigned long)x);
        break;
    case LPP_ACCEL:
    case LPP_GYRO: {
        int div = type == LPP_ACCEL ? 1000 : 100;
        int dec = type == LPP_ACCEL ? 3 : 2;
        label(name, sizeof(name), type == LPP_ACCEL ? "Acceleration" : "Rotation", ch);
        fixed(v, sizeof(v), be(d, 2, true), div, dec);
        fixed(v2, sizeof(v2), be(d + 2, 2, true), div, dec);
        fixed(v3, sizeof(v3), be(d + 4, 2, true), div, dec);
        put(out, max, n, "%s: x %s, y %s, z %s %s", name, v, v2, v3, type == LPP_ACCEL ? "g" : "\xC2\xB0/s");
        break;
    }
    case LPP_GPS:
        label(name, sizeof(name), "Location", ch);
        fixed(v, sizeof(v), be(d, 3, true), 10000, 4); // 0.0001 degree steps
        fixed(v2, sizeof(v2), be(d + 3, 3, true), 10000, 4);
        fixed(v3, sizeof(v3), be(d + 6, 3, true), 100, 0); // 0.01 m steps, shown in whole metres
        put(out, max, n, "%s: %s, %s, %s m", name, v, v2, v3);
        break;
    default: // LPP_GENERIC
        label(name, sizeof(name), "Value", ch);
        put(out, max, n, "%s: %lu", name, (unsigned long)x);
        break;
    }
}

size_t tl_lpp_lines(uint8_t const *lpp, size_t len, tl_line_t *out, size_t max) {
    size_t n = 0, at = 0;
    while (at < len && n < max) {
        if (len - at < 2) {
            put(out, max, &n, "(the answer is cut off)");
            break;
        }
        uint8_t    ch = lpp[at], type = lpp[at + 1];
        lpp_type_t lt;
        if (ch == 0 && type == 0) {
            break; // zero padding after the last value
        }
        if (!lpp_type(type, &lt)) {
            put(out, max, &n, "(a value of type %u could not be read)", type);
            break;
        }
        if (len - at - 2 < lt.size) {
            put(out, max, &n, "(the answer is cut off)");
            break;
        }
        one_value(out, max, &n, ch, type, lpp + at + 2, lt.is_signed);
        at += 2u + lt.size;
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
