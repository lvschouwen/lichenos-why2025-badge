#include "secure_wipe.h"
#include "fw_console.h"
#include "fw_slot.h"
#include "mem_fmt.h"

#include "assets.h"
#include "console_capture.h"
#include "console_cmd.h"
#include "console_os_rules.h"
#include "dev_gate.h"
#include "driver/uart.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "restart_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include <string.h>
#include <time.h>

// 12 B of .bss per slot (name, usage, fn). tools/check_console_slots.py fails the build above 80 % of this (#243)
#define MAX_COMMANDS   96
#define MAX_RULE_TABLES 32 // 4 B of .bss each: the commands with a sub-command table (#464)
#define MAX_TASKS_SEEN 48

static char const TAG[] = "console";

static char const *app_ids[CONSOLE_MAX_APPS]; // the app ids that registered a command (#423), literals kept by pointer
static int         n_apps;
static console_entry_t commands[MAX_COMMANDS]; // dev_mode.c looks commands up and lists them (#373)
static int       n_commands;

#define REPLY_MAX 384 // longer replies reach the UART whole but are cut in the log and the Wi-Fi console

static void (*tap)(char const *text, size_t len);

void console_set_tap(void (*fn)(char const *text, size_t len)) {
    tap = fn;
}

// The output of console_exec (#38): only the task that runs it is collected, so the UART console keeps working
static struct {
    TaskHandle_t      task;
    console_capture_t out;
    bool              done_called;
    console_done_fn   done;
    void             *ctx;
    console_origin_t  origin; // WIFI or LOCAL: a UART line is never captured
} capture;

static TaskHandle_t _Atomic s_uart_task; // the console task: its commands are typed on the UART (#373)

// One command runs at a time, from the UART or from Wi-Fi: commands share state (the capture, the commands' statics)
static SemaphoreHandle_t run_mutex;
#define RUN_WAIT_MS 2000
// reply_v formats the whole "fw: text\n" line into one static buffer (off the 4 KB console and c6link task stacks),
// guarded by its own mutex, and writes it with one fwrite (#327). The text is cut at REPLY_MAX - 1 everywhere.
static SemaphoreHandle_t reply_mutex;
static char              reply_line[CONSOLE_LINE_PREFIX_LEN + REPLY_MAX + 1];

typedef enum { LINE_PUBLIC, LINE_SECRET, LINE_PRIVATE } line_kind_t;

static void reply_emit(line_kind_t kind, console_route_t to, char *line, size_t cap, char const *fmt, va_list ap) {
    bool   secret = kind == LINE_SECRET;
    size_t text_len;
    size_t n = console_line_fmt(line, cap, fmt, ap, &text_len);
    if (to.uart) {
        fwrite(line, 1, n, stdout);
    } else {
        fputs("fw: " CONSOLE_SECRET_UART "\n", stdout);
    }
    if (secret) {
        fflush(stdout); // out of stdio's buffer now rather than whenever the next line comes
    }
    char *text = line + CONSOLE_LINE_PREFIX_LEN;
    if (text_len > 0) {
        if (tap && to.tap) {
            tap(text, text_len + 1); // the newline follows the text in the same buffer
        }
        if (to.capture) {
            console_capture_add(&capture.out, text, text_len, secret);
        }
    }
    if (kind != LINE_PUBLIC) {
        secure_wipe(line, cap);
    }
}

// Where a line goes is console_route's decision (console_capture.c, host-tested): a secret line (#123) reaches only the
// one who asked, never the tap (the log store is read over Wi-Fi and on the screen later). Its formatted copy is wiped.
static void reply_v(line_kind_t kind, char const *fmt, va_list ap) {
    bool            capturing = capture.task && capture.task == xTaskGetCurrentTaskHandle();
    console_route_t to = kind == LINE_PRIVATE ? console_route_private(capturing) : console_route(kind == LINE_SECRET, capturing);
    if (reply_mutex && xSemaphoreTake(reply_mutex, pdMS_TO_TICKS(RUN_WAIT_MS)) == pdTRUE) {
        reply_emit(kind, to, reply_line, sizeof(reply_line), fmt, ap);
        xSemaphoreGive(reply_mutex);
    } else {
        // Before the mutex exists or when it is held too long: a short line on the stack still leaves in one write
        char line[96];
        to.tap = to.capture = false;
        reply_emit(kind, to, line, sizeof(line), fmt, ap);
    }
}

void console_reply(char const *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    reply_v(LINE_PUBLIC, fmt, ap);
    va_end(ap);
}

void console_reply_secret(char const *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    reply_v(LINE_SECRET, fmt, ap);
    va_end(ap);
}

void console_reply_private(char const *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    reply_v(LINE_PRIVATE, fmt, ap);
    va_end(ap);
}

// Components register from their own tasks at boot while the console task may already run a command (#5): writers
// take commands_lock, and an entry is complete before the count that makes it visible is published. Entries are never
// changed or removed, so readers need only the count (commands_count) and no lock.
static portMUX_TYPE commands_lock = portMUX_INITIALIZER_UNLOCKED;

static int commands_count(void) {
    return __atomic_load_n(&n_commands, __ATOMIC_ACQUIRE);
}

// The commands' sub-command tables (#464): an entry keeps 1 + the index here in its padding, not a pointer of its own.
// Written once per table before the entry that names it is published, like the commands.
static console_sub_t const *rule_tables[MAX_RULE_TABLES];
static int                  n_rule_tables;

