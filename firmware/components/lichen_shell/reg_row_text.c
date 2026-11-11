#include "reg_row_text.h"

#include <stdbool.h>
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

// The last " (" of a cached text that ends in ")": where its note starts
static char const *note_start(char const *cached, size_t len) {
    char const *cut = NULL;
    if (cached[len - 1] != ')') {
        return NULL;
    }
    for (char const *s = cached; (s = strstr(s, " (")) != NULL; s += 2) {
        cut = s;
    }
    return cut;
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
