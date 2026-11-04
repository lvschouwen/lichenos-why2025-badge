// The Console app's pure logic (#150): see console_model.h
#include "console_model.h"

#include <ctype.h>
#include <stdio.h>
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

typedef struct {
    char const *cmd;
    char const *subs; // space-separated subcommands allowed after it ("-" stands for none); NULL: any arguments
} allow_t;

// Built from the console_register calls: only commands that take no secret, and only the subcommands that take none.
// Left out on purpose: wifi <ssid> <password>, channel, mesh send/dm/name/remote..., nvs-enc, token, keys (types into
// the focused field), factory-reset, reboot and the like (not secret, but not worth a recall from history).
static allow_t const allowed[] = {
    {"help", NULL},   {"mem", NULL},     {"assets", NULL}, {"net", NULL},      {"uicheck", NULL}, {"fonts", NULL},
    {"bench", NULL},  {"boot", NULL},    {"ui", NULL},     {"idle", NULL},     {"thermal", NULL}, {"i2c", NULL},
    {"sensors", NULL}, {"psram", NULL},  {"led", NULL},    {"vib", NULL},      {"screensaver", NULL}, {"bl", NULL},
    {"whatsnew", NULL}, {"home", NULL},  {"airplane", NULL}, {"msgs", NULL},   {"channels", NULL}, {"advert", NULL},
    {"contacts", "-"}, {"tasks", NULL},
    {"mesh", "- link out chats hist"},
    {"sd", "- status mount unmount test"},
    {"crash", "- dump"},
    {"ota", "- on off auto"},
    {"lora", "- on off log raw"},
    {"clock", "- sync"},
};

static bool word_in(char const *list, char const *word) {
    size_t w = strlen(word);
    for (char const *p = list; *p;) {
        size_t n = strcspn(p, " ");
        if (n == w && strncmp(p, word, n) == 0) {
            return true;
        }
        p += n + (p[n] ? 1 : 0);
    }
    return false;
}

static char const *skip_fw(char const *line) {
    while (*line == ' ') {
        line++;
    }
    if (strncmp(line, "fw ", 3) == 0) {
        line += 3;
        while (*line == ' ') {
            line++;
        }
    }
    return line;
}

void console_first_word(char const *line, char *out, size_t n) {
    line     = skip_fw(line);
    size_t k = strcspn(line, " ");
    if (k >= n) {
        k = n - 1;
    }
    memcpy(out, line, k);
    out[k] = '\0';
}

static bool has_word(char const *line, char const *word) {
    size_t w = strlen(word);
    for (char const *p = line; *p;) {
        p += strspn(p, " ");
        size_t n = strcspn(p, " ");
        if (n == w && strncmp(p, word, n) == 0) {
            return true;
        }
        p += n;
    }
    return false;
}

// cmd_wifi (c6link.c) matches its subcommands as whole strings and reads ANY other line as "<ssid> <password>":
// `wifi on hunter2` joins a network called "on". So only these exact argument strings count as public.
static bool wifi_public(char const *rest) {
    static char const *const exact[] = {"",      "on",      "off",    "scan",  "reconnect", "stats", "ps",
                                        "ps none", "ps min", "bw",    "bw 20", "bw 40",     "proto", "proto n",
                                        "proto ax", "tx"};
    for (size_t i = 0; i < sizeof(exact) / sizeof(exact[0]); i++) {
        if (strcmp(rest, exact[i]) == 0) {
            return true;
        }
    }
    if (strncmp(rest, "tx ", 3) != 0 || !rest[3]) { // tx <level>: digits only
        return false;
    }
    for (char const *p = rest + 3; *p; p++) {
        if (!isdigit((unsigned char)*p)) {
            return false;
        }
    }
    return strlen(rest + 3) <= 3;
}

bool console_line_is_public(char const *line) {
    if (!line || console_line_is_secret(line)) {
        return false;
    }
    line = skip_fw(line);
    if (has_word(line, "YES")) {
        return false;
    }
    char cmd[24], sub[24];
    console_first_word(line, cmd, sizeof(cmd));
    char const *rest = line + strcspn(line, " ");
    rest += strspn(rest, " ");
    size_t k = strcspn(rest, " ");
    snprintf(sub, sizeof(sub), "%.*s", (int)(k < sizeof(sub) - 1 ? k : sizeof(sub) - 1), rest);
    if (strlen(cmd) >= sizeof(cmd) - 1) {
        return false;
    }
    if (strcmp(cmd, "wifi") == 0) {
        return wifi_public(rest);
    }
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
        if (strcmp(allowed[i].cmd, cmd) != 0) {
            continue;
        }
        return !allowed[i].subs || word_in(allowed[i].subs, sub[0] ? sub : "-");
    }
    return false;
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
    if (!line || !line[0] || !console_line_is_public(line) || strlen(line) > CONSOLE_LINE_MAX) {
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
