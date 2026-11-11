// Weather (#155): the BME690 (the sensors snapshot API, #69) as current values and a graph of the last hours. A small
// task on core 0 takes one sample a minute into a ring in PSRAM (no flash writes), so the graph keeps filling while
// the app is closed. The temperature offset and the CSV log switch are the app's settings; the log appends one line a
// minute to /sd/lichen/apps/weather/weather.csv and switches itself off, with a notification, when the card is missing
// or a write fails (no retries: the person turns it on again). Only OS APIs: sensors, settings, fs, notify, clock.
#include "weather_app.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lichen_app.h"
#include "lichen_clock.h"
#include "lichen_fs.h"
#include "lichen_notify.h"
#include "lichen_sensors.h"
#include "lichen_mem.h"
#include "lichen_settings.h"
#include "lichen_task.h"
#include "menu.h"
#include "lichen_apps.h"
#include "theme.h"

#include <errno.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>

static char const TAG[] = "weather";

#define SAMPLE_MS       60000
#define FIRST_SAMPLE_MS 20000 // the sensor needs a few seconds after boot
#define STALE_US        15000000 // an environment reading older than this is the stopped service's, not the room's
#define SD_WAIT_MS      2000
#define TASK_STACK      6144
#define KEY_OFFSET      "toff"
#define KEY_LOG         "csv"
#define LOG_FILE        "weather.csv"

static lichen_app_t const weather_app;
static lichen_ctx_t self_ctx; // lichen_register_app's answer, for the gate calls (#230 E1b)

static wx_ring_t         s_ring;
static SemaphoreHandle_t s_lock;
static atomic_uint       s_version;
static atomic_int        s_offset_half;
static atomic_bool       s_log_on;
static atomic_bool       s_stopping;
static atomic_bool       s_writing;

float weather_offset_k(void) {
    return (float)atomic_load(&s_offset_half) * 0.5f;
}

unsigned weather_version(void) {
    return atomic_load(&s_version);
}

size_t weather_series(wx_qty_t q, float *out, size_t max) {
    if (!s_lock) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t n = wx_series(&s_ring, q, weather_offset_k(), out, max);
    xSemaphoreGive(s_lock);
    return n;
}

// The snapshot as a sample; NaN for what the sensor did not deliver
static void sample_from(lichen_sensors_t const *sn, wx_sample_t *s) {
    for (int q = 0; q < WX_QTY; q++) {
        s->v[q] = NAN;
    }
    if (!sn->env_valid || esp_timer_get_time() - sn->env_us > STALE_US) {
        return;
    }
    s->v[WX_TEMP]  = sn->temperature_c;
    s->v[WX_HUM]   = sn->humidity_pct;
    s->v[WX_PRESS] = sn->pressure_hpa;
    if (sn->gas_valid) {
        s->v[WX_GAS] = sn->gas_kohm;
    }
}

bool weather_current(wx_sample_t *out) {
    lichen_sensors_t sn;
    lichen_sensors_get(&sn);
    sample_from(&sn, out);
    if (isnan(out->v[WX_TEMP])) {
        return false;
    }
    out->v[WX_TEMP] += weather_offset_k();
    return true;
}

typedef enum { LOG_OK, LOG_SKIP, LOG_FAIL } log_result_t;

// One CSV line, in one SD batch (the card cannot be unmounted under the open file). A card that is busy for a moment
// skips this minute; a missing card or a failed write is a failure.
static log_result_t log_append(int64_t unix_s, wx_sample_t const *s) {
    char const *dir = lichen_fs_sd_dir(self_ctx);
    if (!dir) {
        return LOG_FAIL;
    }
    char path[96];
    snprintf(path, sizeof(path), "%s/" LOG_FILE, dir);
    if (!lichen_fs_sd_begin(self_ctx, SD_WAIT_MS)) {
        return lichen_fs_sd_mounted() ? LOG_SKIP : LOG_FAIL;
    }
    char   line[128], header[96];
    size_t n = wx_csv_line(line, sizeof(line), unix_s, s, weather_offset_k());
    struct stat st;
    bool   fresh = stat(path, &st) != 0 || st.st_size == 0;
    FILE  *f     = fopen(path, "a");
    bool   ok    = f && n > 0;
    if (ok && fresh) {
        ok = wx_csv_header(header, sizeof(header)) > 0 && fputs(header, f) >= 0;
    }
    if (ok) {
        ok = fputs(line, f) >= 0;
    }
    if (f && fclose(f) != 0) {
        ok = false;
    }
    int err = ok ? 0 : (errno ? errno : EIO);
    lichen_fs_sd_end(self_ctx, err);
    if (!ok) {
        ESP_LOGW(TAG, "log write failed (errno %d)", err);
    }
    return ok ? LOG_OK : LOG_FAIL;
}

