// "fw sd cat" arguments and output lines (#378): see sd_cat_fmt.h
#include "sd_cat_fmt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The next space-separated word of *s: its start and length (0 at the end)
static size_t next_word(char const **s, char const **word) {
    while (**s == ' ') {
        (*s)++;
    }
    *word = *s;
    while (**s && **s != ' ') {
        (*s)++;
    }
    return (size_t)(*s - *word);
}

static bool parse_u32(char const *w, size_t n, uint32_t *out) {
    if (n == 0 || n > 10) {
        return false;
    }
    unsigned long long v = 0;
    for (size_t i = 0; i < n; i++) {
        if (w[i] < '0' || w[i] > '9') {
            return false;
        }
        v = v * 10u + (unsigned)(w[i] - '0');
    }
    if (v > UINT32_MAX) {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

// The n-th plain word (the path, the offset, the length) goes into out
static bool take_word(sd_cat_args_t *out, int index, char const *w, size_t n) {
    switch (index) {
        case 0:
            if (n > sizeof(out->path) - 1) {
                n = sizeof(out->path) - 1; // still over SD_PATH_MAX: sd_path_check refuses it as too long
            }
            memcpy(out->path, w, n);
            out->path[n] = '\0';
            return true;
        case 1: return parse_u32(w, n, &out->offset);
        case 2: return parse_u32(w, n, &out->len) && out->len >= 1 && out->len <= SD_CAT_MAX;
        default: return false;
    }
}

bool sd_cat_parse(char const *args, sd_cat_args_t *out) {
    *out = (sd_cat_args_t){.len = SD_CAT_DEFAULT};
    int         words = 0;
    char const *w;
    for (size_t n; args && (n = next_word(&args, &w)) > 0;) {
        if (n == 2 && !memcmp(w, "-x", 2)) {
            out->hex = true;
        } else if (!take_word(out, words++, w, n)) {
            return false;
        }
    }
    return words >= 1;
}

static char shown(uint8_t c) {
    return c >= 0x20 && c < 0x7f ? (char)c : '.';
}

size_t sd_cat_hex_line(uint32_t off, uint8_t const *b, size_t n, char *out, size_t cap) {
    if (cap < SD_CAT_LINE_CAP || n > SD_CAT_HEX_ROW) {
        if (cap) {
            out[0] = '\0';
        }
        return 0;
    }
    size_t len = (size_t)snprintf(out, cap, "%08lx ", (unsigned long)off);
    for (size_t i = 0; i < SD_CAT_HEX_ROW; i++) {
        len += i < n ? (size_t)snprintf(out + len, cap - len, " %02x", b[i]) : (size_t)snprintf(out + len, cap - len, "   ");
    }
    len += (size_t)snprintf(out + len, cap - len, "  |");
    for (size_t i = 0; i < n; i++) {
        out[len++] = shown(b[i]);
    }
    out[len++] = '|';
    out[len]   = '\0';
    return len;
}

size_t sd_cat_text_line(uint8_t const *b, size_t n, size_t *consumed, char *out, size_t cap) {
    size_t len = 0, i = 0;
    for (; i < n && len < SD_CAT_TEXT_COL && len + 1 < cap; i++) {
        if (b[i] == '\n') {
            i++;
            break;
        }
        if (b[i] != '\r') {
            out[len++] = b[i] == '\t' ? ' ' : shown(b[i]);
        }
    }
    if (cap) {
        out[len] = '\0';
    }
    *consumed = i;
    return len;
}