static void register_entry(char const *app, char const *name, char const *usage, console_fn_t fn, bool dev,
                           console_sub_t const *rules) {
    portENTER_CRITICAL(&commands_lock);
    int  n    = n_commands;
    bool clash = dev_mode_name_clash(commands, n, app_ids, n_apps, app, name); // #423: a word must mean one thing
    // A command whose rules cannot be kept is not registered at all: without them it would run unchecked
    bool rules_room = !rules || n_rule_tables < MAX_RULE_TABLES;
    int  id    = app && !clash && rules_room ? dev_mode_app_intern(app_ids, &n_apps, CONSOLE_MAX_APPS, app) : 0;
    bool room  = !clash && rules_room && n < MAX_COMMANDS && (app == NULL || id != 0);
    if (room) {
        unsigned char r = 0;
        if (rules) {
            rule_tables[n_rule_tables] = rules;
            r                          = (unsigned char)++n_rule_tables;
        }
        commands[n] = (console_entry_t){name, usage, fn, dev, (unsigned char)id, r};
        __atomic_store_n(&n_commands, n + 1, __ATOMIC_RELEASE);
    }
    portEXIT_CRITICAL(&commands_lock);
    if (clash) {
        ESP_LOGE(TAG, "command %s (app %s) clashes with an app id or a command name: not registered", name, app ? app : "-");
    } else if (!room) {
        ESP_LOGE(TAG, "no room for command %s (table of %d, %d apps or %d rule tables is full)", name, MAX_COMMANDS,
                 CONSOLE_MAX_APPS, MAX_RULE_TABLES);
    }
}

void console_register(char const *name, char const *usage, console_fn_t fn) {
    register_entry(NULL, name, usage, fn, false, NULL);
}

void console_register_app(char const *app, char const *name, char const *usage, console_fn_t fn) {
    register_entry(app, name, usage, fn, false, NULL);
}

void console_register_rules(char const *name, char const *usage, console_fn_t fn, console_sub_t const *rules) {
    register_entry(NULL, name, usage, fn, false, rules);
}

void console_register_app_rules(char const *app, char const *name, char const *usage, console_fn_t fn,
                                console_sub_t const *rules) {
    register_entry(app, name, usage, fn, false, rules);
}

// Developer mode (#373): read once from the settings service at bind time, then kept here. A release build points at
// no fw dev, so its image holds no developer mode words at all.
#ifdef CONFIG_LICHEN_DEV_COMMANDS
#define DEV_OFF_REPLY DEV_MODE_OFF_NOTE
#else
#define DEV_OFF_REPLY "not in this build"
#endif
static atomic_bool s_dev_on;

#ifdef CONFIG_LICHEN_DEV_COMMANDS
// What tools/check_release_image.sh looks for (#401): a string only debug code holds, unlike UI wording such as
// "developer mode". `retain` keeps it through --gc-sections; the developer image is checked to hold it.
__attribute__((used, retain)) char const lichen_debug_build_marker[] = "LICHEN-DEBUG-BUILD-MARKER";

static fw_set_u8_fn s_dev_save;

void console_register_dev(char const *name, char const *usage, console_fn_t fn) {
    register_entry(NULL, name, usage, fn, true, NULL);
}
#endif

void console_bind_dev_store(fw_get_u8_fn get, fw_set_u8_fn set) {
#ifdef CONFIG_LICHEN_DEV_COMMANDS
    s_dev_save = set;
    atomic_store(&s_dev_on, get && get(DEV_MODE_KEY, 0) == 1);
#else
    (void)get;
    (void)set; // a release build has no debug commands, so nothing to turn on: a saved "on" stays unread
#endif
}

bool console_dev_mode(void) {
    return atomic_load(&s_dev_on);
}

bool console_dev_set(bool on) {
#ifdef CONFIG_LICHEN_DEV_COMMANDS
    atomic_store(&s_dev_on, on);
    ESP_LOGI(TAG, "developer mode %s", on ? "on" : "off");
    return s_dev_save && s_dev_save(DEV_MODE_KEY, on ? 1 : 0);
#else
    (void)on;
    return false;
#endif
}

bool console_dev_gate(char const *what) {
    if (console_dev_mode()) {
        return true;
    }
    console_reply("%s: " DEV_OFF_REPLY, what);
    return false;
}

static void help_line(char const *usage, void *ctx) {
    (void)ctx;
    console_reply("%s", usage);
}

// The index's line width: a name list wraps here (a 4 KB Wi-Fi reply is plenty; an 80-column terminal is not the aim)
#define HELP_WIDTH 100

// Full usages, every group under its app id (#423); every command also runs as "fw <app> <command>"
static int help_apps(int n) {
    int listed = 0;
    int apps   = __atomic_load_n(&n_apps, __ATOMIC_ACQUIRE);
    for (int a = 1; a <= apps; a++) {
        if (dev_mode_help_app(commands, n, console_dev_mode(), a, NULL, NULL) > 0) {
            console_reply("%s (also: fw %s <command>):", app_ids[a - 1], app_ids[a - 1]);
            listed += dev_mode_help_app(commands, n, console_dev_mode(), a, help_line, NULL);
        }
    }
    return listed;
}

static void help_count(int n, int listed) {
#ifdef CONFIG_LICHEN_DEV_COMMANDS
    if (listed < n) {
        console_reply("%d of %d commands registered (%d debug ones hidden: fw dev)", n, MAX_COMMANDS, n - listed);
        return;
    }
#else
    (void)listed; // a release build registers no debug commands: all are listed
#endif
    console_reply("%d of %d commands registered", n, MAX_COMMANDS);
}

