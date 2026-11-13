// The console's on-air allow list (#289): mesh_onair.h. Pure.
#include "mesh_onair.h"

#include <stdio.h>
#include <string.h>

static int hex_val(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    c = (char)(c | 0x20);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

static bool all_hex(char const *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (hex_val(s[i]) < 0) {
            return false;
        }
    }
    return true;
}

// A channel name as the badge compares it (channel_same_name: ASCII case)
static bool same_name(char const *a, char const *b) {
    for (;; a++, b++) {
        char ca = *a >= 'A' && *a <= 'Z' ? (char)(*a | 0x20) : *a;
        char cb = *b >= 'A' && *b <= 'Z' ? (char)(*b | 0x20) : *b;
        if (ca != cb) {
            return false;
        }
        if (!ca) {
            return true;
        }
    }
}

// 1 to ONAIR_NAME_MAX bytes, no control character, no space at either end
static bool channel_ok(char const *s, size_t len) {
    if (len == 0 || len > ONAIR_NAME_MAX || s[0] == ' ' || s[len - 1] == ' ') {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == 0x7f) {
            return false;
        }
    }
    return true;
}

static void set_channel(onair_entry_t *out, char const *s, size_t len) {
    memset(out, 0, sizeof(*out));
    out->kind = ONAIR_CHANNEL;
    memcpy(out->name, s, len);
}

static void set_contact(onair_entry_t *out, char const *hex) {
    memset(out, 0, sizeof(*out));
    out->kind = ONAIR_CONTACT;
    for (size_t i = 0; i < ONAIR_PREFIX; i++) {
        out->prefix[i] = (uint8_t)(hex_val(hex[2 * i]) << 4 | hex_val(hex[2 * i + 1]));
    }
}

onair_result_t onair_entry_parse(char const *text, onair_entry_t *out) {
    if (!text) {
        return ONAIR_BAD;
    }
    while (*text == ' ') {
        text++;
    }
    size_t len = strlen(text);
    while (len && text[len - 1] == ' ') {
        len--;
    }
    if (len && all_hex(text, len)) { // key digits, also when mistyped: never silently a channel of that name
        if (len < ONAIR_HEX_MIN || len > ONAIR_HEX_MAX || len % 2) {
            return ONAIR_BAD;
        }
        set_contact(out, text);
        return ONAIR_OK;
    }
    if (!channel_ok(text, len)) {
        return ONAIR_BAD;
    }
    set_channel(out, text, len);
    return ONAIR_OK;
}

// The same target: a channel by name (the key compared by the caller where it matters), a contact by prefix
static bool same_target(onair_entry_t const *e, onair_tx_t tx, char const *channel, uint8_t const *prefix) {
    if (tx == ONAIR_TX_CHANNEL) {
        return e->kind == ONAIR_CHANNEL && channel && channel[0] && same_name(e->name, channel);
    }
    return tx == ONAIR_TX_CONTACT && e->kind == ONAIR_CONTACT && prefix &&
           memcmp(e->prefix, prefix, ONAIR_PREFIX) == 0;
}

// The index of the entry for e's target, or -1
static int find(onair_list_t const *list, onair_entry_t const *e) {
    onair_tx_t tx = e->kind == ONAIR_CHANNEL ? ONAIR_TX_CHANNEL : ONAIR_TX_CONTACT;
    for (size_t i = 0; i < list->n && i < ONAIR_MAX; i++) {
        if (same_target(&list->e[i], tx, e->name, e->prefix)) {
            return (int)i;
        }
    }
    return -1;
}

onair_result_t onair_add(onair_list_t *list, onair_entry_t const *e) {
    int at = find(list, e);
    if (at >= 0) {
        onair_entry_t *have = &list->e[at];
        if (e->kind != ONAIR_CHANNEL || memcmp(have->key_id, e->key_id, ONAIR_KEY_ID) == 0) {
            return ONAIR_DUPLICATE;
        }
        *have = *e; // the channel of that name now has another key: only the new one is listed
        return ONAIR_REKEYED;
    }
    if (list->n >= ONAIR_MAX) {
        return ONAIR_FULL;
    }
    list->e[list->n++] = *e;
    return ONAIR_OK;
}