static void log_stop_with_notice(void) {
    atomic_store(&s_log_on, false);
    app_settings_set_u8(self_ctx, KEY_LOG, 0);
    ESP_LOGW(TAG, "SD log switched off");
    lichen_notify(self_ctx, LN_WARN, "Weather log stopped",
                  "The SD card is missing or not writable. Put it back, then turn the log on again in Settings > Weather.", 0);
}

static void log_sample(wx_sample_t const *s) {
    if (!atomic_load(&s_log_on) || !clock_is_set()) {
        return; // without a clock a line has no time
    }
    atomic_store(&s_writing, true);
    log_result_t r = atomic_load(&s_stopping) ? LOG_SKIP : log_append((int64_t)time(NULL), s);
    atomic_store(&s_writing, false);
    if (r == LOG_FAIL) {
        log_stop_with_notice();
    }
}

static void take_sample(void) {
    lichen_sensors_t sn;
    lichen_sensors_get(&sn);
    wx_sample_t s;
    sample_from(&sn, &s);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    wx_ring_push(&s_ring, &s);
    xSemaphoreGive(s_lock);
    atomic_fetch_add(&s_version, 1);
    // Debug only: float printf is the deepest stack user here after FATFS, and `fw sensors` shows the same values
    ESP_LOGD(TAG, "sample %u: %.1f C, %.0f%%, %.0f hPa, gas %.0f kOhm", (unsigned)s_ring.count, (double)s.v[WX_TEMP],
             (double)s.v[WX_HUM], (double)s.v[WX_PRESS], (double)s.v[WX_GAS]);
    log_sample(&s);
    // The task stack is internal RAM (it writes NVS when the log fails): report its margin once an hour (review #155)
    static unsigned samples;
    if (samples++ % 60 == 0) {
        ESP_LOGI(TAG, "sampler stack never below %u bytes free (of %u), log %s", (unsigned)uxTaskGetStackHighWaterMark(NULL),
                 (unsigned)TASK_STACK, atomic_load(&s_log_on) ? "on" : "off");
    }
}

static void sampler_task(void *arg) {
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(FIRST_SAMPLE_MS));
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        if (!atomic_load(&s_stopping)) {
            take_sample();
        }
        vTaskDelayUntil(&last, pdMS_TO_TICKS(SAMPLE_MS));
    }
}

static bool start_background(void) {
    wx_sample_t *store = lichen_alloc(self_ctx, WX_RING_CAP * sizeof(wx_sample_t), LICHEN_MEM_BULK | LICHEN_MEM_ZERO);
    s_lock             = xSemaphoreCreateMutex();
    if (!store || !s_lock) {
        ESP_LOGE(TAG, "no memory for the history");
        lichen_free(self_ctx, store);
        if (s_lock) {
            vSemaphoreDelete(s_lock);
            s_lock = NULL;
        }
        return false;
    }
    wx_ring_init(&s_ring, store, WX_RING_CAP);
    atomic_store(&s_offset_half, wx_offset_half_from_byte(app_settings_get_u8(self_ctx, KEY_OFFSET, wx_offset_byte(0))));
    atomic_store(&s_log_on, app_settings_get_u8(self_ctx, KEY_LOG, 0) != 0);
    if (!lichen_task_start(self_ctx, "weather", sampler_task, NULL, TASK_STACK, LICHEN_PRIO_BACKGROUND, NULL)) {
        ESP_LOGE(TAG, "no memory for the sampler task");
        return false;
    }
    return true;
}

