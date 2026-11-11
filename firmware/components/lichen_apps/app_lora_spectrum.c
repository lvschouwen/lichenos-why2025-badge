// LoRa spectrum app (#161): the signal per frequency step across the 868 MHz band as bars, with the highest reading seen
// per step and the MeshCore channel's noise. Receive only: the C6 sweeps the band (lora_sweep) and nothing is sent.
// The radio has one owner (#179), so while the screen is open MeshCore is paused (it gives the radio back when it stops,
// #103) and started again when the screen closes. The pause, the sweeps and the hand-back run on a task of their own,
// never on the LVGL task; the screen only draws what that task hands over.
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lichen_app.h"
#include "lichen_mem.h"
#include "lichen_radio.h"
#include "lichen_task.h"
#include "lichen_ui.h"
#include "lora_app.h"
#include "lora_spectrum.h"
#include "lichen_apps.h"
#include "theme.h"

#include <stdio.h>
#include <string.h>

static char const TAG[] = "spectrum";

#define OWNER            "spectrum" // the radio's owner name while the screen is open (lora_app.h)
#define GRAPH_W          648        // 720 - 2 * THEME_PAD
#define GRAPH_H          160        // 200 pushed the status line's second row under the pinned hint (#161 bench)
#define PEAK_H           2
#define SWEEP_TIMEOUT_MS 3000 // a pass over 128 steps: 0.5-1 s on the C6 by estimate, to measure on the bench
#define SWEEP_GAP_MS     100  // between sweeps: the C6 does its other work, the screen redraws
#define RETRY_MS         1000
#define WORKER_STACK     4096
#define STOP_WAIT_MS     6000 // stop_for_update: a sweep's timeout plus the hand-back

typedef enum {
    ST_STARTING,      // pausing MeshCore and taking the radio
    ST_LIVE,
    ST_OFF,           // the LoRa radio switch (or airplane mode) is off
    ST_NOT_FREE,      // MeshCore did not let go of the radio
    ST_UNAVAILABLE,   // stopped for an update, or the C6 is being recovered
    ST_NO_ANSWER,
    ST_NOT_RECEIVING, // the C6 is not receiving yet
    ST_FAILED,
    ST_NO_MEMORY,
} spec_state_t;

// Between the worker and the LVGL task, under lock(); in PSRAM, made at the first open and kept
typedef struct {
    bool                want;        // the screen is open
    bool                alive;       // the worker task exists
    bool                posted;      // an update is queued for the LVGL task
    bool                fresh;       // result holds a sweep the screen has not drawn
    bool                paused_mesh; // MeshCore ran and was paused for this
    uint8_t             band;        // asked for by the screen
    spec_state_t        state;
    uint32_t            sweeps;
    spec_plan_t         plan;        // what result measured
    uint8_t             result_band; // the band it was asked for
    lora_sweep_result_t result;
} shared_t;

static lichen_app_t const spectrum_app; // defined at the end: the owner of the task and the buffers (#181)
static lichen_ctx_t self_ctx; // lichen_register_app's answer, for the gate calls (#230 E1b)
static shared_t    *sh;

// A mutex, not a spinlock: sh and ui are in PSRAM, and a critical section must never touch PSRAM (lichen_task.c,
// #271). Made with sh, on the LVGL task before the first worker: everything that locks has seen sh set.
static StaticSemaphore_t sh_lock_buf;
static SemaphoreHandle_t sh_lock;

static void lock(void) {
    xSemaphoreTake(sh_lock, portMAX_DELAY);
}
static void unlock(void) {
    xSemaphoreGive(sh_lock);
}

// The screen's own, LVGL task only, in PSRAM while it is open
typedef struct {
    lv_obj_t           *bar[LORA_SWEEP_BINS];
    lv_obj_t           *peak[LORA_SWEEP_BINS];
    lv_obj_t           *caption, *axis_lo, *axis_hi, *channel, *now, *highest, *status;
    spec_plan_t         plan; // what the bars are laid out for (count 0: none yet)
    uint8_t             band;
    bool                have_peak;
    int16_t             peak_x10[LORA_SWEEP_BINS];
    lora_sweep_result_t shown;
} ui_t;