onair_result_t onair_remove(onair_list_t *list, char const *text) {
    onair_entry_t  e;
    onair_result_t r = onair_entry_parse(text, &e);
    if (r != ONAIR_OK) {
        return r;
    }
    int at = find(list, &e);
    if (at < 0) {
        return ONAIR_NOT_FOUND;
    }
    memmove(&list->e[at], &list->e[at + 1], (list->n - (size_t)at - 1) * sizeof(list->e[0]));
    list->n--;
    memset(&list->e[list->n], 0, sizeof(list->e[0]));
    return ONAIR_OK;
}

bool mesh_onair_allows(onair_list_t const *list, bool on, bool from_console, onair_tx_t tx, char const *channel,
                       uint8_t const *key_id, uint8_t const *prefix) {
    if (!from_console || !on || tx == ONAIR_TX_ADVERT) {
        return true;
    }
    for (size_t i = 0; list && i < list->n && i < ONAIR_MAX; i++) {
        onair_entry_t const *e = &list->e[i];
        if (same_target(e, tx, channel, prefix) &&
            (tx != ONAIR_TX_CHANNEL || (key_id && memcmp(e->key_id, key_id, ONAIR_KEY_ID) == 0))) {
            return true;
        }
    }
    return false;
}

void onair_refused_text(onair_tx_t tx, char const *channel, uint8_t const *prefix, char *out, size_t cap) {
    char target[ONAIR_NAME_MAX + 1] = "?";
    if (tx == ONAIR_TX_CHANNEL && channel && channel[0]) {
        snprintf(target, sizeof(target), "%s", channel);
    } else if (tx == ONAIR_TX_CONTACT && prefix) {
        snprintf(target, sizeof(target), "%02x%02x%02x%02x%02x%02x", prefix[0], prefix[1], prefix[2], prefix[3],
                 prefix[4], prefix[5]);
    }
    snprintf(out, cap, "on air refused: %s is not on the console allow list", target);
}

// The word at s ("on", "allow", ...) alone or followed by a space; *rest what follows the space
static bool word_is(char const *s, char const *word, char const **rest) {
    size_t len = strlen(word);
    if (strncmp(s, word, len) != 0 || (s[len] != '\0' && s[len] != ' ')) {
        return false;
    }
    *rest = s[len] ? s + len + 1 : s + len;
    return true;
}

onair_op_t onair_op_parse(char const *args, char const **rest) {
    static struct {
        char const *word;
        onair_op_t  op;
        bool        target;
    } const OPS[] = {
        {"on", ONAIR_OP_ON, false},       {"off", ONAIR_OP_OFF, false},          {"clear", ONAIR_OP_CLEAR, false},
        {"allow", ONAIR_OP_ALLOW, true},  {"remove", ONAIR_OP_REMOVE, true},
    };
    char const *after = "";
    *rest             = "";
    if (!args || !*args) {
        return ONAIR_OP_STATUS;
    }
    for (size_t i = 0; i < sizeof(OPS) / sizeof(OPS[0]); i++) {
        if (word_is(args, OPS[i].word, &after)) {
            // an op without a target takes nothing after it; one with a target needs it
            if (OPS[i].target != (*after != '\0')) {
                return ONAIR_OP_USAGE;
            }
            *rest = after;
            return OPS[i].op;
        }
    }
    return ONAIR_OP_USAGE;
}

bool onair_op_permitted(onair_op_t op, console_origin_t origin) {
    switch (op) {
    case ONAIR_OP_STATUS:
    case ONAIR_OP_ON:
    case ONAIR_OP_USAGE:
        return true;
    case ONAIR_OP_OFF:
    case ONAIR_OP_ALLOW:
    case ONAIR_OP_REMOVE:
    case ONAIR_OP_CLEAR:
        return origin == CONSOLE_ORIGIN_UART;
    }
    return false;
}

