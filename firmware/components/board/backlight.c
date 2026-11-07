#include "backlight.h"
#include "backlight_level.h"
#include "r16_hold.h"
#include "uart_lend.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"

#include "display.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "fw_console.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BL_UART    UART_NUM_1 // the C6's console UART; the C6 flasher and "fw c6log" use it too
#define BL_TX_PIN  16
#define BL_RX_PIN  15
#define BL_FRAME_MS 20 // one 60 Hz frame is 17 ms

static char const TAG[] = "backlight";

static backlight_get_u8_fn store_get;
static backlight_set_u8_fn store_set;

void backlight_bind_store(backlight_get_u8_fn get, backlight_set_u8_fn set) {
    store_get = get;
    store_set = set;
}

// Guards the UART1 driver's lifetime: check-and-install, every write, and backlight_restore's delete (#246). Held only
// for one short write (the 20-byte line, a few ms at 115200) or the bounded tx-done wait before the delete, never across
// anything else. PTHREAD_MUTEX_INITIALIZER needs no create call, so there is no first-use race on the lock itself.
static pthread_mutex_t uart_lock = PTHREAD_MUTEX_INITIALIZER;
// Under uart_lock: UART1 lent to the C6 flasher (exclusive) or "fw c6log" (shared) (#250). A flag and not the lock held
// for the lend: the flasher keeps UART1 for seconds and c6log for up to a minute, and the senders are the UI and console
// tasks (a slider, the screen timeout), which must not stall that long. During the flasher's lend a send only records
// its levels; during c6log's it goes out through the driver the lend keeps installed.
static uart_lend_t lend;

