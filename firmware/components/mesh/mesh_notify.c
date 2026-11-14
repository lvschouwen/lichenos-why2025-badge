// What MeshCore tells the notification service (#50): see mesh_notify.h
#include "mesh_notify.h"

#include <ctype.h>
#include <stdio.h>
#include <strings.h>
#include <string.h>

#define CHAN_TARGET_NAME MN_TARGET_NAME_MAX // MN_TARGET_MAX - 1 - strlen("ch:")

mn_mode_t mn_default_mode(mesh_chat_key_t const *key) {
    if (key->kind == MESH_CHAT_CHANNEL && strcmp(key->chan, "public") == 0) {
        return MN_MODE_MENTIONS;
    }
    return MN_MODE_ALL;
}

// A channel line: the mode decides, Mentions also needs the mentions switch
static mn_out_t decide_channel(mn_mode_t mode, mn_switches_t const *sw, bool mention) {
    if (mode == MN_MODE_OFF) {
        return (mn_out_t){MN_SKIP, MN_R_MODE_OFF};
    }
    if (mode == MN_MODE_MENTIONS) {
        if (!mention) {
            return (mn_out_t){MN_SKIP, MN_R_NOT_MENTION};
        }
        if (!sw->mentions) {
            return (mn_out_t){MN_SKIP, MN_R_MENTIONS_OFF};
        }
    }
    return (mn_out_t){MN_POST, MN_R_OK};
}

// Everything but do-not-disturb; that only softens a POST
static mn_out_t decide_switches(mn_kind_t kind, mn_mode_t mode, mn_switches_t const *sw, bool chat_open, bool mention) {
    if (chat_open) {
        return (mn_out_t){MN_SKIP, MN_R_CHAT_OPEN};
    }
    if (kind == MN_DM) {
        return sw->dm ? (mn_out_t){MN_POST, MN_R_OK} : (mn_out_t){MN_SKIP, MN_R_DM_OFF};
    }
    if (kind == MN_FAILED) {
        return sw->fail ? (mn_out_t){MN_POST, MN_R_OK} : (mn_out_t){MN_SKIP, MN_R_FAIL_OFF};
    }
    return decide_channel(mode, sw, mention);
}

mn_out_t mn_decide(mn_kind_t kind, mn_mode_t mode, mn_switches_t const *sw, bool chat_open, bool mention, bool dnd) {
    mn_out_t out = decide_switches(kind, mode, sw, chat_open, mention);
    if (out.verdict == MN_POST && dnd) {
        out = (mn_out_t){MN_SILENT, MN_R_DND};
    }
    return out;
}

char const *mn_reason_text(mn_reason_t reason) {
    static char const *const TEXT[MN_R_COUNT] = {"ok",           "chat open",       "DMs off",      "delivery failed off",
                                                 "channel off", "not a mention",   "mentions off", "do not disturb",
                                                 "source off"};
    return reason < MN_R_COUNT ? TEXT[reason] : "?";
}

// ---- mentions ------------------------------------------------------------------------------------------------------