// A firmware update or a restart: let a log write in progress finish, write no more
static bool stop_for_update(void) {
    atomic_store(&s_stopping, true);
    for (int i = 0; i < 40 && atomic_load(&s_writing); i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return !atomic_load(&s_writing);
}

// ---- Settings > Weather ----------------------------------------------------------------------------------------------

static lv_obj_t *s_offset_row;
static lv_obj_t *s_log_row;

static void offset_show(void) {
    char text[16];
    wx_offset_text(text, sizeof(text), atomic_load(&s_offset_half));
    menu_set_value(s_offset_row, text);
}

static void offset_set(int half) {
    half = half > 0 ? -WX_OFFSET_MAX_HALF : half < -WX_OFFSET_MAX_HALF ? 0 : half; // Enter wraps round
    atomic_store(&s_offset_half, half);
    if (!app_settings_set_u8(self_ctx, KEY_OFFSET, wx_offset_byte(half))) {
        ESP_LOGW(TAG, "offset not saved");
    }
    atomic_fetch_add(&s_version, 1); // the graph redraws with it
    offset_show();
}

static void offset_step(void *arg) {
    (void)arg;
    offset_set(atomic_load(&s_offset_half) - 1);
}

static bool offset_text_key(uint32_t key, void *arg) {
    (void)arg;
    int half = atomic_load(&s_offset_half);
    if (key == '-') {
        offset_set(half > -WX_OFFSET_MAX_HALF ? half - 1 : half);
    } else if (key == '+' || key == '=') {
        offset_set(half < 0 ? half + 1 : half);
    } else {
        return false;
    }
    return true;
}

static void log_toggle(void *arg) {
    (void)arg;
    bool on = !atomic_load(&s_log_on);
    if (on && !lichen_fs_sd_mounted()) {
        lichen_notify(self_ctx, LN_WARN, "No SD card", "Put an SD card in the badge to log the weather.", 0);
        return;
    }
    atomic_store(&s_log_on, on);
    app_settings_set_u8(self_ctx, KEY_LOG, on ? 1 : 0);
    menu_set_on(s_log_row, on);
}

static void weather_settings(lv_obj_t *menu) {
    s_offset_row = menu_add(menu, "Temperature offset", "", offset_step, NULL);
    menu_set_kind(s_offset_row, MENU_CYCLE);
    offset_show();
    menu_on_text(menu, offset_text_key, NULL);
    menu_note(menu, "The badge warms the sensor, so it reads high. Enter lowers the offset by 0.5 K (back to 0 K "
                    "at -10 K); + and - also step it. Set it until Temperature matches a thermometer nearby.");
    s_log_row = menu_add(menu, "Log to SD card", "", log_toggle, NULL);
    menu_set_kind(s_log_row, MENU_TOGGLE);
    menu_set_on(s_log_row, atomic_load(&s_log_on));
    menu_note(menu, "One line a minute in weather.csv on the SD card (time in UTC, temperature with the offset). It "
                    "switches itself off, with a notice, if the card is missing or a write fails.");
}

// ---- home tile -------------------------------------------------------------------------------------------------------

static bool weather_tile(char *l1, size_t c1, char *l2, size_t c2) {
    wx_sample_t s;
    if (!weather_current(&s)) {
        return false;
    }
    char t[24], h[16], p[24];
    wx_fmt_value(t, sizeof(t), WX_TEMP, s.v[WX_TEMP]);
    wx_fmt_value(h, sizeof(h), WX_HUM, s.v[WX_HUM]);
    wx_fmt_value(p, sizeof(p), WX_PRESS, s.v[WX_PRESS]);
    snprintf(l1, c1, "%s", t);
    snprintf(l2, c2, "%s \xE2\x80\xA2 %s", h, p);
    return true;
}

static lichen_app_t const weather_app = {
    .name             = "Weather",
    .id               = "weather",
    .draw_icon        = icon_weather,
    .tile_text        = weather_tile,
    .open             = weather_screen_open,
    .settings         = weather_settings,
    .start_background = start_background,
    .stop_for_update  = stop_for_update,
};

void weather_app_register(void) {
    self_ctx = lichen_register_app(&weather_app);
}