// "fw help": the names only (#439: the usages of 60 commands no longer fit the Wi-Fi reply); "fw help <command|app>"
// gives usages, "fw help all" every one (the USB console's old listing)
static int help_index(int n) {
    help_view_t v      = {commands, n, console_dev_mode()};
    int         listed = dev_mode_help_names(&v, 0, "OS:", HELP_WIDTH, help_line, NULL);
    int  apps   = __atomic_load_n(&n_apps, __ATOMIC_ACQUIRE);
    for (int a = 1; a <= apps; a++) {
        char head[DEV_HELP_LINE_MAX]; // two app ids and the frame: longer ids are cut, not lost
        snprintf(head, sizeof(head), "%s (fw %s <cmd>):", app_ids[a - 1], app_ids[a - 1]);
        listed += dev_mode_help_names(&v, a, head, HELP_WIDTH, help_line, NULL);
    }
    return listed;
}

static void cmd_help(char const *args) {
    int n = commands_count();
    char topic[48];
    while (*args == ' ') {
        args++;
    }
    size_t len = strlen(args);
    while (len && args[len - 1] == ' ') {
        len--; // "all " is all
    }
    snprintf(topic, sizeof(topic), "%.*s", (int)len, args);
    args = topic;
    if (*args && strcmp(args, "all") == 0) {
        help_count(n, dev_mode_help_app(commands, n, console_dev_mode(), 0, help_line, NULL) + help_apps(n));
    } else if (*args) {
        help_view_t v = {commands, n, console_dev_mode()};
        if (!dev_mode_help_topic(&v, app_ids, __atomic_load_n(&n_apps, __ATOMIC_ACQUIRE), args, help_line, NULL)) {
            console_reply("help: no such command or app (fw help lists them)"); // never the typed word (#123)
        }
    } else {
        int listed = help_index(n);
        console_reply("fw help <command|app>: its usage; fw help all: every usage (long)");
        help_count(n, listed);
    }
}

static void print_heap(char const *name, uint32_t caps) {
    multi_heap_info_t info;
    heap_caps_get_info(&info, caps);
    char line[160];
    // total = free + allocated: what the heap manages, so the percentages in the standard have a denominator (#205)
    mem_fmt_heap(line, sizeof(line), name, (uint32_t)info.total_free_bytes, (uint32_t)info.largest_free_block,
                 (uint32_t)info.minimum_free_bytes, (uint32_t)(info.total_free_bytes + info.total_allocated_bytes));
    console_reply("%s", line);
}

// "mem blocks [internal|dma|psram] [min bytes]": the allocated blocks of a heap, the ones of at least min bytes listed
// (#93: who holds the internal RAM). Collected first, printed after: the walk holds the heap's lock.
#define MAX_LISTED 48

typedef struct {
    size_t min;
    unsigned used_n, used_bytes, big_n, big_bytes, listed;
    struct {
        void  *ptr;
        size_t size;
    } big[MAX_LISTED];
} blocks_ctx_t;

static bool block_cb(walker_heap_into_t heap, walker_block_info_t b, void *user) {
    (void)heap;
    blocks_ctx_t *c = user;
    if (!b.used) {
        return true;
    }
    c->used_n++;
    c->used_bytes += (unsigned)b.size;
    if (b.size >= c->min) {
        c->big_n++;
        c->big_bytes += (unsigned)b.size;
        if (c->listed < MAX_LISTED) {
            c->big[c->listed].ptr  = b.ptr;
            c->big[c->listed].size = b.size;
            c->listed++;
        }
    }
    return true;
}

static void mem_blocks(char const *args) {
    uint32_t    caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    char const *name = "internal";
    if (strncmp(args, "psram", 5) == 0) {
        caps = MALLOC_CAP_SPIRAM;
        name = "psram";
    } else if (strncmp(args, "dma", 3) == 0) {
        caps = MALLOC_CAP_DMA;
        name = "dma";
    }
    char const *num = strpbrk(args, "0123456789");
    static blocks_ctx_t ctx; // 400 B: not on the console task's stack
    memset(&ctx, 0, sizeof(ctx));
    ctx.min = num ? (size_t)atoi(num) : 1024;
    heap_caps_walk(caps, block_cb, &ctx);
    for (unsigned i = 0; i < ctx.listed; i++) {
        console_reply("mem block %p %u B", ctx.big[i].ptr, (unsigned)ctx.big[i].size);
    }
    console_reply("mem %s blocks: %u allocated = %u B; %u of them >= %u B = %u B%s", name, ctx.used_n, ctx.used_bytes, ctx.big_n,
                  (unsigned)ctx.min, ctx.big_bytes, ctx.big_n > ctx.listed ? " (first ones listed)" : "");
}

static void (*mem_report)(void);
static void (*mem_window)(char const *args);

void console_set_mem_report(void (*report)(void)) {
    mem_report = report;
}

void console_set_mem_window(void (*window)(char const *args)) {
    mem_window = window;
}

// "mem blocks ..." and "mem window ...": true when args was one of them (and it ran)
static bool mem_subcommand(char const *args) {
    if (strncmp(args, "blocks", 6) == 0) {
        mem_blocks(args + 6 + (args[6] == ' '));
        return true;
    }
    if (strncmp(args, "window", 6) != 0 || (args[6] != '\0' && args[6] != ' ')) {
        return false;
    }
    if (mem_window) {
        mem_window(args + 6 + (args[6] == ' '));
    } else {
        console_reply("mem window: not in this build"); // lichen_os registers it only with LICHEN_DEV_COMMANDS
    }
    return true;
}

