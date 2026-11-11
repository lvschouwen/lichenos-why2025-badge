#include "reg_row_text.h"

#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    char value[40];
    char note[28]; // the age ("3.2 s") of a read, or the reason of an unread key
    bool stale;
    bool unread;
} parts_t;

static void copy_n(char *dst, size_t cap, char const *src, size_t n) {
    n = n < cap - 1 ? n : cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

// "[stale, ]<n>[.<n>] s ago": the only age forms lichen_reg_cached_text prints (the note of a read key)
static bool is_age(char const *note, size_t n) {
    if (n >= 7 && strncmp(note, "stale, ", 7) == 0) {
        note += 7;
        n -= 7;
    }
    if (n < 6 || strncmp(note + n - 6, " s ago", 6) != 0) {
        return false;
    }
    size_t digits = 0, dots = 0;
    for (size_t i = 0; i + 6 < n; i++) {
        dots += note[i] == '.';
        digits += note[i] >= '0' && note[i] <= '9';
        if (!((note[i] >= '0' && note[i] <= '9') || (note[i] == '.' && i > 0))) {
            return false;
        }
    }
    return digits > 0 && dots <= 1;
}

// The last " (" of a cached text that ends in ")" and whose bracket is the cache's note (an age, or the reason after an
// unread "?"): where its note starts. A value with brackets of its own and no note has none.
static char const *note_start(char const *cached, size_t len) {
    char const *cut = NULL;
    if (cached[len - 1] != ')') {
        return NULL;
    }
    for (char const *s = cached; (s = strstr(s, " (")) != NULL; s += 2) {
        cut = s;
    }
    if (!cut) {
        return NULL;
    }
    bool unread = cut == cached + 1 && cached[0] == '?';
    return unread || is_age(cut + 2, len - 1 - (size_t)(cut + 2 - cached)) ? cut : NULL;
}

// "<v> (3.2 s ago)", "<v> (stale, 4 s ago)", "<v>" or "? (<why>)" into its parts
static bool split(char const *cached, parts_t *p) {
    memset(p, 0, sizeof(*p));
    if (!cached || !*cached) {
        return false;
    }
    size_t      len = strlen(cached);
    char const *cut = note_start(cached, len);
    copy_n(p->value, sizeof(p->value), cached, cut ? (size_t)(cut - cached) : len);
    p->unread = strcmp(p->value, "?") == 0;
    if (!cut) {
        return true;
    }
    char const *note = cut + 2;
    size_t      n    = len - 1 - (size_t)(note - cached);
    if (!p->unread && strncmp(note, "stale, ", 7) == 0) {
        p->stale = true;
        note += 7;
        n -= 7;
    }
    if (!p->unread && n >= 4 && strncmp(note + n - 4, " ago", 4) == 0) {
        n -= 4;
    }
    copy_n(p->note, sizeof(p->note), note, n);
    return true;
}

// What follows the value: the stale age, else the saved value when it differs, else the age
static void tail_text(parts_t const *p, char const *saved, char const *unit, char *tail, size_t cap) {
    tail[0] = '\0';
    if (p->stale) {
        snprintf(tail, cap, " (stale %s)", p->note);
    } else if (saved && *saved && strcmp(saved, p->value) != 0) {
        snprintf(tail, cap, " (saved %s%s)", saved, unit ? unit : "");
    } else if (*p->note) {
        snprintf(tail, cap, " (%s)", p->note);
    }
}

size_t reg_row_text(char const *label, char const *cached, char const *saved, char const *unit, char *out, size_t cap) {
    if (!cap) {
        return 0;
    }
    parts_t p;
    char    head[24] = "", tail[40];
    if (label && *label) {
        snprintf(head, sizeof(head), "%s: ", label);
    }
    if (!split(cached, &p)) {
        snprintf(out, cap, "%s?", head);
    } else if (p.unread) { // the cache's own reason ("not read yet", "no answer from the C6") says why
        snprintf(out, cap, "%s?%s%s%s", head, *p.note ? " (" : "", p.note, *p.note ? ")" : "");
    } else {
        tail_text(&p, saved, unit, tail, sizeof(tail));
        snprintf(out, cap, "%s%s%s%s", head, p.value, unit ? unit : "", tail);
    }
    return strlen(out);
}

// ---- the multi-key lines (#333) -------------------------------------------------------------------------------------

typedef struct {
    char value[RR_LORA_KEYS][24]; // both lines have five keys; a value longer than this is cut
    char note[28];                // the first key's age, or the first unread key's reason
    bool stale;
    bool unread;
} fields_t;

static void gather(char const *const cached[], fields_t *f) {
    memset(f, 0, sizeof(*f));
    for (int i = 0; i < RR_LORA_KEYS; i++) {
        parts_t p;
        if (!split(cached[i], &p) || p.unread) {
            if (!f->unread) {
                snprintf(f->note, sizeof(f->note), "%s", cached[i] ? p.note : "");
            }
            f->unread = true;
            continue;
        }
        f->stale = f->stale || p.stale;
        snprintf(f->value[i], sizeof(f->value[i]), "%.23s", p.value);
        if (i == 0 && !f->unread) {
            snprintf(f->note, sizeof(f->note), "%s", p.note);
        }
    }
}

// The head ("<label>: ") into out; returns its length
static size_t head_out(char const *label, char *out, size_t cap) {
    if (!label || !*label) {
        out[0] = '\0';
        return 0;
    }
    size_t n = (size_t)snprintf(out, cap, "%s: ", label);
    return n < cap ? n : cap - 1;
}

// out holds "<head><line>" (n bytes): add the age or "(stale)", or replace it all when a key is unread. The line is
// built straight into out so the LVGL task's timers do not carry a second buffer for it
static size_t line_end(char const *label, fields_t const *f, char *out, size_t cap, size_t n) {
    if (f->unread) {
        n = head_out(label, out, cap);
        snprintf(out + n, cap - n, "?%s%s%s", *f->note ? " (" : "", f->note, *f->note ? ")" : "");
    } else if (f->stale) {
        snprintf(out + n, cap - n, " (stale)");
    } else if (*f->note) {
        snprintf(out + n, cap - n, " (%s)", f->note);
    }
    return strlen(out);
}

// 62500 Hz -> "62.5 kHz", 125000 -> "125 kHz"
static void khz_text(unsigned long hz, char *out, size_t cap) {
    if (hz % 1000) {
        snprintf(out, cap, "%lu.%lu kHz", hz / 1000, hz % 1000 / 100);
    } else {
        snprintf(out, cap, "%lu kHz", hz / 1000);
    }
}

size_t reg_lora_line(char const *label, char const *const cached[RR_LORA_KEYS], char *out, size_t cap) {
    fields_t f;
    char     bw[16];
    if (!cap) {
        return 0;
    }
    gather(cached, &f);
    // Round to the nearest kHz like MeshCore's "%.3f" (#346): the SX1262 applies 869.618 MHz as 869617981 Hz
    unsigned long khz = (strtoul(f.value[0], NULL, 10) + 500) / 1000;
    khz_text(strtoul(f.value[1], NULL, 10), bw, sizeof(bw));
    size_t n = head_out(label, out, cap);
    // The comma wording of mesh_format_radio, so Settings and MeshCore say it the same way (UI rules 4/9)
    n += (size_t)snprintf(out + n, cap - n, "%lu.%03lu MHz, %s, SF%s, CR%s, %s dBm", khz / 1000, khz % 1000, bw,
                          f.value[2], f.value[3], f.value[4]);
    return line_end(label, &f, out, cap, n < cap ? n : cap - 1);
}

// WIFI_PROTOCOL_11B/G/N/AX bits as letters ("bgn"); 0 or none of them: "-"
static void proto_text(unsigned long bits, char *out, size_t cap) {
    size_t n = 0;
    for (int i = 0; i < 4 && n + 3 < cap; i++) {
        static char const letter[4] = {'b', 'g', 'n', 0};
        static unsigned const mask[4] = {1, 2, 4, 0x40};
        if (bits & mask[i]) {
            n += (size_t)snprintf(out + n, cap - n, i == 3 ? "ax" : "%c", letter[i]);
        }
    }
    if (!n) {
        snprintf(out, cap, "-");
    }
}

// 80 quarter-dBm -> "20 dBm", 74 -> "18.5 dBm"
static void qdbm_text(long q, char *out, size_t cap) {
    long a = q < 0 ? -q : q;
    char const *sign = q < 0 ? "-" : "";
    if (a % 4) {
        snprintf(out, cap, "%s%ld.%02ld dBm", sign, a / 4, a % 4 * 25);
    } else {
        snprintf(out, cap, "%s%ld dBm", sign, a / 4);
    }
}

size_t reg_wifi_line(char const *label, char const *const cached[RR_WIFI_KEYS], char *out, size_t cap) {
    fields_t f;
    char     tx[24], proto[8];
    if (!cap) {
        return 0;
    }
    gather(cached, &f);
    qdbm_text(strtol(f.value[1], NULL, 10), tx, sizeof(tx));
    proto_text(strtoul(f.value[3], NULL, 10), proto, sizeof(proto));
    size_t n = head_out(label, out, cap);
    n += (size_t)snprintf(out + n, cap - n, "%s, %s, %s MHz, %s, ps %s", f.value[0], tx, f.value[2], proto, f.value[4]);
    return line_end(label, &f, out, cap, n < cap ? n : cap - 1);
}

size_t reg_mhz_text(char const *label, char const *cached_hz, char const *want_mhz, char *out, size_t cap) {
    parts_t p;
    if (!cap) {
        return 0;
    }
    if (!split(cached_hz, &p) || p.unread) {
        return reg_row_text(label, cached_hz, NULL, NULL, out, cap);
    }
    unsigned long hz = strtoul(p.value, NULL, 10);
    char          mhz[24], tail[40];
    if (hz % 1000000) {
        snprintf(mhz, sizeof(mhz), "%lu.%03lu", hz / 1000000, hz % 1000000 / 1000);
    } else {
        snprintf(mhz, sizeof(mhz), "%lu", hz / 1000000);
    }
    tail_text(&p, NULL, NULL, tail, sizeof(tail));
    if (!p.stale && want_mhz && *want_mhz && strcmp(want_mhz, mhz) != 0) { // "wants", not "saved": a trial is pending
        snprintf(tail, sizeof(tail), " (wants %s MHz)", want_mhz);
    }
    char head[24] = "";
    if (label && *label) {
        snprintf(head, sizeof(head), "%s: ", label);
    }
    snprintf(out, cap, "%s%s MHz%s", head, mhz, tail);
    return strlen(out);
}

size_t reg_row_value(char const *cached, char *out, size_t cap) {
    parts_t p;
    if (!cap) {
        return 0;
    }
    out[0] = '\0';
    if (split(cached, &p) && !p.unread) {
        snprintf(out, cap, "%s", p.value);
    }
    return strlen(out);
}