// Call with uart_lock held
static bool open_uart(void) {
    if (uart_is_driver_installed(BL_UART)) {
        return true;
    }
    uart_config_t const cfg = {
        .baud_rate  = 115200,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    // 2 KiB receive buffer: "fw c6log" reads the C6's console through this driver
    if (uart_driver_install(BL_UART, 2048, 0, 0, NULL, 0) != ESP_OK) {
        ESP_LOGE(TAG, "UART to the C6 not available");
        return false;
    }
    uart_param_config(BL_UART, &cfg);
    uart_set_pin(BL_UART, BL_TX_PIN, BL_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    return true;
}

typedef enum { BL_SENT, BL_HELD, BL_FAILED } bl_sent_t; // HELD: kept for after the flasher's lend

// The levels as given, no clamp
static bl_sent_t send_raw(unsigned display, unsigned keyboard) {
    char   cmd[24];
    size_t len = backlight_cmd_format(cmd, sizeof(cmd), display, keyboard);
    if (!len) {
        return BL_FAILED;
    }
    pthread_mutex_lock(&uart_lock);
    bl_sent_t r = BL_HELD;
    if (uart_lend_send(&lend, (uint8_t)display, (uint8_t)keyboard)) {
        r = open_uart() && uart_write_bytes(BL_UART, cmd, len) == (int)len ? BL_SENT : BL_FAILED;
    }
    pthread_mutex_unlock(&uart_lock);
    return r;
}

static bool uart_lent(void) {
    pthread_mutex_lock(&uart_lock);
    bool lent = lend.mode != UART_LEND_NONE;
    pthread_mutex_unlock(&uart_lock);
    return lent;
}

bool backlight_uart_lend(bool exclusive) {
    pthread_mutex_lock(&uart_lock);
    bool ok = uart_lend_take(&lend, exclusive ? UART_LEND_EXCLUSIVE : UART_LEND_SHARED);
    if (ok && exclusive && uart_is_driver_installed(BL_UART)) {
        uart_wait_tx_done(BL_UART, pdMS_TO_TICKS(50)); // a BL line still going out
        uart_driver_delete(BL_UART);
    } else if (ok && !exclusive && !open_uart()) {
        // Shared: the backlight's own driver, installed here under the lock and kept for the lend, so no install or
        // delete can race a BL write
        uint8_t d, k;
        uart_lend_give(&lend, &d, &k);
        ok = false;
    }
    pthread_mutex_unlock(&uart_lock);
    return ok;
}

void backlight_uart_return(void) {
    char    cmd[24];
    size_t  len = 0;
    uint8_t display, keyboard;
    pthread_mutex_lock(&uart_lock);
    if (uart_lend_give(&lend, &display, &keyboard)) {
        len = backlight_cmd_format(cmd, sizeof(cmd), display, keyboard);
    }
    // Still under the lock, so a newer send from another task cannot be overtaken by these older levels
    bool ok = !len || (open_uart() && uart_write_bytes(BL_UART, cmd, len) == (int)len);
    pthread_mutex_unlock(&uart_lock);
    if (!ok) {
        ESP_LOGW(TAG, "backlight held back during the UART1 lend not sent");
    }
}

// The last levels asked for (a slider included, not only the saved ones): a C6 reset brings back its 30/10 defaults and
// backlight_reapply() sends these again (#14). The restart blank (send_raw 0 0) is not one of them.
static portMUX_TYPE applied_lock = portMUX_INITIALIZER_UNLOCKED;
static bool         applied_known;
static uint8_t      applied_display, applied_keyboard;

static bl_sent_t send(uint8_t display, uint8_t keyboard) {
    portENTER_CRITICAL(&applied_lock);
    applied_known    = true;
    applied_display  = display;
    applied_keyboard = keyboard;
    portEXIT_CRITICAL(&applied_lock);
    return send_raw(display, keyboard);
}

void backlight_reapply(void) {
    uint8_t display, keyboard;
    portENTER_CRITICAL(&applied_lock);
    bool known = applied_known;
    display    = applied_display;
    keyboard   = applied_keyboard;
    portEXIT_CRITICAL(&applied_lock);
    if (!known) {
        backlight_get(&display, &keyboard);
    }
    if (send_raw(display, keyboard) == BL_FAILED) { // HELD goes out when the flasher returns UART1
        ESP_LOGW(TAG, "backlight not re-sent to the C6");
    }
}

static void save(uint8_t display, uint8_t keyboard) {
    if (!backlight_levels_save(store_set, display, keyboard)) {
        ESP_LOGW(TAG, "backlight levels not saved");
    }
}

// The person's levels (backlight_set*, the saved ones at start): what backlight_resume() goes back to after a dim (#43)
static uint8_t     user_display = BACKLIGHT_DEFAULT_DISPLAY, user_keyboard = BACKLIGHT_DEFAULT_KEYBOARD;
static atomic_uint user_gen;

static bl_sent_t send_user(uint8_t display, uint8_t keyboard, bool count) {
    portENTER_CRITICAL(&applied_lock);
    user_display  = display;
    user_keyboard = keyboard;
    portEXIT_CRITICAL(&applied_lock);
    if (count) {
        atomic_fetch_add(&user_gen, 1);
    }
    return send(display, keyboard);
}

uint32_t backlight_user_generation(void) {
    return atomic_load(&user_gen);
}

void backlight_idle(uint8_t display, uint8_t keyboard) {
    send(display > 100 ? 100 : display, backlight_keyboard_level(keyboard));
}

void backlight_resume(void) {
    portENTER_CRITICAL(&applied_lock);
    uint8_t display  = user_display;
    uint8_t keyboard = user_keyboard;
    portEXIT_CRITICAL(&applied_lock);
    send(display, keyboard);
}

void backlight_temporary(uint8_t display, uint8_t keyboard) {
    send(backlight_display_level(display), backlight_keyboard_level(keyboard));
}

void backlight_temporary_keyboard(uint8_t keyboard) {
    portENTER_CRITICAL(&applied_lock);
    uint8_t display = applied_known ? applied_display : user_display; // whatever the screen timeout set stays
    portEXIT_CRITICAL(&applied_lock);
    send(display, backlight_keyboard_level(keyboard));
}

void backlight_set(uint8_t display, uint8_t keyboard) {
    send_user(backlight_display_level(display), backlight_keyboard_level(keyboard), true);
}

void backlight_set_saved(uint8_t display, uint8_t keyboard) {
    display  = backlight_display_level(display);
    keyboard = backlight_keyboard_level(keyboard);
    send_user(display, keyboard, true);
    save(display, keyboard);
}

void backlight_get(uint8_t *display, uint8_t *keyboard) {
    backlight_levels_load(store_get, display, keyboard);
}

// ---- R16 (#12) ----
#define R16_GPIO      GPIO_NUM_5
#define R16_SETTLE_MS 150
#define R16_SAMPLES   20000 // a few ms: over a hundred periods of the C6's 25 kHz PWM

static atomic_bool r16_driving;
static atomic_bool r16_flashing; // a C6 flash check is running (c6_flasher): detection would sample a C6 in its ROM

// A true input: no pulls (gpio_reset_pin leaves the pull-up on), no interrupt
static void r16_input(void) {
    gpio_config_t const in = {.pin_bit_mask = 1ULL << R16_GPIO, .mode = GPIO_MODE_INPUT,
                              .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
                              .intr_type = GPIO_INTR_DISABLE};
    gpio_config(&in);
}

void backlight_r16_flash(bool on) {
    atomic_store(&r16_flashing, on);
}

static r16_fit_t r16_stored(void) {
    uint8_t v = store_get ? store_get(BACKLIGHT_KEY_R16, 0) : 0;
    return v == R16_FITTED || v == R16_ABSENT ? (r16_fit_t)v : R16_UNKNOWN;
}

// Input only, no pulls: a pull would turn a floating pin into a plausible reading
static void r16_sample_pin(r16_samples_t *s, int i) {
    uint32_t high = 0;
    for (int n = 0; n < R16_SAMPLES; n++) {
        high += (uint32_t)gpio_get_level(R16_GPIO);
    }
    s->high[i]  = high;
    s->total[i] = R16_SAMPLES;
}

static void r16_detect(void) {
    if (atomic_load(&r16_driving) || atomic_load(&r16_flashing)) {
        console_reply("r16: a C6 flash check is running, not now");
        return;
    }
    if (uart_lent()) { // "fw c6log": the levels would not reach the C6 until it ends
        console_reply("r16: UART1 to the C6 is busy, not now");
        return;
    }
    r16_input();
    r16_samples_t s = {0};
    for (int i = 0; i < R16_LEVELS; i++) {
        backlight_idle(r16_detect_level[i], 0); // the C6's PWM at this level; not saved
        vTaskDelay(pdMS_TO_TICKS(R16_SETTLE_MS));
        r16_sample_pin(&s, i);
    }
    backlight_resume();
    r16_input();
    r16_fit_t fit = r16_classify(&s);
    console_reply("r16: GPIO5 high at display %u/%u/%u %%: %u/%u/%u of %u -> %s", r16_detect_level[0], r16_detect_level[1],
                  r16_detect_level[2], (unsigned)s.high[0], (unsigned)s.high[1], (unsigned)s.high[2], R16_SAMPLES,
                  r16_fit_name(fit));
    if (fit != R16_UNKNOWN && !(store_set && store_set(BACKLIGHT_KEY_R16, (uint8_t)fit))) {
        console_reply("r16: not saved");
    }
}

void backlight_r16_hold(void) {
    if (!r16_may_drive(r16_stored(), R16_C6_ROM)) {
        return;
    }
    gpio_set_level(R16_GPIO, 1); // level first, so the pin never glitches low
    gpio_config_t const out = {.pin_bit_mask = 1ULL << R16_GPIO, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&out);
    gpio_set_level(R16_GPIO, 1);
    atomic_store(&r16_driving, true);
    ESP_LOGI(TAG, "R16 fitted: P4 holds the backlight while the C6 is in its ROM");
}

void backlight_r16_release(void) {
    if (!atomic_load(&r16_driving)) {
        return;
    }
    r16_input();
    atomic_store(&r16_driving, false);
    esp_rom_delay_us(100);    // the pad is high-z long before EN rises (the caller's next step)
}

static void cmd_bl(char const *args) {
    if (strcmp(args, "r16") == 0) {
        r16_detect();
        return;
    }
    char         *end;
    unsigned long d = strtoul(args, &end, 10);
    unsigned long k = strtoul(end, &end, 10);
    if (!*args || *end || d > 100 || k > 100) {
        console_reply("bl <display %d-100> <keyboard 0-100>", BACKLIGHT_DISPLAY_MIN);
        return;
    }
    backlight_set_saved((uint8_t)d, (uint8_t)k);
    uint8_t display, keyboard;
    backlight_get(&display, &keyboard);
    console_reply("backlight %u %u", display, keyboard);
}

// Runs inside esp_restart() (before the scheduler stops, newest handler first, so before esp_wifi_stop). The panel
// has no power switch and the backlight is the C6's: without this, the stopped DSI feed leaves the lit panel showing
// garbage (yellow in #23, blue since #106) until the next boot resets it. Backlight off first, so nothing shows while
// the panel switches off. Not run on panic, abort, watchdog or brownout resets: nothing can be done there. During the
// C6 flasher's exclusive UART1 lend (boot only) the BL 0 0 is held, not sent, so a restart then cannot blank the backlight;
// during a "fw c6log" it goes out as usual (#250).
static void blank_for_restart(void) {
    if (send_raw(0, 0) == BL_SENT) {
        pthread_mutex_lock(&uart_lock); // a restore cannot delete the driver under the wait
        if (uart_is_driver_installed(BL_UART)) {
            uart_wait_tx_done(BL_UART, pdMS_TO_TICKS(50));
        }
        pthread_mutex_unlock(&uart_lock);
    }
    display_blank();
    vTaskDelay(pdMS_TO_TICKS(BL_FRAME_MS));
}

// The shutdown handler (every esp_restart): system_restart's hook has held the "Restarting" frame long enough to read
// (#134); the boot screen lights the backlight again (backlight_restore after the first frame)
static void restart_handler(void) {
    blank_for_restart();
}

void backlight_power_off(void) {
    blank_for_restart();
}

void backlight_restore(void) {
    uint8_t display, keyboard;
    backlight_get(&display, &keyboard);
    bool sent = send_user(display, keyboard, false) == BL_SENT;
    // The C6 flasher installs the UART driver itself and fails if it is already there; backlight_start() opens it
    // again after the C6 check. The lock keeps a send from another task out of the driver while it goes away; its
    // next send opens it again, which only happens before the flasher runs if a task sends in that window.
    pthread_mutex_lock(&uart_lock);
    if (lend.mode == UART_LEND_NONE && uart_is_driver_installed(BL_UART)) { // a lent UART1 stays as the borrower has it
        if (sent) {
            uart_wait_tx_done(BL_UART, pdMS_TO_TICKS(50));
        }
        uart_driver_delete(BL_UART);
    }
    pthread_mutex_unlock(&uart_lock);
}

void backlight_start(void) {
    console_register("bl", "bl <display> <keyboard> (backlight 0-100, screen at least 5) | bl r16 (is R16 fitted?)", cmd_bl);
    uint8_t display, keyboard;
    backlight_get(&display, &keyboard);
    send_user(display, keyboard, false);
    // After c6link_start(): handlers run newest first, so this runs before esp_wifi_stop (which the C6 Wi-Fi setup
    // registers; there are only 5 slots in all)
    if (esp_register_shutdown_handler(restart_handler) != ESP_OK) {
        ESP_LOGE(TAG, "no shutdown handler slot: the screen will flash on restart");
    }
}
