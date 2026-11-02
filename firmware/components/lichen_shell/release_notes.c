#include "release_notes.h"

#include <ctype.h>
#include <string.h>

static bool digits(char const *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (!isdigit((unsigned char)s[i])) {
            return false;
        }
    }
    return true;
}

bool rn_parse_name(char const *name, uint32_t *seq, char *date) {
    if (!name) {
        return false;
    }
    size_t   i = 0;
    uint32_t v = 0;
    while (isdigit((unsigned char)name[i]) && i < 9) {
        v = v * 10 + (uint32_t)(name[i] - '0');
        i++;
    }
    // YYYY-MM-DD.md
    char const *d = name + i;
    if (i == 0 || d[0] != '-' || !digits(d + 1, 4) || d[5] != '-' || !digits(d + 6, 2) || d[8] != '-' ||
        !digits(d + 9, 2) || strcmp(d + 11, ".md") != 0) {
        return false;
    }
    if (seq) {
        *seq = v;
    }
    if (date) {
        memcpy(date, d + 1, RN_DATE_LEN - 1);
        date[RN_DATE_LEN - 1] = '\0';
    }
    return true;
}

size_t rn_build_index(rn_file_t const *files, size_t n, rn_note_t *out, size_t max) {
    size_t count = 0;
    for (size_t i = 0; i < n; i++) {
        rn_note_t note;
        if (!rn_parse_name(files[i].name, &note.seq, note.date) || !files[i].text) {
            continue;
        }
        note.text = files[i].text;
        size_t at = 0; // insertion sort, newest first; an equal sequence goes behind the first one and is dropped
        bool   dup = false;
        while (at < count && out[at].seq > note.seq) {
            at++;
        }
        if (at < count && out[at].seq == note.seq) {
            dup = true;
        }
        if (dup || (count == max && at >= max)) {
            continue;
        }
        size_t last = count < max ? count : max - 1; // the oldest falls off a full table
        for (size_t j = last; j > at; j--) {
            out[j] = out[j - 1];
        }
        out[at] = note;
        if (count < max) {
            count++;
        }
    }
    return count;
}

static size_t copy_line(char const *p, char *out, size_t cap) {
    size_t n = 0;
    while (p[n] && p[n] != '\n' && p[n] != '\r') {
        n++;
    }
    while (n && (p[n - 1] == ' ' || p[n - 1] == '\t')) {
        n--;
    }
    if (!cap) {
        return 0;
    }
    size_t w = n < cap - 1 ? n : cap - 1;
    memcpy(out, p, w);
    out[w] = '\0';
    return w;
}

size_t rn_title(char const *text, char *out, size_t cap) {
    char const *cursor = text;
    rn_block_t  b;
    do {
        rn_next_block(&cursor, &b);
        if (b.kind == RN_H1) {
            return copy_line(b.text, out, cap);
        }
    } while (b.kind != RN_END);
    return copy_line("Release notes", out, cap);
}

uint32_t rn_parse_seen(char const *value, bool *known) {
    uint32_t v = 0;
    size_t   i = 0;
    while (value && isdigit((unsigned char)value[i]) && i < 9) {
        v = v * 10 + (uint32_t)(value[i] - '0');
        i++;
    }
    *known = i > 0 && value[i] == '\0';
    return *known ? v : 0;
}

size_t rn_count_new(rn_note_t const *notes, size_t n, bool seen_known, uint32_t seen) {
    size_t count = 0;
    for (size_t i = 0; i < n; i++) {
        if (!seen_known || notes[i].seq > seen) {
            count++;
        }
    }
    return count;
}

rn_boot_t rn_boot_decision(bool updated, bool seen_known, uint32_t seen, uint32_t newest) {
    if (newest == 0 || (seen_known && seen >= newest)) {
        return RN_BOOT_NOTHING;
    }
    if (updated) {
        return RN_BOOT_SHOW;
    }
    return seen_known ? RN_BOOT_NOTHING : RN_BOOT_MARK;
}

// ---- the markdown subset ----

static bool blank(char const *line) {
    while (*line == ' ' || *line == '\t' || *line == '\r') {
        line++;
    }
    return *line == '\0' || *line == '\n';
}

static char const *next_line(char const *p) {
    while (*p && *p != '\n') {
        p++;
    }
    return *p ? p + 1 : p;
}

// Adds the line's words to the block, one space between lines
static void append_line(rn_block_t *b, char const *line) {
    while (*line == ' ' || *line == '\t') {
        line++;
    }
    size_t len = strlen(b->text);
    char   tmp[RN_TEXT_MAX];
    size_t n = copy_line(line, tmp, sizeof(tmp));
    if (n == 0 || len >= RN_TEXT_MAX - 1) {
        return;
    }
    if (len) {
        b->text[len++] = ' ';
    }
    size_t room = RN_TEXT_MAX - 1 - len;
    n           = n < room ? n : room;
    memcpy(b->text + len, tmp, n);
    b->text[len + n] = '\0';
}

static bool is_bullet(char const *line) {
    return (line[0] == '-' || line[0] == '*') && line[1] == ' ';
}

void rn_next_block(char const **cursor, rn_block_t *out) {
    char const *p = *cursor;
    out->text[0]  = '\0';
    while (*p && blank(p)) {
        p = next_line(p);
    }
    if (!*p) {
        out->kind = RN_END;
        *cursor   = p;
        return;
    }
    if (p[0] == '#') {
        int level = p[1] == '#' ? 2 : 1;
        char const *t = p + level;
        out->kind = level == 1 ? RN_H1 : RN_H2;
        if (*t == ' ') {
            append_line(out, t);
        } else {
            out->kind = RN_PARA; // "#tag" or "###": plain text
            append_line(out, p);
        }
        *cursor = next_line(p);
        return;
    }
    char const *line = p;
    if (is_bullet(p)) {
        out->kind = RN_BULLET;
        append_line(out, p + 2);
        p = next_line(p);
        // an indented line continues the bullet
        while (*p && (p[0] == ' ' || p[0] == '\t') && !blank(p)) {
            append_line(out, p);
            p = next_line(p);
        }
        *cursor = p;
        return;
    }
    out->kind = RN_PARA;
    while (*p && !blank(p) && p[0] != '#' && !is_bullet(p)) {
        append_line(out, p);
        p = next_line(p);
    }
    if (p == line) { // cannot happen: the first line is neither blank, heading nor bullet
        p = next_line(p);
    }
    *cursor = p;
}
