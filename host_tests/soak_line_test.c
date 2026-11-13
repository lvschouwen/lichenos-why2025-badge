// Host test: the "fw soak" lines (#409), a contract for the soak sampler (#407): every key, its order, the sentinels,
// the enum words and the longest line
#include "soak_line.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static char line[SOAK_LINE_MAX];

static char const *at(soak_t const *s, unsigned n) {
    return soak_line(s, n, line, sizeof(line)) ? line : "";
}

#define CHECK_LINE(s, n, want)                                                                                         \
    do {                                                                                                               \
        char const *got_ = at(s, n);                                                                                   \
        if (strcmp(got_, want) != 0) {                                                                                 \
            printf("FAIL %s:%d: line %u\n  got  %s\n  want %s\n", __FILE__, __LINE__, (unsigned)(n), got_, want);      \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

// The pinned key order, line by line (after "soakN"). Appending a key appends here; nothing else may change.
static char const *const KEYS[SOAK_LINES][40] = {
    {"v", "boot", "up", "rst", "rstcrash", "crash", "slot", "slotok", "int", "intmin", "intbig", "intwin", "intwx", "dma",
     "dmamin", "dmabig", "dmawin", "dmawx", "ps", "psmin", "psbig", "pswin", "pswx", "lp", "lpmin", "dlow", NULL},
    {"v", "boot", "up", "lock", "canary", "c6", "c6rs", "hb", "hbmiss", "hbdrop", "pooltx", "poolrx", "lora", "lpkt",
     "lcrc", "c6up", "lage", "lrx", "ltx", NULL},
    {"v", "boot", "up", "mrx", "mtx", "mfail", "mtmo", "mref", "mdrop", "airlog", "airdrop", "wdbm", "wup", "therm",
     "store", "sd", "sdok", "sdfree", "sderr", "bat", "batmv", "kbd", "kbdrst", "bl", "blapp", "kbl", "kblapp", NULL},
};

// A badge as a soak sample might see it: every value distinct, so a key reading the wrong field shows
static soak_t sample(void) {
    soak_t s;
    soak_init(&s);
    s = (soak_t){
        .boot = 42, .up_ms = 3600250, .reset = 3, .reset_crash = 0, .crashes = 2, .slot = 1, .slot_ok = 1,
        .int_free = 81234, .int_min = 70001, .int_big = 30002, .int_win = 75003, .int_wx = 1,
        .dma_free = 99004, .dma_min = 89005, .dma_big = 28006, .dma_win = 90007, .dma_wx = 0,
        .ps_free = 30000008, .ps_min = 29000009, .ps_big = 16000010, .ps_win = 29500011, .ps_wx = 1,
        .lp_free = 12012, .lp_min = 11013, .dma_drops = 3,
        .lock_hits = 0, .canary = 4, .c6_state = 0, .c6_restarts = 1, .hb_beats = 120, .hb_misses = 5, .hb_dropped = 6,
        .pool_tx_drops = 7, .pool_rx_drops = 8, .lora_state = 2, .lora_packets = 300, .lora_crc = 9, .c6_up_s = 3500,
        .lora_age_s = 10, .lora_rx = 301, .lora_tx = 11,
        .mesh_rx = 290, .mesh_tx = 12, .mesh_failed = 13, .mesh_timeouts = 14, .mesh_refused = 15, .mesh_rx_dropped = 16,
        .air_logged = 302, .air_dropped = 17, .wifi_dbm = -61, .wifi_up = 1, .thermal = 0, .storage_free = 1234567,
        .sd_state = 4, .sd_ok = 1, .sd_free = 31000000000LL, .sd_errors = 18, .battery = 1, .battery_mv = SOAK_NA,
        .kbd_ok = 1, .kbd_resets = 19, .bl_req = 40, .bl_app = 41, .kbl_req = 20, .kbl_app = 21,
    };
    return s;
}

static void exact_lines(void) {
    soak_t s = sample();
    CHECK_LINE(&s, 1,
               "soak1 v=1 boot=42 up=3600250 rst=sw rstcrash=0 crash=2 slot=1 slotok=1 int=81234 intmin=70001 "
               "intbig=30002 intwin=75003 intwx=1 dma=99004 dmamin=89005 dmabig=28006 dmawin=90007 dmawx=0 ps=30000008 "
               "psmin=29000009 psbig=16000010 pswin=29500011 pswx=1 lp=12012 lpmin=11013 dlow=3");
    CHECK_LINE(&s, 2,
               "soak2 v=1 boot=42 up=3600250 lock=0 canary=4 c6=up c6rs=1 hb=120 hbmiss=5 hbdrop=6 pooltx=7 poolrx=8 "
               "lora=rx lpkt=300 lcrc=9 c6up=3500 lage=10 lrx=301 ltx=11");
    CHECK_LINE(&s, 3,
               "soak3 v=1 boot=42 up=3600250 mrx=290 mtx=12 mfail=13 mtmo=14 mref=15 mdrop=16 airlog=302 airdrop=17 "
               "wdbm=-61 wup=1 therm=ok store=1234567 sd=mounted sdok=1 sdfree=31000000000 sderr=18 bat=absent "
               "batmv=-1 kbd=1 kbdrst=19 bl=40 blapp=41 kbl=20 kblapp=21");
}

// Splits a line into its keys and checks them against KEYS[n-1]: names, order, count, one space each, key=value
static void check_keys(char const *text, unsigned n) {
    char name[8];
    snprintf(name, sizeof(name), "soak%u ", n);
    CHECK(strncmp(text, name, strlen(name)) == 0);
    char const *p = text + strlen(name);
    size_t      i = 0;
    for (; KEYS[n - 1][i]; i++) {
        char const *eq  = strchr(p, '=');
        char const *end = strchr(p, ' ');
        end             = end ? end : p + strlen(p);
        CHECK(eq && eq < end && end > eq + 1);
        if (!eq || eq >= end) {
            return;
        }
        CHECK((size_t)(eq - p) == strlen(KEYS[n - 1][i]) && strncmp(p, KEYS[n - 1][i], (size_t)(eq - p)) == 0);
        for (char const *c = eq + 1; c < end; c++) { // a number or a lowercase word
            CHECK((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'z') || *c == '_' || (*c == '-' && c == eq + 1));
        }
        p = *end ? end + 1 : end;
        CHECK(*end == '\0' || (*p && *p != ' ')); // single spaces, none at the end
    }
    CHECK(*p == '\0'); // no key past the pinned list
}

static void keys_in_order(void) {
    soak_t s = sample();
    for (unsigned n = 1; n <= SOAK_LINES; n++) {
        check_keys(at(&s, n), n);
    }
    soak_init(&s); // the sentinels keep the same keys
    for (unsigned n = 1; n <= SOAK_LINES; n++) {
        check_keys(at(&s, n), n);
    }
}

static void sentinels(void) {
    soak_t s;
    soak_init(&s);
    CHECK_LINE(&s, 2,
               "soak2 v=1 boot=-1 up=-1 lock=-1 canary=-1 c6=na c6rs=-1 hb=-1 hbmiss=-1 hbdrop=-1 pooltx=-1 poolrx=-1 "
               "lora=na lpkt=-1 lcrc=-1 c6up=-1 lage=-1 lrx=-1 ltx=-1");
    CHECK(strstr(at(&s, 3), " wdbm=0 ")); // no Wi-Fi reading is 0, never -1 (a dBm)
    CHECK(strstr(at(&s, 1), " rst=na "));
    s.c6_state = 4; // out of range
    s.reset    = 16;
    s.thermal  = -7;
    CHECK(strstr(at(&s, 2), " c6=na "));
    CHECK(strstr(at(&s, 1), " rst=na "));
    CHECK(strstr(at(&s, 3), " therm=na "));
    s.crashes  = -5; // below the floor: unknown
    s.wifi_dbm = -300;
    CHECK(strstr(at(&s, 1), " crash=-1 "));
    CHECK(strstr(at(&s, 3), " wdbm=0 "));
}

static char const *word(soak_t *s, int64_t *f, int64_t v, unsigned n, char const *key) {
    static char got[24];
    *f = v;
    char const *p = strstr(at(s, n), key);
    got[0]        = '\0';
    if (p) {
        p += strlen(key);
        size_t len = strcspn(p, " ");
        snprintf(got, sizeof(got), "%.*s", (int)len, p);
    }
    return got;
}

static void enum_words(void) {
    soak_t s = sample();
    static char const *const rst[] = {"unknown",  "poweron", "ext",  "sw",  "panic", "int_wdt", "task_wdt",   "wdt",
                                      "deepsleep", "brownout", "sdio", "usb", "jtag",  "efuse",   "pwr_glitch", "cpu_lockup"};
    for (int i = 0; i < 16; i++) {
        CHECK(strcmp(word(&s, &s.reset, i, 1, " rst="), rst[i]) == 0);
    }
    static char const *const c6[] = {"up", "down", "attaching", "gaveup"};
    for (int i = 0; i < 4; i++) {
        CHECK(strcmp(word(&s, &s.c6_state, i, 2, " c6="), c6[i]) == 0);
    }
    static char const *const lora[] = {"unconf", "idle", "rx", "error", "tx"};
    for (int i = 0; i < 5; i++) {
        CHECK(strcmp(word(&s, &s.lora_state, i, 2, " lora="), lora[i]) == 0);
    }
    static char const *const th[] = {"ok", "warn", "shutdown"};
    for (int i = 0; i < 3; i++) {
        CHECK(strcmp(word(&s, &s.thermal, i, 3, " therm="), th[i]) == 0);
    }
    static char const *const sd[] = {"off", "nocard", "notfat", "error", "mounted", "removed"};
    for (int i = 0; i < 6; i++) {
        CHECK(strcmp(word(&s, &s.sd_state, i, 3, " sd="), sd[i]) == 0);
    }
    static char const *const bat[] = {"unprobed", "absent", "lineslow", "nobus", "present", "stale", "lost"};
    for (int i = 0; i < 7; i++) {
        CHECK(strcmp(word(&s, &s.battery, i, 3, " bat="), bat[i]) == 0);
    }
}

// Every key at (and past) its ceiling: each line still fits the console's reply, whole
static void extremes_fit(void) {
    soak_t s;
    soak_init(&s);
    int64_t *f = (int64_t *)(void *)&s;
    for (size_t i = 0; i < sizeof(s) / sizeof(int64_t); i++) {
        f[i] = INT64_MAX;
    }
    s.reset = 15; // the longest words
    s.c6_state = 2;
    s.thermal  = 2;
    s.sd_state = 5;
    s.battery  = 0;
    s.lora_state = 0;
    s.wifi_dbm = -128;
    for (unsigned n = 1; n <= SOAK_LINES; n++) {
        size_t len = soak_line(&s, n, line, sizeof(line));
        printf("soak%u longest: %zu of %d\n", n, len, SOAK_LINE_MAX - 1);
        CHECK(len > 0 && len < SOAK_LINE_MAX && strlen(line) == len);
        check_keys(line, n);
    }
    CHECK(strstr(at(&s, 1), " crash=4294967295 "));
    CHECK(strstr(at(&s, 1), " up=9999999999999 ")); // 64-bit uptime: no wrap after 49.7 days
    CHECK(strstr(at(&s, 3), " sdfree=9999999999999 "));
    CHECK(strstr(at(&s, 3), " wdbm=-128 "));
    CHECK(strstr(at(&s, 3), " kblapp=255"));
    s.wifi_dbm = INT64_MIN;
    CHECK(strstr(at(&s, 3), " wdbm=0 "));
    s.up_ms = INT64_MIN;
    CHECK(strstr(at(&s, 1), " up=-1 "));
}

// SOAK_LINES lines and no other; a buffer too small gets nothing rather than a cut line
static void split_rule(void) {
    soak_t s = sample();
    CHECK(SOAK_LINES == 3);
    CHECK(soak_line(&s, 0, line, sizeof(line)) == 0 && line[0] == '\0');
    CHECK(soak_line(&s, SOAK_LINES + 1, line, sizeof(line)) == 0 && line[0] == '\0');
    size_t len = soak_line(&s, 1, line, sizeof(line));
    CHECK(len > 0);
    char small[SOAK_LINE_MAX];
    CHECK(soak_line(&s, 1, small, len + 1) == len);
    CHECK(soak_line(&s, 1, small, len) == 0 && small[0] == '\0');
    CHECK(soak_line(&s, 1, small, 0) == 0);
}

// #416: rstcrash is sticky across the PSRAM re-arm restart that follows a crash at 120 MHz
#define LINE_REARM "2026-11-13T10:00:19Z fw-abc up=19s psram 120 re-arm after crash"
#define LINE_REBOOT "2026-11-13T10:30:00Z fw-abc up=1800s fw reboot"

static void sticky_crash_flag(void) {
    CHECK(soak_reset_crash(true, false, NULL) == 1);              // the panic boot itself, as crash.c judged
    CHECK(soak_reset_crash(false, true, LINE_REARM) == 1);        // the re-arm boot after the panic
    CHECK(soak_reset_crash(false, true, LINE_REARM " caller=0x4ff00012") == 1);
    CHECK(soak_reset_crash(false, true, LINE_REBOOT) == 0);       // a plain software restart
    // the second ordinary reboot after the re-arm boot: only the newest line is read, and it is that reboot's own
    CHECK(soak_reset_crash(false, true, LINE_REBOOT " caller=0x4ff00012") == 0);
    CHECK(soak_reset_crash(false, false, LINE_REARM) == 0);       // a power cycle or EN reset after the re-arm boot
    CHECK(soak_reset_crash(false, true, NULL) == 0);              // no history
    CHECK(soak_reset_crash(false, true, "t? fw up=1s psram 120 re-arm after crashes") == 0);
    // fw crash clear empties the crash log and count, never the restart history, and the rule takes neither: crash.c's
    // verdict is 0 here (a software reset) and the re-arm line alone keeps the flag
    CHECK(soak_reset_crash(false, true, LINE_REARM) == 1);
    // the text speed_job gives the restart must stay the one the rule looks for
    FILE *f = fopen(PSRAM_SPEED_SRC, "r");
    CHECK(f != NULL);
    if (f) {
        char buf[1 << 16];
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        buf[n] = '\0';
        fclose(f);
        CHECK(strstr(buf, "arm_and_restart(\"psram 120 re-arm after crash\")") != NULL);
    }
}

int main(void) {
    exact_lines();
    keys_in_order();
    sentinels();
    enum_words();
    extremes_fit();
    split_rule();
    sticky_crash_flag();
    if (failures) {
        printf("soak_line_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("soak_line_test: all passed\n");
    return 0;
}