void onair_entry_text(onair_entry_t const *e, char *out, size_t cap) {
    if (e->kind == ONAIR_CHANNEL) {
        snprintf(out, cap, "channel %s", e->name);
    } else {
        snprintf(out, cap, "contact %02x%02x%02x%02x%02x%02x", e->prefix[0], e->prefix[1], e->prefix[2],
                 e->prefix[3], e->prefix[4], e->prefix[5]);
    }
}

static void hex_out(uint8_t const *b, size_t n, char *out) {
    static char const DIGITS[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i]     = DIGITS[b[i] >> 4];
        out[2 * i + 1] = DIGITS[b[i] & 15];
    }
    out[2 * n] = '\0';
}

// "\nc <key id> <name>" or "\nk <prefix>": snprintf's count
static int entry_line(onair_entry_t const *e, char *out, size_t cap) {
    char hex[2 * ONAIR_KEY_ID + 1];
    if (e->kind == ONAIR_CHANNEL) {
        hex_out(e->key_id, ONAIR_KEY_ID, hex);
        return snprintf(out, cap, "\nc %s %s", hex, e->name);
    }
    hex_out(e->prefix, ONAIR_PREFIX, hex);
    return snprintf(out, cap, "\nk %s", hex);
}

bool onair_encode(onair_list_t const *list, char *out, size_t cap) {
    if (cap == 0) {
        return false;
    }
    out[0] = '\0';
    if (list->n == 0) {
        return true; // erased, not saved
    }
    int w = snprintf(out, cap, "%s", ONAIR_TEXT_VER);
    if (w < 0 || (size_t)w >= cap) {
        out[0] = '\0';
        return false;
    }
    for (size_t i = 0, used = (size_t)w; i < list->n && i < ONAIR_MAX; i++, used += (size_t)w) {
        w = entry_line(&list->e[i], out + used, cap - used);
        if (w < 0 || (size_t)w >= cap - used) {
            out[0] = '\0';
            return false;
        }
    }
    return true;
}

// One saved line (len bytes at s, no '\n'): "c <name>" or "k <12 hex>", exactly as onair_encode writes them
static bool decode_line(char const *s, size_t len, onair_entry_t *e) {
    if (len < 3 || s[1] != ' ') {
        return false;
    }
    size_t id_hex = 2 * ONAIR_KEY_ID;
    if (s[0] == 'c' && len > 3 + id_hex && all_hex(s + 2, id_hex) && s[2 + id_hex] == ' ' &&
        channel_ok(s + 3 + id_hex, len - 3 - id_hex)) {
        set_channel(e, s + 3 + id_hex, len - 3 - id_hex);
        for (size_t i = 0; i < ONAIR_KEY_ID; i++) {
            e->key_id[i] = (uint8_t)(hex_val(s[2 + 2 * i]) << 4 | hex_val(s[3 + 2 * i]));
        }
        return true;
    }
    if (s[0] == 'k' && len - 2 == 2 * ONAIR_PREFIX && all_hex(s + 2, len - 2)) {
        set_contact(e, s + 2);
        return true;
    }
    return false;
}

bool onair_decode(char const *text, onair_list_t *out) {
    memset(out, 0, sizeof(*out));
    if (!text || !*text) {
        return true;
    }
    size_t ver = strlen(ONAIR_TEXT_VER);
    if (strlen(text) >= ONAIR_TEXT_MAX || strncmp(text, ONAIR_TEXT_VER, ver) != 0 ||
        (text[ver] != '\n' && text[ver] != '\0')) {
        return false; // an unversioned or other version's text: none (fails closed)
    }
    if (!text[ver]) {
        return true;
    }
    for (char const *s = text + ver + 1;;) {
        char const   *end = strchr(s, '\n');
        size_t        len = end ? (size_t)(end - s) : strlen(s);
        onair_entry_t e;
        if (out->n >= ONAIR_MAX || !decode_line(s, len, &e) || find(out, &e) >= 0) {
            memset(out, 0, sizeof(*out)); // damaged, too many or repeated: none, never a guess (fails closed)
            return false;
        }
        out->e[out->n++] = e;
        if (!end) {
            return true;
        }
        s = end + 1;
    }
}
