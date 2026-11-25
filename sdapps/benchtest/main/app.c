// Bench test (#537): an SD app that exercises the file shims, the sandbox, the open-file book and exit() on the badge, so the
// wave-1 bench needs no console. It takes the raw keys; one key runs one test on main()'s own task (the app's one task: never
// on the LVGL task, where exit() would abort), and the result goes to the label (lichen_ui_call) and to a notification (the
// app has no log call; the OS logs the refused paths itself, tag sdfile).
//
//   r  file round trip on benchtest.dat: write, close, read, seek, tell        -> "RT ok" / "RT FAIL <step>"
//   e  escapes: "../x", another app's dir (absolute) and "FLASH0:x" must each refuse -> "ESC ok" / "ESC FAIL <which>"
//   m  open five files: the fifth must refuse                                    -> "MAX ok" / "MAX FAIL <n>"
//   f  hold one file open (benchtest.hold): then close the app or "fw sd unmount" -> "HOLD open"
//   Esc  closes the screen like a normal Esc (shell_back): the unload runs on the normal close path (#556)
//   q  exit(0) from this task: the app closes through the normal unload          -> (the screen goes)
//
// Slow stop (#541): when a file named slowstop exists in the app's own folder, main() keeps running SLOW_STOP_MS after stop() was
// called, longer than the unload's stop deadline, so the loader gives the unload up and finishes it when the task ends ("unloaded
// late, its last task has ended"). The file is looked for once, as main() starts. Without it nothing changes.
//   switch on:  tools/badge.sh ota-file <ip> <any file> 'SD0:[lichen.apps.benchtest]slowstop'   (/sd/lichen/apps/benchtest/slowstop)
//   switch off: tools/badge.sh ota-rm <ip> 'SD0:[lichen.apps.benchtest]slowstop'
//
// Leak (#536): when a file named leak exists in the app's own folder (/sd/lichen/apps/benchtest/leak, looked for once as main()
// starts), the app takes LEAK_BLOCKS blocks of LEAK_BYTES with lichen_alloc and 2 more through malloc (leak.c, the one file the boundary check lets call it) and never frees them, so the OS's sweep at unload has something to free
// and logs "freed 2 leftover blocks (8192 B)" for the malloc ones; the 3 lichen_alloc ones are the book's (fw mem). The pointers are kept in a
// volatile array only so the compiler cannot drop the allocations. Same push and ota-rm as slowstop, with the name leak.
#include "leak.h"
#include "lichen_keys.h"
#include "lichen_mem.h"
#include "lichen_notify.h"
#include "lichen_sd_app.h"
#include "lichen_ui.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// TCA8418 codes (board/tca8418_keymap.h)
#define CODE_ESC 0x01
#define CODE_Q 0x16
#define CODE_E 0x18
#define CODE_R 0x19
#define CODE_F 0x23
#define CODE_M 0x30

#define SLOW_STOP_MS 8000 // past LIFE_DEADLINE_UNLOAD_MS (5000, app_life.h): the stop deadline

#define LEAK_BLOCKS 3
#define LEAK_BYTES  4096

#define DATA     "benchtest-0123456789"
#define DATA_LEN 20

static lv_obj_t *g_screen; // LVGL task only
static char      g_text[48] = "r e m f q";
static _Atomic char g_pending;
static atomic_bool  g_stopping;
static FILE        *g_held;
static void *volatile g_leaked[LEAK_BLOCKS]; // never freed on purpose (the leak switch)

// ---- results ---------------------------------------------------------------------------------------------------------

// LVGL task: the label is the screen's first child, and only while that screen is the one showing (the app's screen may be gone
// already: then there is nothing to write to)
static void show_cb(void *arg) {
    (void)arg;
    if (g_screen && lv_screen_active() == g_screen && lv_obj_get_child_count(g_screen) > 0) {
        lv_label_set_text(lv_obj_get_child(g_screen, 0), g_text);
    }
}

static void report(char const *what, bool ok, char const *detail) {
    snprintf(g_text, sizeof(g_text), "%s %s%s%s", what, ok ? "ok" : "FAIL", detail[0] ? " " : "", detail);
    lichen_ui_call(show_cb, NULL);
    lichen_notify(lichen_ctx_self(), ok ? LN_INFO : LN_WARN, "benchtest", g_text, 0);
}

// ---- the tests (the app task) -----------------------------------------------------------------------------------------

static void test_round_trip(void) {
    char  buf[DATA_LEN + 1] = {0};
    FILE *f                 = fopen("benchtest.dat", "w");
    if (!f) {
        return report("RT", false, "open w");
    }
    bool wrote = fwrite(DATA, 1, DATA_LEN, f) == DATA_LEN;
    fclose(f);
    if (!wrote) {
        return report("RT", false, "write");
    }
    f = fopen("benchtest.dat", "r");
    if (!f) {
        return report("RT", false, "open r");
    }
    char const *bad = NULL;
    if (fread(buf, 1, DATA_LEN, f) != DATA_LEN || memcmp(buf, DATA, DATA_LEN) != 0) {
        bad = "read";
    } else if (fseek(f, 10, SEEK_SET) != 0) {
        bad = "seek";
    } else if (ftell(f) != 10) {
        bad = "tell";
    } else if (fread(buf, 1, 10, f) != 10 || memcmp(buf, DATA + 10, 10) != 0) {
        bad = "reread";
    }
    fclose(f);
    report("RT", !bad, bad ? bad : "");
}

