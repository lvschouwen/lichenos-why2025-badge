// The Console app's pure logic (#150): see console_model.h
#include "console_model.h"

#include <ctype.h>
#include <string.h>

static bool contains_nocase(char const *s, char const *word) {
    size_t w = strlen(word);
    for (; *s; s++) {
        size_t i = 0;
        while (i < w && s[i] && tolower((unsigned char)s[i]) == word[i]) {
            i++;
        }
        if (i == w) {
            return true;
        }
    }
    return false;
}

bool console_line_is_secret(char const *line) {
    return line && (contains_nocase(line, "password") || contains_nocase(line, "login"));
}

void console_hist_init(console_hist_t *h) {
    memset(h, 0, sizeof(*h));
    h->pos = -1;
}

static char *slot(console_hist_t *h, int back) { // back 0 = newest
    return h->line[(h->start + h->count - 1 - back) % CONSOLE_HIST_N];
}

bool console_hist_push(console_hist_t *h, char const *line) {
    h->pos = -1;
    if (!line || !line[0] || console_line_is_secret(line) || strlen(line) > CONSOLE_LINE_MAX) {
        return false;
    }
    if (h->count && strcmp(slot(h, 0), line) == 0) {
        return false;
    }
    if (h->count == CONSOLE_HIST_N) {
        h->start = (h->start + 1) % CONSOLE_HIST_N;
        h->count--;
    }
    h->count++;
    strcpy(slot(h, 0), line);
    return true;
}

char const *console_hist_older(console_hist_t *h) {
    if (h->count == 0) {
        return NULL;
    }
    if (h->pos < h->count - 1) {
        h->pos++;
    }
    return slot(h, h->pos);
}

char const *console_hist_newer(console_hist_t *h) {
    if (h->pos < 0) {
        return NULL;
    }
    if (h->pos == 0) {
        h->pos = -1;
        return "";
    }
    h->pos--;
    return slot(h, h->pos);
}

bool console_hist_browsing(console_hist_t const *h) {
    return h->pos >= 0;
}

void console_hist_end_browse(console_hist_t *h) {
    h->pos = -1;
}

// Moves past the bytes of a UTF-8 character cut in the middle
static size_t skip_continuation(char const *s, size_t i, size_t len) {
    while (i < len && ((unsigned char)s[i] & 0xC0) == 0x80) {
        i++;
    }
    return i;
}

// How many bytes to drop from the front of buf[0..len) so that `need` bytes are freed, ending on a line start
static size_t drop_for(char const *buf, size_t len, size_t need) {
    if (need >= len) {
        return len;
    }
    char const *nl = memchr(buf + need - (need ? 1 : 0), '\n', len - need + (need ? 1 : 0));
    return nl ? (size_t)(nl - buf) + 1 : len;
}

size_t console_out_append(char *buf, size_t cap, size_t len, char const *text, size_t n) {
    if (cap < 2) {
        return 0;
    }
    if (n >= cap) { // only the newest part fits: start at a line start inside it when there is one
        size_t from = n - (cap - 1);
        char const *nl = memchr(text + from, '\n', n - from);
        from = nl ? (size_t)(nl - text) + 1 : skip_continuation(text, from, n);
        text += from;
        n    -= from;
        len   = 0;
    } else if (len + n + 1 > cap) {
        size_t drop = drop_for(buf, len, len + n + 1 - cap);
        memmove(buf, buf + drop, len - drop);
        len -= drop;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)text[i];
        buf[len++]      = (c < 0x20 && c != '\n') ? ' ' : c == 0x7f ? '?' : (char)c;
    }
    buf[len] = '\0';
    return len;
}
