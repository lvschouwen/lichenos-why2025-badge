#include "fw_console.h"

#include "assets.h"
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
#include "nvs.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include <string.h>
#include <time.h>

#define MAX_COMMANDS   40
#define MAX_TASKS_SEEN 32

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
    TaskHandle_t    task;
    char           *buf;
    size_t          cap;
    size_t          len;
    bool            cut;
    bool            done_called;
    console_done_fn done;
    void           *ctx;
} capture;

#define CUT_NOTE "[output cut]\n"

static void capture_add(char const *text, size_t n) {
    if (capture.cut || capture.cap < sizeof(CUT_NOTE) + 2) {
        return;
    }
    size_t room = capture.cap - capture.len - sizeof(CUT_NOTE); // always keeps room for the note
    if (n + 1 > room) {
        memcpy(capture.buf + capture.len, CUT_NOTE, sizeof(CUT_NOTE) - 1);
        capture.len += sizeof(CUT_NOTE) - 1;
        capture.cut  = true;
        return;
    }
    memcpy(capture.buf + capture.len, text, n);
    capture.buf[capture.len + n] = '\n';
    capture.len                 += n + 1;
}

// One command runs at a time, from the UART or from Wi-Fi: commands share state (the capture, the commands' statics)
static SemaphoreHandle_t run_mutex;
#define RUN_WAIT_MS 2000
// reply_v's formatting buffer is static (384 B off the 4 KB console and c6link task stacks), guarded by its own mutex
static SemaphoreHandle_t reply_mutex;
static char              reply_text[REPLY_MAX];