static void test_escapes(void) {
    static char const *const paths[] = {"../x", "/sd/lichen/apps/hello/x", "FLASH0:x"};
    for (int i = 0; i < 3; i++) {
        FILE *f = fopen(paths[i], "w"); // "w": an escape that works leaves a file to find
        if (f) {
            fclose(f);
            char which[8];
            snprintf(which, sizeof(which), "#%d", i);
            return report("ESC", false, which);
        }
    }
    report("ESC", true, "");
}

static void test_max(void) {
    FILE *f[5];
    char  name[24];
    int   opened = 0;
    for (int i = 0; i < 5; i++) {
        snprintf(name, sizeof(name), "benchtest.m%d", i);
        f[i] = fopen(name, "w");
        opened += f[i] != NULL;
    }
    bool ok = opened == 4 && f[4] == NULL;
    for (int i = 0; i < 5; i++) {
        if (f[i]) {
            fclose(f[i]);
        }
    }
    char n[8];
    snprintf(n, sizeof(n), "%d", opened);
    report("MAX", ok, ok ? "" : n);
}

static void test_hold(void) {
    if (g_held) {
        return report("HOLD", true, "already open");
    }
    g_held = fopen("benchtest.hold", "w");
    if (g_held) {
        fwrite("held", 1, 4, g_held);
    }
    report("HOLD", g_held != NULL, g_held ? "open" : "no file");
}

static void run(char c) {
    switch (c) {
    case 'r':
        test_round_trip();
        break;
    case 'e':
        test_escapes();
        break;
    case 'm':
        test_max();
        break;
    case 'f':
        test_hold();
        break;
    case 'x':
        shell_back(); // the normal screen close: the last screen goes, the loader stops the app (#556)
        break;
    case 'q':
        report("EXIT", true, "0");
        exit(0); // from this task, never from an LVGL callback
    default:
        break;
    }
}

// ---- hooks ------------------------------------------------------------------------------------------------------------

// LVGL task: only queues the key
static void on_key(lichen_raw_key_t const *key, void *arg) {
    (void)arg;
    if (!key->pressed) {
        return;
    }
    char c = key->code == CODE_R ? 'r' : key->code == CODE_E ? 'e' : key->code == CODE_M ? 'm'
           : key->code == CODE_F ? 'f' : key->code == CODE_Q ? 'q' : key->code == CODE_ESC ? 'x' : 0;
    char none = 0;
    if (c) {
        atomic_compare_exchange_strong(&g_pending, &none, c); // a key while one runs is dropped
    }
}

static void app_open(lv_obj_t *screen) {
    g_screen         = screen;
    lv_obj_t *label  = lv_label_create(screen);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_text(label, g_text);
    if (!lichen_keys_raw_take(on_key, NULL)) {
        lv_label_set_text(label, "raw keys busy");
    }
}

static void release_cb(void *arg) {
    (void)arg;
    lichen_keys_raw_release(on_key, NULL);
    g_screen = NULL;
}

static bool app_stop(void) {
    atomic_store(&g_stopping, true);
    lichen_ui_call(release_cb, NULL); // the keys are LVGL's: released there (a no-op when the screen's close already did)
    return true;
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    lichen_sd_hooks_t const hooks = {.size = sizeof(hooks), .open = app_open, .stop = app_stop};
    if (!lichen_sd_app_hooks(&hooks)) {
        return 1;
    }
    FILE *flag = fopen("slowstop", "r"); // relative: the app's own folder
    bool  slow = flag != NULL;
    if (flag) {
        fclose(flag);
    }
    FILE *leak = fopen("leak", "r");
    if (leak) {
        fclose(leak);
        for (int i = 0; i < LEAK_BLOCKS; i++) {
            g_leaked[i] = lichen_alloc(lichen_ctx_self(), LEAK_BYTES, LICHEN_MEM_BULK);
        }
        bench_leak_shim_blocks(); // and 2 through malloc (leak.c): the loader's block set
    }
    // main() goes on as the app's one task (the unload waits for it: stop sets g_stopping)
    while (!atomic_load(&g_stopping)) {
        char c = atomic_exchange(&g_pending, 0);
        if (c) {
            run(c);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (slow) {
        vTaskDelay(pdMS_TO_TICKS(SLOW_STOP_MS)); // the stop hook answered at once: it is the task that overruns
    }
    if (g_held) {
        fclose(g_held);
        g_held = NULL;
    }
    return 0;
}