static ui_t *ui;

// ---- the worker --------------------------------------------------------------------------------------------------

static void on_update(void *arg);

// One queued update at a time: the LVGL task reads the newest state when it runs
static void post(void) {
    lock();
    bool need  = !sh->posted;
    sh->posted = true;
    unlock();
    if (need && !lichen_ui_call(on_update, NULL)) {
        lock();
        sh->posted = false; // the next state change tries again
        unlock();
    }
}

static void set_state(spec_state_t st) {
    lock();
    sh->state = st;
    unlock();
    post();
}

static bool wanted(void) {
    lock();
    bool w = sh->want;
    unlock();
    return w;
}

static void wait_while_wanted(uint32_t ms) {
    for (uint32_t t = 0; t < ms && wanted(); t += 50) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static spec_state_t state_of(lora_sweep_err_t err) {
    switch (err) {
        case LORA_SWEEP_OK: return ST_LIVE;
        case LORA_SWEEP_OFF: return ST_UNAVAILABLE;
        case LORA_SWEEP_NO_ANSWER: return ST_NO_ANSWER;
        case LORA_SWEEP_NOT_RECEIVING: return ST_NOT_RECEIVING;
        default: return ST_FAILED;
    }
}

// One sweep of the band the screen asks for, handed to the screen
static void sweep_once(lora_sweep_result_t *res) {
    lock();
    uint8_t band = sh->band;
    unlock();
    uint8_t     sf, cr;
    uint16_t    preamble;
    uint32_t    bw;
    spec_plan_t plan = {0};
    lora_get_radio(&sf, &bw, &cr, &preamble);
    if (!spec_plan(&spec_bands[band], lora_get_freq(), bw, LORA_SWEEP_BINS, &plan)) {
        set_state(ST_FAILED);
        wait_while_wanted(RETRY_MS);
        return;
    }
    lora_sweep_err_t err = lora_sweep(OWNER, plan.first_hz, plan.step_hz, plan.count, res, SWEEP_TIMEOUT_MS);
    if (err == LORA_SWEEP_OK && (res->count != plan.count || res->start_hz != plan.first_hz ||
                                 res->step_hz != plan.step_hz)) {
        ESP_LOGW(TAG, "sweep answer does not match the request: dropped");
        wait_while_wanted(SWEEP_GAP_MS);
        return;
    }
    if (err != LORA_SWEEP_OK) {
        ESP_LOGD(TAG, "sweep: %d", (int)err);
        set_state(state_of(err));
        wait_while_wanted(RETRY_MS);
        return;
    }
    lock();
    sh->result = *res;
    sh->plan   = plan;
    sh->result_band = band;
    sh->fresh  = true;
    sh->state  = ST_LIVE;
    sh->sweeps++;
    unlock();
    post();
    wait_while_wanted(SWEEP_GAP_MS);
}

// One stay of the screen: MeshCore paused, the radio ours, sweeps until the screen closes, everything given back
static void borrow_and_sweep(lora_sweep_result_t *res) {
    while (wanted() && !lichen_radio_get(RADIO_LORA)) {
        set_state(ST_OFF); // nothing paused for a radio that is off: the scan starts once it is switched on
        wait_while_wanted(RETRY_MS);
    }
    if (!wanted()) {
        return;
    }
    set_state(ST_STARTING);
    bool was_running = false;
    // Claimed only after a pause that finished: a MeshCore still stopping holds the radio. The wait ends early when the
    // screen closes (or an update asks this app to stop).
    bool paused  = lichen_radio_lora_pause(&was_running, wanted);
    bool claimed = paused && wanted() && lora_claim(OWNER, NULL);
    lock();
    sh->paused_mesh = was_running;
    unlock();
    if (!claimed) {
        ESP_LOGW(TAG, "LoRa radio not free (MeshCore %s)", paused ? "paused" : "did not stop");
        set_state(ST_NOT_FREE);
    }
    ESP_LOGI(TAG, "%s", claimed ? "sweeping, receive only" : "not sweeping");
    while (claimed && wanted()) {
        if (!lichen_radio_get(RADIO_LORA)) {
            set_state(ST_OFF);
            wait_while_wanted(RETRY_MS);
            continue;
        }
        sweep_once(res);
    }
    // Not claimed: no retry (each would stop and start MeshCore again); it waits for the screen to close
    while (wanted()) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (claimed) {
        lora_release(OWNER);
    }
    ESP_LOGI(TAG, "radio given back");
    if (was_running) {
        lichen_radio_lora_resume(); // waits for a late stop to finish, then starts MeshCore (logged there)
    }
}

// Ends by returning: lichen_task_start takes it out of the book (#181)
static void worker(void *arg) {
    (void)arg;
    lora_sweep_result_t *res = lichen_alloc(self_ctx, sizeof(*res), LICHEN_MEM_BULK);
    if (!res) {
        set_state(ST_NO_MEMORY);
    }
    for (;;) {
        if (res) {
            borrow_and_sweep(res);
        }
        // Opened again while giving back: start over instead of leaving the new screen without a worker
        lock();
        bool again = sh->want && res;
        sh->alive  = again;
        unlock();
        if (!again) {
            break;
        }
    }
    lichen_free(self_ctx, res);
}

// LVGL task, from open: the worker starts, or the one still giving back carries on
static void worker_want(void) {
    lock();
    sh->want    = true;
    sh->fresh   = false;
    sh->state   = ST_STARTING;
    bool create = !sh->alive;
    sh->alive   = true;
    unlock();
    if (create &&
        !lichen_task_start(self_ctx, "spectrum", worker, NULL, WORKER_STACK, LICHEN_PRIO_BACKGROUND, NULL)) {
        lock();
        sh->alive = false;
        sh->state = ST_NO_MEMORY;
        unlock();
        ESP_LOGW(TAG, "worker task not started");
    }
}

// ---- drawing -----------------------------------------------------------------------------------------------------

static lv_obj_t *box(lv_obj_t *parent, uint32_t colour) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(colour), 0);
    lv_obj_set_scrollable(o, false);
    lv_obj_set_clickable(o, false);
    lv_obj_set_hidden(o, true);
    return o;
}

