// "fw soak" lines (#409): the key table is the contract (soak_line.h); host_tests/soak_line_test.c pins it.
#include "soak_line.h"

#include <string.h>

// The enum words. Indexed by the source enum's number; soak_cmd.c asserts those numbers where the enums are visible
// (lora.c for lora_state_t). The soak's words are owned here, so a display name changing elsewhere never changes the
// contract. Out of range prints SOAK_NA_WORD.
typedef struct {
    char const *const *w;
    int64_t            n;
} words_t;
#define WORDS(a) {a, (int64_t)(sizeof(a) / sizeof(a[0]))}

static char const *const RESET_W[] = {"unknown",  "poweron", "ext",  "sw",  "panic", "int_wdt", "task_wdt",   "wdt",
                                      "deepsleep", "brownout", "sdio", "usb", "jtag",  "efuse",   "pwr_glitch", "cpu_lockup"};
static char const *const C6_W[]    = {"up", "down", "attaching", "gaveup"};                         // c6w_state_t
static char const *const LORA_W[]  = {"unconf", "idle", "rx", "error", "tx"};                       // lora_state_t
static char const *const TH_W[]    = {"ok", "warn", "shutdown"};                                    // th_level_t
static char const *const SD_W[]    = {"off", "nocard", "notfat", "error", "mounted", "removed"};    // sd_state_t
static char const *const BAT_W[]   = {"unprobed", "absent", "lineslow", "nobus", "present", "stale", "lost"}; // bat_gauge_t

// Ceilings: a value above one prints it, so the longest line is known (the test fills every key with its ceiling)
#define U32_MAX_V   4294967295LL
#define BYTES_MAX_V 9999999999999LL // 13 digits: about 9 TB, past any SD card the badge can mount; up: 317 years of ms
#define DBM_MIN     (-128)

// What a key's value is: its range, or its words. Kept apart so a key costs 8 bytes of the image.
enum { U32, BYTES, FLAG, LEVEL, DBM, RESET, C6, LORA, TH, SD, BAT };
typedef struct {
    int64_t        min, max;
    words_t const *words; // NULL: a number
} soak_kind_t;
static words_t const     RESET_T = WORDS(RESET_W), C6_T = WORDS(C6_W), LORA_T = WORDS(LORA_W), TH_T = WORDS(TH_W),
                     SD_T = WORDS(SD_W), BAT_T = WORDS(BAT_W);
static soak_kind_t const KINDS[] = {
    [U32] = {SOAK_NA, U32_MAX_V, NULL}, [BYTES] = {SOAK_NA, BYTES_MAX_V, NULL}, [FLAG] = {SOAK_NA, 1, NULL},
    [LEVEL] = {SOAK_NA, 255, NULL},     [DBM] = {DBM_MIN, 0, NULL},             [RESET] = {0, 0, &RESET_T},
    [C6] = {0, 0, &C6_T},               [LORA] = {0, 0, &LORA_T},               [TH] = {0, 0, &TH_T},
    [SD] = {0, 0, &SD_T},               [BAT] = {0, 0, &BAT_T},
};

typedef struct {
    char const *key;
    uint16_t    off;  // of the int64_t field in soak_t
    uint8_t     line; // 1..SOAK_LINES
    uint8_t     kind;
} soak_key_t;
_Static_assert(sizeof(soak_t) <= UINT16_MAX, "soak_key_t.off is 16 bits");

#define NUM(l, k, f, kind) {k, (uint16_t)offsetof(soak_t, f), l, kind}
#define DBM(l, k, f)       NUM(l, k, f, DBM)
#define ENUM(l, k, f, w)   NUM(l, k, f, w)

// The order is the contract: append only (soak_line.h)
static soak_key_t const KEYS[] = {
    ENUM(1, "rst", reset, RESET),           NUM(1, "rstcrash", reset_crash, FLAG), NUM(1, "crash", crashes, U32),
    NUM(1, "slot", slot, FLAG),             NUM(1, "slotok", slot_ok, FLAG),
    NUM(1, "int", int_free, U32),           NUM(1, "intmin", int_min, U32),        NUM(1, "intbig", int_big, U32),
    NUM(1, "intwin", int_win, U32),         NUM(1, "intwx", int_wx, FLAG),
    NUM(1, "dma", dma_free, U32),           NUM(1, "dmamin", dma_min, U32),        NUM(1, "dmabig", dma_big, U32),
    NUM(1, "dmawin", dma_win, U32),         NUM(1, "dmawx", dma_wx, FLAG),
    NUM(1, "ps", ps_free, U32),             NUM(1, "psmin", ps_min, U32),          NUM(1, "psbig", ps_big, U32),
    NUM(1, "pswin", ps_win, U32),           NUM(1, "pswx", ps_wx, FLAG),
    NUM(1, "lp", lp_free, U32),             NUM(1, "lpmin", lp_min, U32),          NUM(1, "dlow", dma_drops, U32),

    NUM(2, "lock", lock_hits, U32),         NUM(2, "canary", canary, U32),         ENUM(2, "c6", c6_state, C6),
    NUM(2, "c6rs", c6_restarts, U32),       NUM(2, "hb", hb_beats, U32),           NUM(2, "hbmiss", hb_misses, U32),
    NUM(2, "hbdrop", hb_dropped, U32),      NUM(2, "pooltx", pool_tx_drops, U32),  NUM(2, "poolrx", pool_rx_drops, U32),
    ENUM(2, "lora", lora_state, LORA),      NUM(2, "lpkt", lora_packets, U32),     NUM(2, "lcrc", lora_crc, U32),
    NUM(2, "c6up", c6_up_s, U32),           NUM(2, "lage", lora_age_s, U32),       NUM(2, "lrx", lora_rx, U32),
    NUM(2, "ltx", lora_tx, U32),

    NUM(3, "mrx", mesh_rx, U32),            NUM(3, "mtx", mesh_tx, U32),           NUM(3, "mfail", mesh_failed, U32),
    NUM(3, "mtmo", mesh_timeouts, U32),     NUM(3, "mref", mesh_refused, U32),     NUM(3, "mdrop", mesh_rx_dropped, U32),
    NUM(3, "airlog", air_logged, U32),      NUM(3, "airdrop", air_dropped, U32),   DBM(3, "wdbm", wifi_dbm),
    NUM(3, "wup", wifi_up, FLAG),           ENUM(3, "therm", thermal, TH),         NUM(3, "store", storage_free, BYTES),
    ENUM(3, "sd", sd_state, SD),            NUM(3, "sdok", sd_ok, FLAG),           NUM(3, "sdfree", sd_free, BYTES),
    NUM(3, "sderr", sd_errors, U32),        ENUM(3, "bat", battery, BAT),          NUM(3, "batmv", battery_mv, U32),
    NUM(3, "kbd", kbd_ok, FLAG),            NUM(3, "kbdrst", kbd_resets, U32),     NUM(3, "bl", bl_req, LEVEL),
    NUM(3, "blapp", bl_app, LEVEL),         NUM(3, "kbl", kbl_req, LEVEL),         NUM(3, "kblapp", kbl_app, LEVEL),
};
#define N_KEYS (sizeof(KEYS) / sizeof(KEYS[0]))