// "mem window" is registered by lichen_os only with LICHEN_DEV_COMMANDS (#410), so a release image's usage text
// neither lists it nor holds the words "developer mode" (#437)
#ifdef CONFIG_LICHEN_DEV_COMMANDS
#define MEM_USAGE \
    "mem [blocks [internal|dma|psram] [min bytes] | window [reset]] (heaps, per-app use, task stacks, per-task core and CPU %; blocks: who holds a heap; window: the lows since the last reset, developer mode, sampled every 250 ms; shorter dips above the since-boot low can be missed, #410)"
#else
#define MEM_USAGE "mem [blocks [internal|dma|psram] [min bytes]] (heaps, per-app use, task stacks, per-task core and CPU %; blocks: who holds a heap)"
#endif

static void cmd_mem(char const *args) {
    if (mem_subcommand(args)) {
        return;
    }
    print_heap("internal", MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    print_heap("dma", MALLOC_CAP_DMA);
    print_heap("psram", MALLOC_CAP_SPIRAM);
    if (mem_report) {
        mem_report();
    }

    UBaseType_t   n     = uxTaskGetNumberOfTasks();
    TaskStatus_t *tasks = malloc(n * sizeof(TaskStatus_t));
    if (!tasks) {
        console_reply("mem: out of memory for the task list");
        return;
    }
    configRUN_TIME_COUNTER_TYPE total = 0; // 64-bit since #125: the 32-bit µs counter wrapped after 71.6 min
    n                                 = uxTaskGetSystemState(tasks, n, &total);

    // CPU % is over the interval since the previous "fw mem" (since boot on the first call). Tasks are matched by
    // handle over every task; a task the last call did not see ran only since then, so its counter is its use.
    static struct {
        TaskHandle_t                handle;
        configRUN_TIME_COUNTER_TYPE runtime;
    } prev[MAX_TASKS_SEEN];
    static configRUN_TIME_COUNTER_TYPE prev_total;
    static bool                        have_prev;
    configRUN_TIME_COUNTER_TYPE        span = total - prev_total;
    console_reply("cpu over %s (%llu us)", have_prev ? "the interval since the last fw mem" : "the time since boot",
                  (unsigned long long)span);

    for (UBaseType_t i = 0; i < n; i++) {
        configRUN_TIME_COUNTER_TYPE before = 0;
        for (int j = 0; have_prev && j < MAX_TASKS_SEEN; j++) {
            if (prev[j].handle == tasks[i].xHandle) {
                before = prev[j].runtime;
                break;
            }
        }
        configRUN_TIME_COUNTER_TYPE used = tasks[i].ulRunTimeCounter - before;
        unsigned pct = span ? (unsigned)(((uint64_t)used * 1000 + span / 2) / span) : 0; // tenths of a percent
        if (pct > 1000) {
            pct = 1000; // a sample taken a moment after the total can run slightly ahead of it
        }
        char core[4]; // 0 or 1 on this two-core chip
        if (tasks[i].xCoreID == tskNO_AFFINITY) {
            strcpy(core, "-");
        } else {
            snprintf(core, sizeof(core), "%u", (unsigned)tasks[i].xCoreID & 1u);
        }
        console_reply(
            "task %s core %s stack_free %u cpu %u.%u%%",
            tasks[i].pcTaskName,
            core,
            (unsigned)tasks[i].usStackHighWaterMark,
            pct / 10,
            pct % 10
        );
    }
    for (int j = 0; j < MAX_TASKS_SEEN; j++) {
        prev[j].handle  = j < (int)n ? tasks[j].xHandle : NULL;
        prev[j].runtime = j < (int)n ? tasks[j].ulRunTimeCounter : 0;
    }
    if (n > MAX_TASKS_SEEN) {
        console_reply("mem: %u tasks, only the first %d are remembered for the next cpu interval", (unsigned)n,
                      MAX_TASKS_SEEN);
    }
    prev_total = total;
    have_prev  = true;
    free(tasks);
}

static fw_slot_state_fn _Atomic s_slot_state; // ota_slot_state_name, bound by main (#285)

void console_bind_slot_state(fw_slot_state_fn state) {
    atomic_store(&s_slot_state, state);
}

static fw_slot_switch_fn _Atomic s_slot_switch; // ota_image_note_slot_switch, bound by main (#411)

void console_bind_slot_switch(fw_slot_switch_fn note) {
    atomic_store(&s_slot_switch, note);
}

static char const *slot_state_name(esp_partition_t const *p) {
    fw_slot_state_fn state = atomic_load(&s_slot_state);
    return state ? state(p) : "unknown";
}

static void cmd_assets(char const *args) {
    (void)args;
    console_reply("assets %s", assets_status());
}

static void cmd_slot(char const *args) {
    esp_partition_t const *running = esp_ota_get_running_partition();
    esp_partition_t const *other   = esp_ota_get_next_update_partition(NULL);
    if (!*args) {
        esp_app_desc_t desc;
        bool           has_app = other && esp_ota_get_partition_description(other, &desc) == ESP_OK;
        console_reply("slot running %s (%s)", running->label, slot_state_name(running));
        console_reply(
            "slot other %s (%s) %s",
            other ? other->label : "none",
            slot_state_name(other),
            has_app ? desc.version : "no app"
        );
        return;
    }
    if (strcmp(args, "other") != 0) {
        console_reply("slot [other]");
        return; // "other" refuses while the USB hold is on: its row in console_os_rules.h, checked by run_command
    }
    esp_app_desc_t desc;
    if (!other || esp_ota_get_partition_description(other, &desc) != ESP_OK) {
        console_reply("slot: the other slot holds no app");
        return;
    }
    esp_err_t err = esp_ota_set_boot_partition(other);
    if (err != ESP_OK) {
        console_reply("slot: can't boot %s: %s", other->label, esp_err_to_name(err));
        return;
    }
    fw_slot_switch_fn note = atomic_load(&s_slot_switch);
    if (note) {
        note(other);
    }
    console_reply("booting %s (%s)", other->label, desc.version);
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(200));
    system_restart_asked("slot switch");
}