// A new plan (another band, another bandwidth): the bars are laid out again and the highest readings start over
static void lay_out(spec_plan_t const *plan) {
    for (int i = 0; i < LORA_SWEEP_BINS; i++) {
        bool used = i < plan->count;
        lv_obj_set_hidden(ui->bar[i], !used);
        lv_obj_set_hidden(ui->peak[i], true);
        if (used) {
            int x, w;
            spec_bin_span((uint8_t)i, plan->count, GRAPH_W, &x, &w);
            lv_obj_set_pos(ui->bar[i], x, GRAPH_H - SPEC_MIN_BAR);
            lv_obj_set_size(ui->bar[i], w, SPEC_MIN_BAR);
            lv_obj_set_x(ui->peak[i], x);
            lv_obj_set_width(ui->peak[i], w);
            lv_obj_set_style_bg_color(ui->bar[i], lv_color_hex(i == plan->channel_bin ? THEME_AMBER : THEME_TEAL), 0);
        }
    }
    ui->plan      = *plan;
    ui->have_peak = false;
    if (!plan->count) {
        return; // flat until the first sweep of the new band
    }
    char step[16], lo[24], hi[24], text[160];
    spec_fmt_khz(plan->step_hz, step, sizeof(step));
    spec_fmt_mhz(plan->first_hz - plan->step_hz / 2, lo, sizeof(lo));
    spec_fmt_mhz(plan->first_hz + plan->step_hz * (plan->count - 1u) + plan->step_hz / 2, hi, sizeof(hi));
    lv_label_set_text(ui->axis_lo, lo);
    lv_label_set_text(ui->axis_hi, hi);
    snprintf(text, sizeof(text), "Signal per %s step, %d to %d dBm.%s Line: highest seen.", step, SPEC_FLOOR_DBM,
             SPEC_TOP_DBM, plan->channel_bin >= 0 ? " Amber: the MeshCore channel." : "");
    lv_label_set_text(ui->caption, text);
}