static int64_t *field(soak_t *s, size_t off) {
    return (int64_t *)(void *)((char *)s + off);
}

static int64_t value_of(soak_t const *s, size_t off) {
    int64_t v;
    memcpy(&v, (char const *)s + off, sizeof(v));
    return v;
}

void soak_init(soak_t *s) {
    memset(s, 0, sizeof(*s));
    *field(s, offsetof(soak_t, boot))  = SOAK_NA;
    *field(s, offsetof(soak_t, up_ms)) = SOAK_NA;
    for (size_t i = 0; i < N_KEYS; i++) {
        *field(s, KEYS[i].off) = KEYS[i].kind == DBM ? 0 : SOAK_NA; // wdbm: 0 is its "no reading"
    }
}

// The line being built; ok turns false once something did not fit
typedef struct {
    char  *out;
    size_t cap, len;
    bool   ok;
} buf_t;

static void put_text(buf_t *b, char const *t) {
    size_t n = strlen(t);
    if (!b->ok || b->len + n >= b->cap) {
        b->ok = false;
        return;
    }
    memcpy(b->out + b->len, t, n);
    b->len += n;
    b->out[b->len] = '\0';
}

// Digits by hand: newlib-nano's printf on the badge has no %lld
static void put_num(buf_t *b, int64_t v) {
    char     tmp[24];
    size_t   i   = sizeof(tmp) - 1;
    bool     neg = v < 0;
    uint64_t u   = neg ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    tmp[i]       = '\0';
    do {
        tmp[--i] = (char)('0' + u % 10);
        u /= 10;
    } while (u);
    if (neg) {
        tmp[--i] = '-';
    }
    put_text(b, tmp + i);
}

static void put_key(buf_t *b, char const *key, int64_t v) {
    put_text(b, " ");
    put_text(b, key);
    put_text(b, "=");
    put_num(b, v);
}

static void put_entry(buf_t *b, soak_key_t const *k, int64_t v) {
    soak_kind_t const *kind = &KINDS[k->kind];
    if (kind->words) {
        put_text(b, " ");
        put_text(b, k->key);
        put_text(b, "=");
        put_text(b, v >= 0 && v < kind->words->n ? kind->words->w[v] : SOAK_NA_WORD);
        return;
    }
    if (v < kind->min) {
        v = k->kind == DBM ? 0 : SOAK_NA; // below the floor: unknown (wdbm: no reading)
    } else if (v > kind->max) {
        v = kind->max;
    }
    put_key(b, k->key, v);
}

size_t soak_line(soak_t const *s, unsigned n, char *out, size_t cap) {
    if (cap) {
        out[0] = '\0';
    }
    if (n < 1 || n > SOAK_LINES || !cap) {
        return 0;
    }
    buf_t b = {out, cap, 0, true};
    put_text(&b, "soak");
    put_num(&b, n);
    put_key(&b, "v", SOAK_FORMAT);
    int64_t boot = value_of(s, offsetof(soak_t, boot)), up = value_of(s, offsetof(soak_t, up_ms));
    put_key(&b, "boot", boot < SOAK_NA ? SOAK_NA : boot > U32_MAX_V ? U32_MAX_V : boot);
    put_key(&b, "up", up < SOAK_NA ? SOAK_NA : up > BYTES_MAX_V ? BYTES_MAX_V : up); // 64-bit: no wrap after 49.7 days
    for (size_t i = 0; i < N_KEYS; i++) {
        if (KEYS[i].line == n) {
            put_entry(&b, &KEYS[i], value_of(s, KEYS[i].off));
        }
    }
    if (!b.ok) {
        out[0] = '\0';
        return 0;
    }
    return b.len;
}
