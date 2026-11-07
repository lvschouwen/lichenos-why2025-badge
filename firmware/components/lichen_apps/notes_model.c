#include "notes_model.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define SLUG_MAX 24

note_kind_t notes_parse(char const *line, char const **body) {
    note_kind_t kind = NOTE_PLAIN;
    *body            = line;
    if (line[0] == '[' && (line[1] == ' ' || line[1] == 'x' || line[1] == 'X') && line[2] == ']' && line[3] == ' ') {
        kind  = line[1] == ' ' ? NOTE_TODO : NOTE_DONE;
        *body = line + NOTES_PREFIX_LEN;
    }
    return kind;
}

bool notes_format(note_kind_t kind, char const *body, char *out, size_t cap) {
    char const *prefix = kind == NOTE_TODO ? "[ ] " : kind == NOTE_DONE ? "[x] " : "";
    if (cap) {
        out[0] = '\0';
    }
    if (strlen(body) > NOTES_TEXT_MAX || strlen(prefix) + strlen(body) >= cap) {
        return false;
    }
    snprintf(out, cap, "%s%s", prefix, body);
    return true;
}

note_kind_t notes_next_kind(note_kind_t kind) {
    return kind == NOTE_TODO ? NOTE_DONE : NOTE_TODO;
}

void notes_clean_line(char const *raw, size_t len, char *out, size_t cap) {
    if (!cap) {
        return;
    }
    size_t at  = 0;
    bool   gap = false;
    for (size_t i = 0; i < len && raw[i] != '\n' && raw[i] != '\r' && raw[i] != '\0'; i++) {
        unsigned char c = (unsigned char)raw[i];
        if (c < 0x20 || c == 0x7F || c == ' ') {
            gap = at > 0;
            continue;
        }
        if (at + (gap ? 2 : 1) >= cap) {
            // Cut here; when this byte continues a character, drop the part that is already copied
            if ((c & 0xC0) == 0x80) {
                while (at > 0 && ((unsigned char)out[at - 1] & 0xC0) == 0x80) {
                    at--;
                }
                if (at > 0) {
                    at--; // the lead byte
                }
            }
            break;
        }
        if (gap) {
            out[at++] = ' ';
        }
        gap       = false;
        out[at++] = (char)c;
    }
    out[at] = '\0';
}

static size_t slug(char const *body, char *out) {
    size_t at  = 0;
    bool   dash = false;
    for (; *body && at < SLUG_MAX; body++) {
        unsigned char c = (unsigned char)*body;
        if (isascii(c) && isalnum(c)) {
            if (dash && at > 0 && at + 1 < SLUG_MAX) {
                out[at++] = '-';
            }
            dash = false;
            out[at++] = (char)tolower(c);
        } else {
            dash = true;
        }
    }
    if (at == 0) {
        memcpy(out, "note", 4);
        at = 4;
    }
    out[at] = '\0';
    return at;
}

bool notes_file_name(char const *body, unsigned n, char *out, size_t cap) {
    char s[SLUG_MAX + 1];
    slug(body, s);
    int len = n ? snprintf(out, cap, "%s-%u.txt", s, n) : snprintf(out, cap, "%s.txt", s);
    if (len < 0 || (size_t)len >= cap) {
        if (cap) {
            out[0] = '\0';
        }
        return false;
    }
    return true;
}

bool notes_name_ok(char const *name) {
    size_t n = name ? strlen(name) : 0;
    if (n < 5 || n >= NOTES_NAME_MAX || name[0] == '.' || strchr(name, '/') || strcmp(name + n - 4, ".txt") != 0) {
        return false;
    }
    return true;
}

int notes_name_compare(char const *a, char const *b) {
    char const *x = a, *y = b;
    for (; *x && *y; x++, y++) {
        int d = tolower((unsigned char)*x) - tolower((unsigned char)*y);
        if (d) {
            return d;
        }
    }
    int d = tolower((unsigned char)*x) - tolower((unsigned char)*y);
    return d ? d : strcmp(a, b);
}