static void draw_bars(lora_sweep_result_t const *r) {
    if (!ui->have_peak) {
        spec_peak_reset(ui->peak_x10, r->count);
        ui->have_peak = true;
    }
    spec_peak_update(ui->peak_x10, r->rssi_x10, r->count);
    for (int i = 0; i < r->count; i++) {
        int h = spec_bar_px(r->rssi_x10[i], GRAPH_H);
        lv_obj_set_height(ui->bar[i], h);
        lv_obj_set_y(ui->bar[i], GRAPH_H - h);
        int p = spec_bar_px(ui->peak_x10[i], GRAPH_H);
        lv_obj_set_y(ui->peak[i], GRAPH_H - (p < PEAK_H ? PEAK_H : p));
        lv_obj_set_hidden(ui->peak[i], false);
    }
}

static void draw_readouts(lora_sweep_result_t const *r) {
    char f[24], d[24], text[96];
    spec_fmt_mhz(lora_get_freq(), f, sizeof(f));
    spec_fmt_dbm(r->channel_rssi_x10, d, sizeof(d));
    snprintf(text, sizeof(text), "Channel %s: %s", f, d);
    lv_label_set_text(ui->channel, text);
    size_t at = spec_strongest(r->rssi_x10, r->count);
    spec_fmt_mhz(ui->plan.first_hz + ui->plan.step_hz * at, f, sizeof(f));
    spec_fmt_dbm(r->rssi_x10[at], d, sizeof(d));
    snprintf(text, sizeof(text), "Strongest now: %s, %s", d, f);
    lv_label_set_text(ui->now, text);
    at = spec_strongest(ui->peak_x10, r->count);
    spec_fmt_mhz(ui->plan.first_hz + ui->plan.step_hz * at, f, sizeof(f));
    spec_fmt_dbm(ui->peak_x10[at], d, sizeof(d));
    snprintf(text, sizeof(text), "Highest seen: %s, %s", d, f);
    lv_label_set_text(ui->highest, text);
}

// Before the first sweep of a band: what will be there
static void draw_waiting(void) {
    char f[24], text[96];
    spec_fmt_mhz(lora_get_freq(), f, sizeof(f));
    snprintf(text, sizeof(text), "Channel %s: waiting for the first sweep", f);
    lv_label_set_text(ui->channel, text);
    lv_label_set_text(ui->now, "Strongest now: none yet");
    lv_label_set_text(ui->highest, "Highest seen: none yet");
}

static void draw_status(spec_state_t st, uint32_t sweeps, bool paused_mesh) {
    char text[160];
    switch (st) {
        case ST_STARTING: snprintf(text, sizeof(text), "Pausing MeshCore and taking the LoRa radio..."); break;
        case ST_LIVE:
            snprintf(text, sizeof(text), "Live, sweep %lu. Receive only, nothing is sent.%s", (unsigned long)sweeps,
                     paused_mesh ? " MeshCore is paused and starts again when you leave." : "");
            break;
        case ST_OFF:
            snprintf(text, sizeof(text), "The LoRa radio is off. Switch it on in Settings > LoRa radio; the scan starts "
                                         "by itself.");
            break;
        case ST_NOT_FREE:
            snprintf(text, sizeof(text), "MeshCore did not give the LoRa radio free. Leave and open LoRa spectrum again.");
            break;
        case ST_UNAVAILABLE:
            snprintf(text, sizeof(text), "The LoRa radio is not available now (an update, or the C6 restarting). "
                                         "Trying again.");
            break;
        case ST_NO_ANSWER: snprintf(text, sizeof(text), "The C6 radio chip did not answer. Trying again."); break;
        case ST_NOT_RECEIVING: snprintf(text, sizeof(text), "The LoRa radio is not receiving yet. Trying again."); break;
        case ST_FAILED: snprintf(text, sizeof(text), "The LoRa radio reported an error. Trying again."); break;
        case ST_NO_MEMORY: snprintf(text, sizeof(text), "Out of memory. Leave and open LoRa spectrum again."); break;
        default: text[0] = '\0'; break;
    }
    lv_label_set_text(ui->status, text);
}