static void capture_finish(void) {
    if (capture.task && !capture.done_called) {
        capture.done_called = true;
        capture.done(capture.ctx, capture.out.buf, capture.out.len);
    }
}

static fw_get_blob_fn s_log_get; // the settings service (#235); NULL until lichen_os binds it
static fw_set_blob_fn s_log_set;

void system_restart_bind_store(fw_get_blob_fn get, fw_set_blob_fn set) {
    s_log_get = get;
    s_log_set = set;
}

static char const *s_restart_why; // set by system_restart; NULL when something else called esp_restart (#41)
static bool        s_restart_recorded;

int system_restart_history(restart_log_t *out) {
    return restart_log_load(s_log_get, out);
}

// NVS, not RTC_NOINIT: LP RAM does survive a software restart and a panic, but #330 measured it lost at the EN reset
// and a power-off, which the history must outlive. One small blob write, no flash mapping
static void push_restart_line(char const *why, uint32_t caller) {
    char     when[24];
    char    *when_p = NULL;
    time_t   now    = time(NULL);
    struct tm tm;
    if (now > 1735689600 && gmtime_r(&now, &tm) && strftime(when, sizeof(when), "%Y-%m-%dT%H:%M:%SZ", &tm)) {
        when_p = when;
    }
    char line[RESTART_LINE_MAX];
    restart_line_format(line, sizeof(line), when_p, esp_app_get_description()->version, (uint32_t)(esp_timer_get_time() / 1000000),
                        why, caller);
    ESP_LOGI(TAG, "restart: %s", line);
    restart_log_t *log = malloc(sizeof(*log));
    if (!log) {
        return;
    }
    system_restart_history(log);
    restart_log_push(log, line);
    restart_log_store(s_log_set, log);
    free(log);
}

static void record_restart(char const *why, uint32_t caller) {
    if (s_restart_recorded) {
        return;
    }
    s_restart_recorded = true;
    push_restart_line(why, caller);
}

static char const *_Atomic s_next_name; // names the next bare esp_restart in the history (#354)

void system_restart_name_next(char const *why) {
    atomic_store(&s_next_name, why);
}

void system_restart_note(char const *why) {
    push_restart_line(why, 0); // not the restart guard: the real restart of this boot is still recorded
}

// Every esp_restart() in the image ends up here (linker --wrap), including ones from IDF components and apps that
// skip system_restart: those are recorded as "unknown caller" with the address they were called from
extern void __real_esp_restart(void) __attribute__((noreturn));
void        __wrap_esp_restart(void) __attribute__((noreturn));

static void (*restart_prepare)(void);

void system_restart_set_prepare(void (*prepare)(void)) {
    restart_prepare = prepare;
}

// Set once a power off is asked (#281): the restart's last step switches the badge off instead
static void (*_Atomic s_power_off)(void);

void system_power_off_requested(void (*off)(void)) {
    atomic_store(&s_power_off, off);
}

// Every restart's last step, after the saves (system_restart) or at once (a bare esp_restart). Taken out first, so a
// restart from inside off restarts.
static void power_off_if_requested(void) {
    void (*off)(void) = atomic_load(&s_power_off);
    if (off && atomic_exchange(&s_power_off, NULL) == off) {
        ESP_LOGW(TAG, "restart (%s) after a power off was asked: powering off instead",
                 s_restart_why ? s_restart_why : "unknown caller");
        off(); // does not return
    }
}

// The trial rule main binds (#304): fw_console asks, ota_valid.c decides
static _Atomic system_restart_gate_fn s_trial_gate;

void system_restart_set_trial_gate(system_restart_gate_fn gate) {
    atomic_store(&s_trial_gate, gate);
}

static system_restart_gate_t trial_gate(bool asked, bool known_only, char const **note) {
    system_restart_gate_fn gate = atomic_load(&s_trial_gate);
    *note                       = "";
    return gate ? gate(asked, atomic_load(&s_power_off) != NULL, known_only, note) : SYSTEM_RESTART_GO;
}

bool system_restart_would_wait(void) {
    char const *note;
    return trial_gate(false, false, &note) == SYSTEM_RESTART_WAIT;
}

bool system_restart_on_trial(void) {
    char const *note;
    return trial_gate(true, false, &note) == SYSTEM_RESTART_ON_TRIAL;
}

void __wrap_esp_restart(void) {
    power_off_if_requested();
    bool deliberate = s_restart_why != NULL;
    if (deliberate) {
        record_restart(s_restart_why, 0);
    } else {
        // Only a word: fault paths and IDF components restart here and must not wait (#304), nor read flash
        char const *note;
        if (trial_gate(true, true, &note) == SYSTEM_RESTART_ON_TRIAL) {
            ESP_LOGW(TAG, "restart (unknown caller %p) on trial: %s", __builtin_return_address(0),
                     SYSTEM_RESTART_TRIAL_NOTE);
        }
        char const *name = atomic_load(&s_next_name);
        record_restart(name ? name : "unknown caller", (uint32_t)(uintptr_t)__builtin_return_address(0));
    }
    // After the record: the frame can only be skipped, never cost the history. A bare esp_restart() (no system_restart)
    // is how IDF components and fault handlers restart, so it gets no frame: the UI may be the problem (#39)
    if (restart_prepare && deliberate) {
        void (*prepare)(void) = restart_prepare;
        restart_prepare       = NULL; // a restart from inside the hook must not draw twice
        prepare();
    }
    __real_esp_restart();
}

