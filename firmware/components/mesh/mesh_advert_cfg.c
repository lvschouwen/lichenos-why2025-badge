// The advert, location and auto-add rules (#52): see mesh_advert_cfg.h
#include "mesh_advert_cfg.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool advcfg_interval_ok(uint32_t minutes) {
    return minutes == 0 || (minutes >= ADVCFG_INTERVAL_MIN && minutes <= ADVCFG_INTERVAL_MAX);
}

char const *advcfg_parse_interval(char const *text, uint32_t *minutes) {
    while (*text == ' ') {
        text++;
    }
    if (strcmp(text, "off") == 0 || strcmp(text, "0") == 0) {
        *minutes = 0;
        return NULL;
    }
    if (*text < '0' || *text > '9') {
        return "type off, or a time like 180, 6h or 2d";
    }
    char         *end;
    unsigned long v = strtoul(text, &end, 10);
    unsigned long unit = 1;
    while (*end == ' ') {
        end++;
    }
    if (*end == 'h' || *end == 'H') {
        unit = 60, end++;
    } else if (*end == 'd' || *end == 'D') {
        unit = 1440, end++;
    } else if (strncmp(end, "min", 3) == 0) {
        end += 3;
    } else if (*end == 'm') {
        end++;
    }
    while (*end == ' ') {
        end++;
    }
    if (*end || v > ADVCFG_INTERVAL_MAX) { // v > max also keeps v * unit from overflowing
        return *end ? "type off, or a time like 180, 6h or 2d" : "interval is 3 hours to 7 days (180 to 10080 min), or off";
    }
    v *= unit;
    if (!advcfg_interval_ok((uint32_t)v)) {
        return "interval is 3 hours to 7 days (180 to 10080 min), or off";
    }
    *minutes = (uint32_t)v;
    return NULL;
}

void advcfg_interval_text(uint32_t minutes, char *out, size_t cap) {
    if (cap == 0) {
        return;
    }
    if (minutes == 0) {
        snprintf(out, cap, "Off");
    } else if (minutes % 1440 == 0) {
        snprintf(out, cap, "%u d", (unsigned)(minutes / 1440));
    } else if (minutes % 60 == 0) {
        snprintf(out, cap, "%u h", (unsigned)(minutes / 60));
    } else {
        snprintf(out, cap, "%u min", (unsigned)minutes);
    }
}

uint32_t advcfg_interval_cycle(uint32_t minutes) {
    static uint32_t const steps[] = {0, 180, 360, 720, 1440, 2880, 10080};
    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        if (minutes < steps[i]) {
            return steps[i]; // an in-between value (set from the console) goes to the next preset up
        }
        if (minutes == steps[i]) {
            return steps[(i + 1) % (sizeof(steps) / sizeof(steps[0]))];
        }
    }
    return 0; // above the last preset: wrap to off
}

uint32_t advcfg_next_due(uint32_t interval_min, uint32_t last_epoch, uint32_t now_epoch) {
    if (interval_min == 0 || now_epoch == 0) {
        return 0;
    }
    if (last_epoch == 0 || last_epoch > now_epoch) {
        return now_epoch + ADVCFG_FIRST_DELAY_S;
    }
    return last_epoch + interval_min * 60;
}

uint32_t advcfg_next_in_s(uint32_t next_epoch, uint32_t now_epoch) {
    if (next_epoch == 0 || now_epoch == 0) {
        return UINT32_MAX;
    }
    return next_epoch > now_epoch ? next_epoch - now_epoch : 0;
}

void advcfg_next_badge(uint32_t next_in_s, char *out, size_t cap) {
    if (cap == 0) {
        return;
    }
    if (next_in_s == UINT32_MAX) {
        out[0] = '\0';
    } else if (next_in_s < 60) {
        snprintf(out, cap, "<1m");
    } else if (next_in_s < 3600) {
        snprintf(out, cap, "%um", (unsigned)(next_in_s / 60));
    } else if (next_in_s < 86400) {
        snprintf(out, cap, "%uh", (unsigned)((next_in_s + 1800) / 3600)); // rounded: 11h59m reads 12h
    } else {
        snprintf(out, cap, "%ud", (unsigned)((next_in_s + 43200) / 86400));
    }
}