static int lower(unsigned char c) {
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

static bool word_char(unsigned char c) {
    return isalnum(c) || c == '_' || c >= 0x80; // a UTF-8 byte continues a word
}

// Does text start with name (ASCII case-insensitive)? *len is the name's length when it does
static bool starts_with_name(char const *text, char const *name, size_t name_len) {
    for (size_t i = 0; i < name_len; i++) {
        if (!text[i] || lower((unsigned char)text[i]) != lower((unsigned char)name[i])) {
            return false;
        }
    }
    return true;
}

bool mn_is_mention(char const *text, char const *my_name) {
    while (*my_name == ' ') {
        my_name++;
    }
    size_t len = strlen(my_name);
    while (len && my_name[len - 1] == ' ') {
        len--;
    }
    if (!len || !text) {
        return false;
    }
    char const *sep  = strstr(text, ": ");
    char const *body = sep ? sep + 2 : text;
    for (char const *p = strchr(body, '@'); p; p = strchr(p + 1, '@')) {
        char const *name = p + 1;
        if (*name == '[') {
            if (starts_with_name(name + 1, my_name, len) && name[1 + len] == ']') {
                return true;
            }
        } else if (starts_with_name(name, my_name, len) && !word_char((unsigned char)name[len])) {
            return true;
        }
    }
    return false;
}

// ---- targets -------------------------------------------------------------------------------------------------------

size_t mn_target(mn_kind_t kind, mesh_chat_key_t const *key, char *out, size_t cap) {
    char const *prefix = kind == MN_FAILED ? (key->kind == MESH_CHAT_DM ? "fd:" : "fc:")
                                           : (key->kind == MESH_CHAT_DM ? "dm:" : "ch:");
    int n;
    if (key->kind == MESH_CHAT_DM) {
        n = snprintf(out, cap, "%s%02x%02x%02x%02x%02x%02x", prefix, key->id[0], key->id[1], key->id[2], key->id[3],
                     key->id[4], key->id[5]);
    } else {
        n = snprintf(out, cap, "%s%.*s", prefix, CHAN_TARGET_NAME, key->chan);
    }
    return n < 0 ? 0 : (size_t)n < cap ? (size_t)n : cap - 1;
}

static bool parse_prefix(char const *hex, uint8_t id[6]) {
    unsigned b[6];
    if (strspn(hex, "0123456789abcdef") != 12 || hex[12] ||
        sscanf(hex, "%2x%2x%2x%2x%2x%2x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        id[i] = (uint8_t)b[i];
    }
    return true;
}

bool mn_target_parse(char const *target, mesh_chat_key_t *key, bool *failed) {
    memset(key, 0, sizeof(*key));
    if (strlen(target) < 3 || target[2] != ':') {
        return false;
    }
    bool fail = target[0] == 'f' && (target[1] == 'c' || target[1] == 'd');
    bool dm   = fail ? target[1] == 'd' : target[0] == 'd' && target[1] == 'm';
    bool ch   = fail ? target[1] == 'c' : target[0] == 'c' && target[1] == 'h';
    if (!dm && !ch) {
        return false;
    }
    if (dm) {
        key->kind = MESH_CHAT_DM;
        if (!parse_prefix(target + 3, key->id)) {
            return false;
        }
    } else {
        key->kind = MESH_CHAT_CHANNEL;
        snprintf(key->chan, sizeof(key->chan), "%s", target + 3);
    }
    *failed = fail;
    return true;
}

int mn_channel_match(char const names[][MESH_CHANNEL_MAX + 1], size_t n, char const *chan) {
    size_t len = strlen(chan);
    for (size_t i = 0; i < n; i++) {
        if (strlen(names[i]) == len && strncasecmp(names[i], chan, len) == 0) {
            return (int)i;
        }
    }
    if (len < MN_TARGET_NAME_MAX) {
        return -1; // not cut, so no longer name can be meant
    }
    int found = -1;
    for (size_t i = 0; i < n; i++) {
        if (strlen(names[i]) > len && strncasecmp(names[i], chan, len) == 0) {
            if (found >= 0) {
                return -1; // ambiguous
            }
            found = (int)i;
        }
    }
    return found;
}

// ---- stored modes --------------------------------------------------------------------------------------------------

uint32_t mn_key_hash(mesh_chat_key_t const *key) {
    uint32_t h = 2166136261u;
    h          = (h ^ key->kind) * 16777619u;
    if (key->kind == MESH_CHAT_DM) {
        for (int i = 0; i < 6; i++) {
            h = (h ^ key->id[i]) * 16777619u;
        }
    } else {
        for (char const *c = key->chan; *c; c++) {
            h = (h ^ (unsigned char)*c) * 16777619u;
        }
    }
    return h;
}

void mn_nvs_key(mesh_chat_key_t const *key, char out[10]) {
    snprintf(out, 10, "m%08x", (unsigned)mn_key_hash(key));
}

bool mn_cache_lookup(mn_cache_t const *c, mesh_chat_key_t const *key, mn_mode_t *mode) {
    uint32_t h = mn_key_hash(key);
    for (uint8_t i = 0; i < c->n; i++) {
        if (c->hash[i] == h) {
            *mode = (mn_mode_t)c->mode[i];
            return true;
        }
    }
    return false;
}

void mn_cache_store(mn_cache_t *c, mesh_chat_key_t const *key, mn_mode_t mode) {
    uint32_t h = mn_key_hash(key);
    for (uint8_t i = 0; i < c->n; i++) {
        if (c->hash[i] == h) {
            c->mode[i] = (uint8_t)mode;
            return;
        }
    }
    uint8_t slot;
    if (c->n < MN_CACHE_MAX) {
        slot = c->n++;
    } else {
        slot    = c->next;
        c->next = (uint8_t)((c->next + 1) % MN_CACHE_MAX);
    }
    c->hash[slot] = h;
    c->mode[slot] = (uint8_t)mode;
}

bool mn_counts_unread(mesh_chat_key_t const *key, mn_mode_t mode) {
    return key->kind == MESH_CHAT_DM || mode == MN_MODE_ALL;
}

// ---- notes ---------------------------------------------------------------------------------------------------------

static bool same_chat(mn_note_t const *n, mn_kind_t kind, mesh_chat_key_t const *key) {
    return n->kind == kind && n->key.kind == key->kind &&
           (key->kind == MESH_CHAT_DM ? memcmp(n->key.id, key->id, 6) == 0 : strcmp(n->key.chan, key->chan) == 0);
}

void mn_text_prepare(mn_text_t *out, char const *title, char const *body) {
    mesh_sanitize(title, strlen(title), out->title, sizeof(out->title));
    mesh_sanitize(body, strlen(body), out->body, sizeof(out->body));
}

bool mn_queue_push(mn_queue_t *q, mn_kind_t kind, mesh_chat_key_t const *key, char const *title, char const *body,
                   bool mention) {
    mn_note_t *n = NULL;
    for (uint8_t i = 0; i < q->n; i++) {
        if (same_chat(&q->note[i], kind, key)) {
            n = &q->note[i];
            break;
        }
    }
    if (n) {
        if (n->count < 255) {
            n->count++;
        }
        if (mention || !n->mention) { // a mention keeps its line against plain ones after it
            mesh_sanitize(title, strlen(title), n->title, sizeof(n->title));
            mesh_sanitize(body, strlen(body), n->body, sizeof(n->body));
        }
        n->mention = n->mention || mention;
        return true;
    }
    if (q->n >= MN_QUEUE_MAX) {
        q->lost++;
        return false;
    }
    n = &q->note[q->n++];
    memset(n, 0, sizeof(*n));
    n->kind    = kind;
    n->key     = *key;
    n->mention = mention;
    n->count   = 1;
    mesh_sanitize(title, strlen(title), n->title, sizeof(n->title));
    mesh_sanitize(body, strlen(body), n->body, sizeof(n->body));
    return true;
}

size_t mn_queue_take(mn_queue_t *q, mn_note_t *out, size_t max) {
    size_t n = q->n < max ? q->n : max;
    memcpy(out, q->note, n * sizeof(mn_note_t));
    q->n = 0;
    return n;
}

bool mn_failtrack_step(mn_failtrack_t *t, uint32_t id, bool is_failed) {
    for (int i = 0; i < MN_FAILTRACK_MAX; i++) {
        if (t->id[i] == id && id) {
            bool fresh = is_failed && !t->failed[i];
            t->failed[i] = is_failed;
            return fresh;
        }
    }
    uint8_t slot = t->next;
    t->next      = (uint8_t)((t->next + 1) % MN_FAILTRACK_MAX);
    t->id[slot]  = id;
    t->failed[slot] = is_failed;
    return is_failed;
}