// LVGL task: the worker's newest state and sweep
static void on_update(void *arg) {
    (void)arg;
    lock();
    sh->posted          = false;
    // A sweep of the band before an Enter is dropped: the next one is of the new band
    bool         fresh  = sh->fresh && ui && sh->result_band == ui->band;
    spec_state_t st     = sh->state;
    uint32_t     sweeps = sh->sweeps;
    bool         paused = sh->paused_mesh;
    spec_plan_t  plan   = sh->plan;
    if (fresh) {
        ui->shown = sh->result;
        sh->fresh = false;
    }
    unlock();
    if (!ui) {
        return; // closed meanwhile
    }
    if (fresh && plan.count) {
        if (!spec_plan_equal(&plan, &ui->plan)) {
            lay_out(&plan);
        }
        draw_bars(&ui->shown);
        draw_readouts(&ui->shown);
    }
    draw_status(st, sweeps, paused);
}

// ---- the screen --------------------------------------------------------------------------------------------------

static void on_key(lv_event_t *e) {
    uint32_t key = lv_event_get_key(e);
    if (!ui) {
        return;
    }
    if (key == LV_KEY_ENTER) {
        // Another band: the bars go flat at once, the worker takes it from its next sweep
        ui->band = (uint8_t)((ui->band + 1) % SPEC_BANDS);
        lock();
        sh->band  = ui->band;
        sh->fresh = false;
        unlock();
        spec_plan_t none = {0}; // count 0: no bars until the new band's first sweep
        lay_out(&none);
        char text[64];
        snprintf(text, sizeof(text), "Switching to %s...", spec_bands[ui->band].name);
        lv_label_set_text(ui->caption, text);
        lv_label_set_text(ui->axis_lo, "");
        lv_label_set_text(ui->axis_hi, "");
        draw_waiting();
    } else if (key == LV_KEY_BACKSPACE || key == LV_KEY_DEL) {
        ui->have_peak = false;
        for (int i = 0; i < LORA_SWEEP_BINS; i++) {
            lv_obj_set_hidden(ui->peak[i], true);
        }
        lv_label_set_text(ui->highest, "Highest seen: cleared");
    }
}

static void on_delete(lv_event_t *e) {
    (void)e;
    lock();
    sh->want = false; // the worker gives the radio back and MeshCore starts again
    unlock();
    lichen_free(self_ctx, ui);
    ui = NULL;
}