void advcfg_next_text(uint32_t next_in_s, char *out, size_t cap) {
    if (cap == 0) {
        return;
    }
    if (next_in_s == UINT32_MAX) {
        out[0] = '\0';
    } else if (next_in_s < 60) {
        snprintf(out, cap, "Soon");
    } else if (next_in_s < 3600) {
        snprintf(out, cap, "In %u min", (unsigned)(next_in_s / 60));
    } else if (next_in_s < 86400) {
        snprintf(out, cap, "In %u h", (unsigned)((next_in_s + 1800) / 3600));
    } else {
        snprintf(out, cap, "In %u d", (unsigned)((next_in_s + 43200) / 86400));
    }
}

uint32_t advcfg_retry_at(uint32_t now_epoch, uint32_t interval_min, bool hard) {
    return now_epoch + (hard ? interval_min * 60 : ADVCFG_RETRY_S);
}

bool advcfg_gap_ok(uint32_t last_s, uint32_t now_s) {
    return last_s == 0 || now_s - last_s >= ADVCFG_MIN_GAP_S;
}

char const *advcfg_parse_coord(char const *text, bool is_lat, double *out) {
    while (*text == ' ') {
        text++;
    }
    if (*text == '\0') {
        return "type a number like 52.3702";
    }
    char  *end;
    double v = strtod(text, &end);
    while (*end == ' ') {
        end++;
    }
    if (end == text || *end || !isfinite(v)) {
        return "not a number: type degrees like 52.3702 (a dot, no N/S/E/W)";
    }
    double lim = is_lat ? 90.0 : 180.0;
    if (v < -lim || v > lim) {
        return is_lat ? "latitude is -90 to 90" : "longitude is -180 to 180";
    }
    *out = v;
    return NULL;
}

char const *advcfg_coord_submit(char const *text, bool is_lat, bool hand_ok, double *out) {
    double      v;
    char const *why = advcfg_parse_coord(text, is_lat, &v);
    if (why) {
        return why;
    }
    if (!hand_ok) {
        return ADVCFG_COORD_HAND_ONLY;
    }
    *out = v;
    return NULL;
}

void advcfg_coord_text(double lat, double lon, char *out, size_t cap) {
    snprintf(out, cap, "%.6f %.6f", lat, lon);
}

bool advcfg_location_set(double lat, double lon) {
    return !(lat == 0.0 && lon == 0.0);
}

uint8_t advcfg_type_bit(uint8_t adv_type) {
    switch (adv_type) {
        case 1: return ADVCFG_AA_CHAT;
        case 2: return ADVCFG_AA_REPEATER;
        case 3: return ADVCFG_AA_ROOM;
        case 4: return ADVCFG_AA_SENSOR;
        default: return 0;
    }
}

void advcfg_autoadd_decode(uint8_t manual_add, uint8_t config, uint8_t max_hops, advcfg_autoadd_t *out) {
    bool manual    = (manual_add & 1) != 0;
    out->types     = manual ? (config & ADVCFG_AA_TYPES) : ADVCFG_AA_TYPES;
    out->on        = !manual || out->types != 0;
    out->overwrite = (config & ADVCFG_AA_OVERWRITE) != 0;
    out->max_hops  = max_hops > ADVCFG_HOPS_MAX ? ADVCFG_HOPS_MAX : max_hops;
}

void advcfg_autoadd_encode(advcfg_autoadd_t const *in, uint8_t *manual_add, uint8_t *config, uint8_t *max_hops) {
    uint8_t types = in->types & ADVCFG_AA_TYPES;
    bool    all   = in->on && types == ADVCFG_AA_TYPES;
    *manual_add   = all ? 0 : 1;
    *config       = (uint8_t)((in->on ? types : 0) | (in->overwrite ? ADVCFG_AA_OVERWRITE : 0));
    *max_hops     = in->max_hops > ADVCFG_HOPS_MAX ? ADVCFG_HOPS_MAX : in->max_hops;
}

advcfg_decision_t advcfg_autoadd_decide(advcfg_autoadd_t const *a, uint8_t adv_type, uint8_t hops, bool table_full,
                                        bool has_non_favourite) {
    uint8_t bit = advcfg_type_bit(adv_type);
    // MyMesh with every type on is the "add all" mode, which also takes types it does not know
    bool all = a->on && (a->types & ADVCFG_AA_TYPES) == ADVCFG_AA_TYPES;
    if (!a->on || (!all && (bit == 0 || !(a->types & bit)))) {
        return ADVCFG_SKIP_TYPE;
    }
    if (a->max_hops > 0 && hops >= a->max_hops) {
        return ADVCFG_SKIP_HOPS;
    }
    if (!table_full) {
        return ADVCFG_ADD;
    }
    return a->overwrite && has_non_favourite ? ADVCFG_REPLACE_OLDEST : ADVCFG_FULL;
}