// Refuses every caller (#84 T9): erasing NVS takes the Wi-Fi credentials, the clock record and every app's settings with
// it. The OS erases by partition where it means to (main.c on an unusable layout, factory_reset.c for #66); MeshCore's
// DataStore is built with nvs_flash_erase renamed to its own wipe. The caller is named by its return address.
esp_err_t __wrap_nvs_flash_erase(void);
esp_err_t __wrap_nvs_flash_erase(void) {
    ESP_LOGE("nvs", "nvs_flash_erase refused: only the OS erases NVS (factory reset), caller %p",
             __builtin_return_address(0));
    return ESP_ERR_NOT_ALLOWED;
}

// Refuses every caller but main.c (NVS unusable at boot) and factory_reset.c (#66), which call __real_ (#123): erasing
// the partition takes the same with it as nvs_flash_erase. The caller is named by its return address.
esp_err_t __wrap_nvs_flash_erase_partition(char const *part_name);
esp_err_t __wrap_nvs_flash_erase_partition(char const *part_name) {
    ESP_LOGE("nvs", "nvs_flash_erase_partition(%s) refused: only the OS erases NVS (factory reset), caller %p",
             part_name ? part_name : "?", __builtin_return_address(0));
    return ESP_ERR_NOT_ALLOWED;
}

// The apps' saves before a deliberate restart (#123): MeshCore's files and its message journal used to be cut off by
// ota-reboot and fw reboot. On a task of their own, so a save that hangs (a C6 that is gone, a stuck card) costs the
// restart SYSTEM_RESTART_SAVE_MS at most, and the restarting task holds nothing the saves wait for while it waits.
#define RESTART_SAVE_STACK 6144

static void (*restart_save)(void);
static atomic_bool       restart_saving;
static StaticSemaphore_t restart_saved_buf;
static SemaphoreHandle_t restart_saved;

void system_restart_set_save(void (*save)(void)) {
    restart_saved = xSemaphoreCreateBinaryStatic(&restart_saved_buf);
    restart_save  = save;
}

static void restart_save_task(void *arg) {
    (void)arg;
    restart_save();
    xSemaphoreGive(restart_saved);
    vTaskDelete(NULL);
}