static void reply_v(bool private, char const *fmt, va_list ap) {
    va_list copy;
    va_copy(copy, ap);
    printf("fw: ");
    vprintf(fmt, ap);
    printf("\n");
    if (!private && reply_mutex && xSemaphoreTake(reply_mutex, pdMS_TO_TICKS(RUN_WAIT_MS)) == pdTRUE) {
        char *text = reply_text;
        int   n    = vsnprintf(text, REPLY_MAX, fmt, copy);
        if (n > 0) {
            size_t len = (size_t)n < REPLY_MAX ? (size_t)n : REPLY_MAX - 1;
            if (tap) {
                text[len] = '\n';
                tap(text, len + 1);
            }
            if (capture.task && capture.task == xTaskGetCurrentTaskHandle()) {
                capture_add(text, len);
            }
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

void console_reply_private(char const *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    reply_v(true, fmt, ap);
    va_end(ap);
}

void console_register(char const *name, char const *usage, console_fn_t fn) {
    if (n_commands == MAX_COMMANDS) {
        ESP_LOGE(TAG, "no room for command %s", name);
        return;
    }
    commands[n_commands++] = (command_t){name, usage, fn};
}

static void cmd_help(char const *args) {
    (void)args;
    for (int i = 0; i < n_commands; i++) {
        console_reply("%s", commands[i].usage);
    }
}

static void print_heap(char const *name, uint32_t caps) {
    multi_heap_info_t info;
    heap_caps_get_info(&info, caps);
    console_reply(
        "mem %s free %u largest %u min %u",
        name,
        (unsigned)info.total_free_bytes,
        (unsigned)info.largest_free_block,
        (unsigned)info.minimum_free_bytes
    );
}

static void cmd_mem(char const *args) {
    (void)args;
    print_heap("internal", MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    print_heap("dma", MALLOC_CAP_DMA);
    print_heap("psram", MALLOC_CAP_SPIRAM);

    UBaseType_t   n     = uxTaskGetNumberOfTasks();
    TaskStatus_t *tasks = malloc(n * sizeof(TaskStatus_t));
    if (!tasks) {
        console_reply("mem: out of memory for the task list");
        return;
    }
    uint32_t total = 0;
    n              = uxTaskGetSystemState(tasks, n, &total);

    // CPU % is over the interval since the previous "fw mem" (since boot on the first call)
    static struct {
        TaskHandle_t handle;
        uint32_t     runtime;
    } prev[MAX_TASKS_SEEN];
    static uint32_t prev_total;
    static bool     have_prev;
    uint32_t        span = total - prev_total; // unsigned: survives the counter wrapping
    console_reply("cpu over %s (%u us)", have_prev ? "the interval since the last fw mem" : "the time since boot", (unsigned)span);

    uint32_t next_runtime[MAX_TASKS_SEEN] = {0};
    for (UBaseType_t i = 0; i < n; i++) {
        uint32_t before = 0;
        for (int j = 0; j < MAX_TASKS_SEEN; j++) {
            if (prev[j].handle == tasks[i].xHandle) {
                before = prev[j].runtime;
                break;
            }
        }
        uint32_t used = tasks[i].ulRunTimeCounter - (have_prev ? before : 0);
        unsigned pct  = span ? (unsigned)(((uint64_t)used * 1000 + span / 2) / span) : 0; // tenths of a percent
        char     core[4]; // 0 or 1 on this two-core chip
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
        if (i < MAX_TASKS_SEEN) {
            next_runtime[i] = tasks[i].ulRunTimeCounter;
        }
    }
    for (int j = 0; j < MAX_TASKS_SEEN; j++) {
        prev[j].handle  = j < (int)n ? tasks[j].xHandle : NULL;
        prev[j].runtime = next_runtime[j];
    }
    prev_total = total;
    have_prev  = true;
    free(tasks);
}

static char const *ota_state_name(esp_partition_t const *p) {
    esp_ota_img_states_t state;
    if (!p || esp_ota_get_state_partition(p, &state) != ESP_OK) {
        return "unknown";
    }
    switch (state) {
        case ESP_OTA_IMG_NEW: return "new";
        case ESP_OTA_IMG_PENDING_VERIFY: return "pending-verify";
        case ESP_OTA_IMG_VALID: return "valid";
        case ESP_OTA_IMG_INVALID: return "invalid";
        case ESP_OTA_IMG_ABORTED: return "aborted";
        default: return "undefined";
    }
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
        console_reply("slot running %s (%s)", running->label, ota_state_name(running));
        console_reply(
            "slot other %s (%s) %s",
            other ? other->label : "none",
            ota_state_name(other),
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
    system_restart("slot switch");
}

static void capture_finish(void) {
    if (capture.task && !capture.done_called) {
        capture.done_called = true;
        capture.done(capture.ctx, capture.buf, capture.len);
    }
}

#define RESTART_KEY "rst_log"

static char const *s_restart_why; // set by system_restart; NULL when something else called esp_restart (#41)
static bool        s_restart_recorded;

int system_restart_history(restart_log_t *out) {
    memset(out, 0, sizeof(*out));
    nvs_handle_t nvs;
    size_t       size = sizeof(*out);
    if (nvs_open(FW_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return 0;
    }
    esp_err_t err = nvs_get_blob(nvs, RESTART_KEY, out, &size);
    nvs_close(nvs);
    if (err != ESP_OK || size != sizeof(*out)) {
        memset(out, 0, sizeof(*out));
        return 0;
    }
    restart_log_sanitize(out);
    return (int)out->count;
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
    ESP_LOGW(TAG, "restart: %s", line);
    restart_log_t *log = malloc(sizeof(*log));
    if (!log) {
        return;
    }
    system_restart_history(log);
    restart_log_push(log, line);
    nvs_handle_t nvs;
    if (nvs_open(FW_NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_blob(nvs, RESTART_KEY, log, sizeof(*log));
        nvs_commit(nvs);
        nvs_close(nvs);
    }
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
void        __wrap_esp_restart(void) {
    if (s_restart_why) {
        record_restart(s_restart_why, 0);
    } else {
        record_restart("unknown caller", (uint32_t)(uintptr_t)__builtin_return_address(0));
    }
    __real_esp_restart();
}

void system_restart(char const *why) {
    s_restart_why = why ? why : "unspecified";
    if (capture.task && capture.task == xTaskGetCurrentTaskHandle()) {
        capture_finish(); // a command from Wi-Fi answers before the badge goes down (#38)
        vTaskDelay(pdMS_TO_TICKS(300)); // the reply leaves the TCP stack
    }
    esp_restart();
}

static void cmd_reboot(char const *args) {
    (void)args;
    console_reply("rebooting");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(200));
    system_restart("console reboot");
}

static void run(char *line) {
    char *args = strchr(line, ' ');
    if (args) {
        *args++ = '\0';
        while (*args == ' ') {
            args++;
        }
    } else {
        args = line + strlen(line);
    }
    for (int i = 0; i < n_commands; i++) {
        if (strcmp(commands[i].name, line) == 0) {
            commands[i].fn(args);
            return;
        }
    }
    console_reply("unknown command '%s', try 'fw help'", line);
}

void console_exec(char const *line, char *buf, size_t cap, console_done_fn done, void *ctx) {
    char cmd[CONSOLE_CMD_MAX];
    while (*line == ' ') {
        line++;
    }
    if (strncmp(line, "fw ", 3) == 0) {
        line += 3;
    }
    snprintf(cmd, sizeof(cmd), "%s", line); // run() splits it in place
    if (run_mutex && xSemaphoreTake(run_mutex, pdMS_TO_TICKS(RUN_WAIT_MS)) != pdTRUE) {
        static char const busy[] = "busy, try again\n";
        done(ctx, busy, sizeof(busy) - 1);
        return;
    }
    capture = (typeof(capture)){.task = xTaskGetCurrentTaskHandle(), .buf = buf, .cap = cap, .done = done, .ctx = ctx};
    if (cmd[0]) {
        run(cmd);
    }
    capture_finish();
    capture.task = NULL;
    if (run_mutex) {
        xSemaphoreGive(run_mutex);
    }
}

static void console_task(void *arg) {
    (void)arg;
    console_cmd_t state;
    console_cmd_init(&state);
    for (;;) {
        uint8_t     c;
        char const *pass;
        size_t      pass_len;
        if (uart_read_bytes(UART_NUM_0, &c, 1, pdMS_TO_TICKS(100)) != 1) {
            console_cmd_flush(&state, &pass, &pass_len); // there is no app stdin: held bytes are dropped
            continue;
        }
        switch (console_cmd_feed(&state, (char)c, &pass, &pass_len)) {
            case CONSOLE_CMD_READY:
                if (!run_mutex || xSemaphoreTake(run_mutex, pdMS_TO_TICKS(RUN_WAIT_MS)) == pdTRUE) {
                    run(state.buf);
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
    console_register("mem", "mem (heaps, task stacks, per-task core and CPU %)", cmd_mem);
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