void advcfg_hops_text(uint8_t max_hops, char *out, size_t cap) {
    if (cap == 0) {
        return;
    }
    if (max_hops == 0) {
        snprintf(out, cap, "Any");
    } else if (max_hops == 1) {
        snprintf(out, cap, "Direct only");
    } else {
        snprintf(out, cap, "Up to %u hops", (unsigned)(max_hops - 1));
    }
}

uint8_t advcfg_hops_cycle(uint8_t max_hops) {
    static uint8_t const steps[] = {0, 1, 2, 3, 4, 6, 9, 17};
    for (size_t i = 0; i < sizeof(steps); i++) {
        if (max_hops < steps[i]) {
            return steps[i];
        }
        if (max_hops == steps[i]) {
            return steps[(i + 1) % sizeof(steps)];
        }
    }
    return 0;
}

void advcfg_types_text(uint8_t types, char *out, size_t cap) {
    static struct {
        uint8_t     bit;
        char const *name;
    } const all[] = {{ADVCFG_AA_CHAT, "Chat"},
                     {ADVCFG_AA_REPEATER, "Repeaters"},
                     {ADVCFG_AA_ROOM, "Rooms"},
                     {ADVCFG_AA_SENSOR, "Sensors"}};
    if (cap == 0) {
        return;
    }
    out[0]   = '\0';
    size_t n = 0;
    for (size_t i = 0; i < 4; i++) {
        if (types & all[i].bit) {
            int w = snprintf(out + n, cap - n, "%s%s", n ? ", " : "", all[i].name);
            if (w < 0 || (size_t)w >= cap - n) {
                return;
            }
            n += (size_t)w;
        }
    }
    if (n == 0) {
        snprintf(out, cap, "no types");
    }
}

char const *advcfg_parse_hops(char const *text, uint8_t *max_hops) {
    if (strcmp(text, "any") == 0) {
        *max_hops = 0;
        return NULL;
    }
    if (strcmp(text, "direct") == 0) {
        *max_hops = 1;
        return NULL;
    }
    char         *end;
    unsigned long n = strtoul(text, &end, 10);
    if (end == text || *end || text[0] == '-') {
        return "hops: any, direct, or a number";
    }
    if (n + 1 > ADVCFG_HOPS_MAX) {
        return "at most 63 hops";
    }
    *max_hops = (uint8_t)(n + 1); // MyMesh: N means up to N-1 hops
    return NULL;
}

char const *advcfg_parse_types(char const *text, uint8_t *types) {
    static struct {
        char const *name;
        uint8_t     bit;
    } const names[] = {{"chat", ADVCFG_AA_CHAT},         {"companion", ADVCFG_AA_CHAT}, {"companions", ADVCFG_AA_CHAT},
                       {"repeater", ADVCFG_AA_REPEATER}, {"repeaters", ADVCFG_AA_REPEATER}, {"room", ADVCFG_AA_ROOM},
                       {"rooms", ADVCFG_AA_ROOM},        {"sensor", ADVCFG_AA_SENSOR},     {"sensors", ADVCFG_AA_SENSOR}};
    if (strcmp(text, "all") == 0) {
        *types = ADVCFG_AA_TYPES;
        return NULL;
    }
    uint8_t got = 0;
    char    word[16];
    while (*text) {
        while (*text == ' ' || *text == ',') {
            text++;
        }
        size_t n = 0;
        while (text[n] && text[n] != ' ' && text[n] != ',') {
            n++;
        }
        if (n == 0) {
            break;
        }
        if (n >= sizeof(word)) {
            return "types: all, or chat, repeater, room, sensor";
        }
        memcpy(word, text, n);
        word[n]  = '\0';
        text    += n;
        uint8_t bit = 0;
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
            if (strcmp(word, names[i].name) == 0) {
                bit = names[i].bit;
            }
        }
        if (!bit) {
            return "types: all, or chat, repeater, room, sensor";
        }
        got |= bit;
    }
    if (!got) {
        return "types: all, or chat, repeater, room, sensor";
    }
    *types = got;
    return NULL;
}