static void restart_save_run(void) {
    switch (restart_save_plan(restart_save != NULL, atomic_exchange(&restart_saving, true))) {
    case RESTART_SAVE_NONE:
        return;
    case RESTART_SAVE_WAIT:
        // The first restart is saving and goes down after at most its bound; this one only goes if that one hangs
        vTaskDelay(pdMS_TO_TICKS(SYSTEM_RESTART_SAVE_MS + 1000));
        return;
    case RESTART_SAVE_RUN:
        break;
    }
    int64_t started = esp_timer_get_time();
    if (xTaskCreate(restart_save_task, "rstsave", RESTART_SAVE_STACK, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "restart: no memory for the save task, restarting without the apps' saves");
        return;
    }
    if (xSemaphoreTake(restart_saved, pdMS_TO_TICKS(SYSTEM_RESTART_SAVE_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "restart: the apps' saves did not finish in %d ms, restarting anyway", SYSTEM_RESTART_SAVE_MS);
        return;
    }
    ESP_LOGI(TAG, "restart: the apps saved in %lld ms", (long long)((esp_timer_get_time() - started) / 1000));
}

// Before the restart's reason is recorded, so a person's restart during the wait wins (#304). The wait ends: the gate
// says when (the valid mark, the trial loop's end, its bound, a power off; ota_valid.c restart_gate_decide).
static void trial_gate_wait(char const *why, bool asked) {
    bool said = false;
    for (;;) {
        char const *note;
        switch (trial_gate(asked, false, &note)) {
        case SYSTEM_RESTART_GO:
            if (said) {
                ESP_LOGI(TAG, "restart (%s): this version is valid now, restarting", why);
            }
            return;
        case SYSTEM_RESTART_ON_TRIAL:
            ESP_LOGW(TAG, "restart (%s, %s): %s", why, note, SYSTEM_RESTART_TRIAL_NOTE);
            return;
        case SYSTEM_RESTART_WAIT:
        default:
            if (!said) {
                ESP_LOGW(TAG, "restart (%s) waits: this version is still on trial", why);
                said = true;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            break;
        }
    }
}

static void __attribute__((noreturn)) restart_now(char const *why) {
    // The first restart's reason stays: a second one only waits for it (restart_save_run)
    char const *none = NULL;
    __atomic_compare_exchange_n(&s_restart_why, &none, why, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    if (capture.task && capture.task == xTaskGetCurrentTaskHandle()) {
        capture_finish(); // a command from Wi-Fi answers before the badge goes down (#38)
        vTaskDelay(pdMS_TO_TICKS(300)); // the reply leaves the TCP stack
    }
    restart_save_run();
    esp_restart();
}

void system_restart(char const *why) {
    why = why ? why : "unspecified";
    trial_gate_wait(why, false);
    restart_now(why);
}

void system_restart_asked(char const *why) {
    why = why ? why : "unspecified";
    trial_gate_wait(why, true);
    restart_now(why);
}

void system_save_before_off(void) {
    restart_save_run();
}

static void cmd_reboot(char const *args) {
    (void)args;
    if (system_restart_on_trial()) {
        console_reply("rebooting: %s", SYSTEM_RESTART_TRIAL_NOTE);
    } else {
        console_reply("rebooting");
    }
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(200));
    system_restart_asked("console reboot");
}

static void (*_Atomic activity_hook)(void);

void console_set_activity(void (*activity)(void)) {
    atomic_store(&activity_hook, activity);
}

static void activity(void) {
    void (*fn)(void) = atomic_load(&activity_hook);
    if (fn) {
        fn();
    }
}

static char const *(*_Atomic usb_hold)(void);

void console_usb_hold_bind(char const *(*held)(void)) {
    atomic_store(&usb_hold, held);
}

char const *console_usb_hold_why(void) {
    char const *(*held)(void) = atomic_load(&usb_hold);
    return held ? held() : NULL;
}

static char const *(*_Atomic boot_pending)(void);

void console_bind_boot_pending(char const *(*pending)(void)) {
    atomic_store(&boot_pending, pending);
}

char const *console_boot_pending(void) {
    char const *(*pending)(void) = atomic_load(&boot_pending);
    return pending ? pending() : NULL;
}

bool console_usb_hold_ok(char const *what) {
    char const *why = console_usb_hold_why();
    if (!console_hold_refuses(why != NULL, console_origin())) {
        return true;
    }
    console_reply("%s refused: %s is on, which it would erase; only the USB console can do that", what, why);
    return false;
}

bool console_erase_ok(char const *what) {
    if (!console_erase_refuses(console_origin())) {
        return true;
    }
    console_reply("%s refused: it erases storage, so only the USB console can do it (the screen's Settings can too)", what);
    return false;
}

static void run_command(char *line);

// Every command line, from the UART and from Wi-Fi
static void run(char *line) {
    activity();
    run_command(line);
    activity();
}

// The refusals of a sub-command row (#464): "<command> <word>" names it, a REST row names the command alone
static void rule_refused(console_rule_t rule, char const *name, console_sub_t const *row) {
    char   what[48];
    size_t wl = row->word ? strlen(row->word) : 0;
    wl -= wl && row->word[wl - 1] == ' '; // a PREFIX word's own space
    snprintf(what, sizeof(what), "%s%s%.*s", name, wl ? " " : "", (int)wl, wl ? row->word : "");
    switch (rule) {
        case CON_RULE_DEV: console_reply("%s: " DEV_OFF_REPLY, what); break;
        case CON_RULE_HOLD: console_reply("%s refused: %s is on; " HOLD_OFF_FIRST, what, console_usb_hold_why() ? console_usb_hold_why() : "?"); break;
        case CON_RULE_ORIGIN:
        default:
            console_reply("%s refused: %s", what,
                          row->origin == CON_ORIGIN_LOCAL ? "only on the badge (the USB console or the Console app)"
                          : row->flags & CON_F_ERASE ? "it erases storage, so only the USB console can do it (the "
                                                        "screen's Settings can too)"
                                                      : "only from the USB console");
            break;
    }
}

// The one gate and origin check of a command with a sub-command table, before its own code runs
static bool rules_allow(console_entry_t const *e, char const *name, char const *args) {
    if (!e->rules) {
        return true;
    }
    console_sub_t const *row = console_sub_find(rule_tables[e->rules - 1], args);
    if (!row) {
        return true;
    }
    console_ask_t ask = {
        .dev_on = console_dev_mode(),
        .origin = console_origin(),
        .held   = (row->flags & CON_F_USB_HOLD) && console_usb_hold_why() != NULL,
    };
    console_rule_t rule = console_sub_rule(row, &ask);
    if (rule == CON_RULE_RUN) {
        return true;
    }
    rule_refused(rule, name, row);
    return false;
}

static void run_command(char *line) {
    // "fw <app> <command>" (#423): the same table, the same gates; only the command's owner may answer under a prefix
    char const *args;
    int         app;
    line = dev_gate_split(line, (char const *const *)app_ids, __atomic_load_n(&n_apps, __ATOMIC_ACQUIRE), &app, &args);
    int        i = 0;
    dev_find_t found = dev_mode_find_app(commands, commands_count(), line, console_dev_mode(), app, &i);
    // One decision for all three outcomes (host-tested), then the form's row: a test arm of an otherwise open command
    // (#421) is refused before it can write anything
    switch (dev_gate_decide(found)) { // then the form's own row (rules_allow): developer mode, origin, the USB hold
        case DEV_GATE_RUN:
            if (rules_allow(&commands[i], line, args)) {
                commands[i].fn(args);
            }
            return;
        case DEV_GATE_REFUSE: console_reply("%s: " DEV_OFF_REPLY, line); return;
        case DEV_GATE_UNKNOWN: break;
    }
    // Not echoed: a mistyped line may be a password, and the reply goes to the log store (#123); the asker sees what
    // they typed on their own side
    console_reply("unknown command, try 'fw help'");
}

console_origin_t console_origin(void) {
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    return dev_mode_origin(capture.task == self, capture.origin, atomic_load(&s_uart_task) == self);
}

bool console_from_uart(void) {
    return console_origin() == CONSOLE_ORIGIN_UART;
}

// keep_secret is the Wi-Fi console (console_exec_with_secrets, POST /console); without it, the Console app
static void exec(char const *line, char *buf, size_t cap, bool keep_secret, console_done_fn done, void *ctx) {
    while (*line == ' ') {
        line++;
    }
    if (strncmp(line, "fw ", 3) == 0) {
        line += 3;
    }
    // A Wi-Fi line may be up to CONSOLE_WIFI_CMD_MAX (#110): a copy on the heap, not on the caller's stack
    size_t len = strnlen(line, CONSOLE_WIFI_CMD_MAX - 1);
    char  *cmd = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!cmd) {
        static char const no_mem[] = "out of memory, try again\n";
        done(ctx, no_mem, sizeof(no_mem) - 1);
        return;
    }
    memcpy(cmd, line, len); // run() splits it in place
    cmd[len] = '\0';
    // No mutex yet: console_start has not run, and without it the run would not be serialised against the UART
    if (!run_mutex || xSemaphoreTake(run_mutex, pdMS_TO_TICKS(RUN_WAIT_MS)) != pdTRUE) {
        static char const busy[] = "busy, try again\n";
        secure_wipe(cmd, len + 1); // every free path: the line may hold a password (#123)
        free(cmd);
        done(ctx, busy, sizeof(busy) - 1);
        return;
    }
    capture = (typeof(capture)){
        .task   = xTaskGetCurrentTaskHandle(),
        .out    = {.buf = buf, .cap = cap, .keep_secret = keep_secret},
        .done   = done,
        .ctx    = ctx,
        .origin = dev_mode_exec_origin(keep_secret),
    };
    if (cmd[0]) {
        run(cmd);
    }
    secure_wipe(cmd, len + 1); // run() split it in place; a line may hold a password (#123). There is no history.
    free(cmd);
    capture_finish();
    capture.task = NULL;
    xSemaphoreGive(run_mutex);
}

void console_exec(char const *line, char *buf, size_t cap, console_done_fn done, void *ctx) {
    exec(line, buf, cap, false, done, ctx);
}

void console_exec_with_secrets(char const *line, char *buf, size_t cap, console_done_fn done, void *ctx) {
    exec(line, buf, cap, true, done, ctx);
}

#ifdef CONFIG_LICHEN_DEV_COMMANDS
// "fw dev [on|off|status]" (#373): on only from the USB console, never over Wi-Fi (the OTA token is not enough)
static void cmd_dev(char const *args) {
    switch (dev_mode_request(args)) { // "on" is the USB console's only: its row, checked by run_command (#464)
        case DEV_REQ_STATUS: console_reply("developer mode %s", console_dev_mode() ? "on" : "off"); break;
        case DEV_REQ_ON:
        case DEV_REQ_OFF: {
            bool on    = strcmp(args, "on") == 0;
            bool saved = console_dev_set(on);
            console_reply("developer mode %s%s", on ? "on" : "off", saved ? "" : " (not saved: this boot only)");
            break;
        }
        case DEV_REQ_USAGE:
        default: console_reply("dev [on|off|status]"); break;
    }
}
#endif

static void console_task(void *arg) {
    (void)arg;
    atomic_store(&s_uart_task, xTaskGetCurrentTaskHandle());
    console_cmd_t state;
    console_cmd_init(&state);
    for (;;) {
        uint8_t     c;
        char const *pass;
        size_t      pass_len;
        // Held prefix bytes are dropped after 2 s without a key (#5): 100 ms was too short for "fw ..." typed by hand,
        // and never dropping them let a stray "f" eat the next scripted command (there is no app stdin to hand them to)
        if (uart_read_bytes(UART_NUM_0, &c, 1, pdMS_TO_TICKS(2000)) != 1) {
            console_cmd_flush(&state, &pass, &pass_len);
            continue;
        }
        switch (console_cmd_feed(&state, (char)c, &pass, &pass_len)) {
            case CONSOLE_CMD_READY:
                if (!run_mutex || xSemaphoreTake(run_mutex, pdMS_TO_TICKS(RUN_WAIT_MS)) == pdTRUE) {
                    run(state.buf);
                    secure_wipe(state.buf, sizeof(state.buf)); // the splitter reset len; the text is not kept
                    if (run_mutex) {
                        xSemaphoreGive(run_mutex);
                    }
                } else {
                    console_reply("busy, try again");
                }
                break;
            case CONSOLE_CMD_TOO_LONG: console_reply("command too long"); break;
            default: break;
        }
    }
}

void console_start(void) {
    run_mutex   = xSemaphoreCreateMutex();
    reply_mutex = xSemaphoreCreateMutex();
    console_register("help", "help [<command> | <app> | all]", cmd_help);
    console_register_rules("mem", MEM_USAGE, cmd_mem, OS_MEM_RULES);
    console_register("assets", "assets (state of the assets partition and its files)", cmd_assets);
    console_register_rules("slot", "slot [other] (show the app slots, or boot the other one)", cmd_slot, OS_SLOT_RULES);
    console_register_rules("reboot", "reboot", cmd_reboot, OS_REBOOT_RULES);
#ifdef CONFIG_LICHEN_DEV_COMMANDS
    console_register_rules("dev", "dev [on|off|status] (developer mode: debug and test commands; on only from USB)", cmd_dev,
                           OS_DEV_RULES);
#endif

    if (uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0) != ESP_OK) {
        ESP_LOGE(TAG, "UART0 driver install failed, no console");
        return;
    }
    if (xTaskCreatePinnedToCore(console_task, "console", 4096, NULL, 5, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "console task failed to start");
    }
}