static lv_obj_t *build_graph(lv_obj_t *screen) {
    lv_obj_t *g = lv_obj_create(screen);
    lv_obj_remove_style_all(g);
    lv_obj_set_size(g, GRAPH_W, GRAPH_H);
    lv_obj_set_style_bg_opa(g, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(g, lv_color_hex(THEME_TILE), 0);
    lv_obj_set_scrollable(g, false);
    lv_obj_set_clickable(g, false);
    for (int i = 0; i < LORA_SWEEP_BINS; i++) {
        ui->bar[i]  = box(g, THEME_TEAL);
        ui->peak[i] = box(g, THEME_DIM);
        lv_obj_set_height(ui->peak[i], PEAK_H);
    }
    return g;
}

static void build_axis(lv_obj_t *screen) {
    lv_obj_t *row = lv_obj_create(screen);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, GRAPH_W, LV_SIZE_CONTENT);
    lv_obj_set_scrollable(row, false);
    ui->axis_lo = lv_label_create(row);
    ui->axis_hi = lv_label_create(row);
    lv_obj_set_style_text_color(ui->axis_lo, lv_color_hex(THEME_DIM), 0);
    lv_obj_set_style_text_color(ui->axis_hi, lv_color_hex(THEME_DIM), 0);
    lv_obj_align(ui->axis_lo, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_align(ui->axis_hi, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_label_set_text(ui->axis_lo, "");
    lv_label_set_text(ui->axis_hi, "");
}

// Two lines of the label's own font: a fixed 64 px cut the second line by 2 px (33 px lines, #133 sweep)
static int32_t two_lines(lv_obj_t *label) {
    return 2 * lv_font_get_line_height(lv_obj_get_style_text_font(label, LV_PART_MAIN));
}

static void open_spectrum(lv_obj_t *screen) {
    if (!sh) {
        if (!sh_lock) {
            sh_lock = xSemaphoreCreateMutexStatic(&sh_lock_buf); // static: cannot fail
        }
        sh = lichen_alloc(self_ctx, sizeof(*sh), LICHEN_MEM_BULK | LICHEN_MEM_ZERO); // kept for good
    }
    ui = sh ? lichen_alloc(self_ctx, sizeof(*ui), LICHEN_MEM_BULK | LICHEN_MEM_ZERO) : NULL;
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(screen, 8, 0);
    lichen_heading(screen, NULL, "LoRa spectrum");
    if (!ui) {
        lv_obj_t *l = lichen_label_wrap(screen, LV_PCT(100));
        lv_label_set_text(l, "Out of memory. Leave and open LoRa spectrum again.");
        return;
    }
    ui->caption = lichen_label_wrap(screen, LV_PCT(100));
    lv_obj_set_style_text_color(ui->caption, lv_color_hex(THEME_DIM), 0);
    lv_obj_set_height(ui->caption, two_lines(ui->caption)); // two lines, whatever the text: the graph does not jump (R7)
    build_graph(screen);
    build_axis(screen);
    ui->channel = lichen_label_line(screen, LV_PCT(100));
    ui->now     = lichen_label_line(screen, LV_PCT(100));
    ui->highest = lichen_label_line(screen, LV_PCT(100));
    ui->status  = lichen_label_wrap(screen, LV_PCT(100));
    lv_obj_set_style_text_color(ui->status, lv_color_hex(THEME_DIM), 0);
    lv_obj_set_height(ui->status, two_lines(ui->status));
    shell_page_hint(screen, "Enter", "other band", "Backspace", "clear highest", NULL);
    lv_obj_add_event_cb(screen, on_key, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(screen, on_delete, LV_EVENT_DELETE, NULL);
    lock();
    ui->band = sh->band; // the band chosen last time
    unlock();
    char text[64];
    snprintf(text, sizeof(text), "Scanning %s...", spec_bands[ui->band].name);
    lv_label_set_text(ui->caption, text);
    draw_waiting();
    worker_want();
    lock();
    spec_state_t st = sh->state;
    unlock();
    draw_status(st, 0, false);
}

// A firmware update (#76): the screen is closed by then; wait for the radio to be given back. The worker's waits end as
// soon as want is false (a pause under way included), so this takes at most a sweep's timeout.
static bool spectrum_stop_for_update(void) {
    if (!sh) {
        return true;
    }
    lock();
    sh->want = false;
    unlock();
    for (uint32_t t = 0; t < STOP_WAIT_MS; t += 50) {
        lock();
        bool alive = sh->alive;
        unlock();
        if (!alive) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    ESP_LOGW(TAG, "still holding the LoRa radio");
    return false;
}

// Five bars of a spectrum, the middle one amber like the channel's
static void icon_spectrum(lv_obj_t *parent, int size_px) {
    lv_obj_set_style_radius(parent, size_px / 4, 0);
    lv_obj_set_style_bg_color(parent, lv_color_hex(THEME_TRACK), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    static uint8_t const pct[5] = {30, 55, 85, 45, 25};
    int                  w = size_px / 9, gap = size_px / 18, left = (size_px - 5 * w - 4 * gap) / 2;
    for (int i = 0; i < 5; i++) {
        lv_obj_t *b = lv_obj_create(parent);
        lv_obj_remove_style_all(b);
        int h = size_px * 3 / 4 * pct[i] / 100;
        lv_obj_set_size(b, w, h);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(i == 2 ? THEME_AMBER : THEME_TEAL), 0);
        lv_obj_align(b, LV_ALIGN_BOTTOM_LEFT, left + i * (w + gap), -size_px / 8);
    }
}

static lichen_app_t const spectrum_app = {
    .name            = "LoRa spectrum",
    .id              = "lora_spectrum",
    .draw_icon       = icon_spectrum,
    .open            = open_spectrum,
    .stop_for_update = spectrum_stop_for_update,
};

void lora_spectrum_app_register(void) {
    self_ctx = lichen_register_app(&spectrum_app);
}
