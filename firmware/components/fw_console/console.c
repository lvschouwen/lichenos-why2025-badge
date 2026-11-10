#include "secure_wipe.h"
#include "fw_console.h"
#include "fw_slot.h"
#include "mem_fmt.h"

#include "assets.h"
#include "console_capture.h"
#include "console_cmd.h"
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
#define MAX_TASKS_SEEN 48

static char const TAG[] = "console";

typedef struct {
    char const  *name;
    char const  *usage;
    console_fn_t fn;
} command_t;

static command_t commands[MAX_COMMANDS];
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
} capture;

// One command runs at a time, from the UART or from Wi-Fi: commands share state (the capture, the commands' statics)
static SemaphoreHandle_t run_mutex;
#define RUN_WAIT_MS 2000
// reply_v's formatting buffer is static (384 B off the 4 KB console and c6link task stacks), guarded by its own mutex
static SemaphoreHandle_t reply_mutex;
static char              reply_text[REPLY_MAX];

// Where a line goes is console_route's decision (console_capture.c, host-tested): a secret line (#123) reaches only the
// one who asked, never the tap (the log store is read over Wi-Fi and on the screen later). Its formatted copy is wiped.
static void reply_v(bool secret, char const *fmt, va_list ap) {
    va_list copy;
    va_copy(copy, ap);
    console_route_t to = console_route(secret, capture.task && capture.task == xTaskGetCurrentTaskHandle());
    if (to.uart) {
        printf("fw: ");
        vprintf(fmt, ap);
        printf("\n");
    } else {
        printf("fw: " CONSOLE_SECRET_UART "\n");
    }
    if (secret) {
        fflush(stdout); // out of stdio's buffer now rather than whenever the next line comes
    }
    if ((to.tap || to.capture) && reply_mutex && xSemaphoreTake(reply_mutex, pdMS_TO_TICKS(RUN_WAIT_MS)) == pdTRUE) {
        char *text = reply_text;
        int   n    = vsnprintf(text, REPLY_MAX, fmt, copy);
        if (n > 0) {
            size_t len = (size_t)n < REPLY_MAX ? (size_t)n : REPLY_MAX - 1;
            if (tap && to.tap) {
                text[len] = '\n';
                tap(text, len + 1);
            }
            if (to.capture) {
                console_capture_add(&capture.out, text, len, secret);
            }
        }
        if (secret) {
            secure_wipe(reply_text, sizeof(reply_text));
        }
        xSemaphoreGive(reply_mutex);
    }
    va_end(copy);
}

void console_reply(char const *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    reply_v(false, fmt, ap);
    va_end(ap);
}

void console_reply_secret(char const *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    reply_v(true, fmt, ap);
    va_end(ap);
}

// Components register from their own tasks at boot while the console task may already run a command (#5): writers
// take commands_lock, and an entry is complete before the count that makes it visible is published. Entries are never
// changed or removed, so readers need only the count (commands_count) and no lock.
static portMUX_TYPE commands_lock = portMUX_INITIALIZER_UNLOCKED;

static int commands_count(void) {
    return __atomic_load_n(&n_commands, __ATOMIC_ACQUIRE);
}

void console_register(char const *name, char const *usage, console_fn_t fn) {
    portENTER_CRITICAL(&commands_lock);
    int  n    = n_commands;
    bool room = n < MAX_COMMANDS;
    if (room) {
        commands[n] = (command_t){name, usage, fn};
        __atomic_store_n(&n_commands, n + 1, __ATOMIC_RELEASE);
    }
    portEXIT_CRITICAL(&commands_lock);
    if (!room) {
        ESP_LOGE(TAG, "no room for command %s (table of %d is full, raise MAX_COMMANDS)", name, MAX_COMMANDS);
    }
}

static void cmd_help(char const *args) {
    (void)args;
    int n = commands_count();
    for (int i = 0; i < n; i++) {
        console_reply("%s", commands[i].usage);
    }
    console_reply("%d of %d commands registered", n, MAX_COMMANDS);
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

void console_set_mem_report(void (*report)(void)) {
    mem_report = report;
}

static void cmd_mem(char const *args) {
    if (strncmp(args, "blocks", 6) == 0) {
        mem_blocks(args + 6 + (args[6] == ' '));
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
        return;
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

// NVS, not RTC_NOINIT: that does not survive a software restart on the P4 (#4). One small blob write, no flash mapping
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
        record_restart("unknown caller", (uint32_t)(uintptr_t)__builtin_return_address(0));
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

static void run_command(char *line);

// Every command line, from the UART and from Wi-Fi
static void run(char *line) {
    activity();
    run_command(line);
    activity();
}

static void run_command(char *line) {
    char *args = strchr(line, ' ');
    if (args) {
        *args++ = '\0';
        while (*args == ' ') {
            args++;
        }
    } else {
        args = line + strlen(line);
    }
    int n = commands_count();
    for (int i = 0; i < n; i++) {
        if (strcmp(commands[i].name, line) == 0) {
            commands[i].fn(args);
            return;
        }
    }
    // Not echoed: a mistyped line may be a password, and the reply goes to the log store (#123); the asker sees what
    // they typed on their own side
    console_reply("unknown command, try 'fw help'");
}

bool console_from_uart(void) {
    return capture.task != xTaskGetCurrentTaskHandle();
}

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
    if (run_mutex && xSemaphoreTake(run_mutex, pdMS_TO_TICKS(RUN_WAIT_MS)) != pdTRUE) {
        static char const busy[] = "busy, try again\n";
        secure_wipe(cmd, len + 1); // every free path: the line may hold a password (#123)
        free(cmd);
        done(ctx, busy, sizeof(busy) - 1);
        return;
    }
    capture = (typeof(capture)){
        .task = xTaskGetCurrentTaskHandle(),
        .out  = {.buf = buf, .cap = cap, .keep_secret = keep_secret},
        .done = done,
        .ctx  = ctx,
    };
    if (cmd[0]) {
        run(cmd);
    }
    secure_wipe(cmd, len + 1); // run() split it in place; a line may hold a password (#123). There is no history.
    free(cmd);
    capture_finish();
    capture.task = NULL;
    if (run_mutex) {
        xSemaphoreGive(run_mutex);
    }
}

void console_exec(char const *line, char *buf, size_t cap, console_done_fn done, void *ctx) {
    exec(line, buf, cap, false, done, ctx);
}

void console_exec_with_secrets(char const *line, char *buf, size_t cap, console_done_fn done, void *ctx) {
    exec(line, buf, cap, true, done, ctx);
}

static void console_task(void *arg) {
    (void)arg;
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
    console_register("help", "help", cmd_help);
    console_register("mem", "mem [blocks [internal|dma|psram] [min bytes]] (heaps, per-app use, task stacks, per-task core and CPU %; blocks: who holds a heap)", cmd_mem);
    console_register("assets", "assets (state of the assets partition and its files)", cmd_assets);
    console_register("slot", "slot [other] (show the app slots, or boot the other one)", cmd_slot);
    console_register("reboot", "reboot", cmd_reboot);

    if (uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0) != ESP_OK) {
        ESP_LOGE(TAG, "UART0 driver install failed, no console");
        return;
    }
    if (xTaskCreatePinnedToCore(console_task, "console", 4096, NULL, 5, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "console task failed to start");
    }
}
