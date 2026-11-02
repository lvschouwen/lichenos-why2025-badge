# Lichen app API: what an app may use

Apps are compiled into Lichen and there is no MMU, so nothing stops an app from calling anything in the image. The
boundary (#84) is held by three things instead: the app's component only `PRIV_REQUIRES` the components listed here,
so any other header does not resolve at build time; linker wraps that refuse the few calls that would take state the
OS owns; and this page. MeshCore (`firmware/components/mesh`) is the first app and the reference.

The rule of thumb: **the OS owns the hardware and every piece of shared state; an app reaches them only through
Lichen's calls.**

## What an app may use

An app component `PRIV_REQUIRES` from this list only (see `firmware/components/mesh/CMakeLists.txt`):

| Component | For |
|---|---|
| `lichen_api` | the app API below (it brings `lvgl` and `lichen_notify` along) |
| `lichen_clock` | reading the time and proposing one |
| `lichen_random` | random bytes |
| `lora` | the LoRa radio, as its one owner |
| `fw_console` | console commands and a deliberate restart |
| `freertos`, `esp_timer`, `heap` | tasks, timers, memory (PSRAM for anything large) |
| plain libraries | e.g. `mbedtls` (PSA Crypto); no driver, no storage, no network component |

### `lichen_api` headers, one line each

- `lichen_app.h`: register the app (`lichen_register_app`); `id` (a-z 0-9 _, at most 13) names its settings and
  directories; the hooks (`open`, `start_background`, `stop_for_update`, `first_run_set`...) are how the OS calls it.
- `lichen_ui.h`: navigation (`shell_open`, `shell_page_create`, `shell_back_then/_to`, `shell_home`, `shell_on_esc`,
  `shell_on_search`, scroll helpers), the countdown dialog, `lichen_screen_locked()`. Screen code runs on the LVGL task;
  never swap the default group or the active screen yourself. `lichen_ui_call(fn, arg)` hands a result from any task
  to the LVGL task (fn runs there, soon; false when it could not be queued) and `lichen_ui_with_group(group, build,
  arg)` adds widgets to a screen's group (LVGL context).
- `menu.h`, `menu_filter.h`, `list_filter.h`, `scroll_anchor.h`, `text_entry.h`: the shared list, filter and text
  widgets, so every app behaves the same under the keyboard.
- `theme.h`, `fonts.h`: Lichen's colours, sizes and fonts. Use them instead of literals.
- `lichen_settings.h`: the app's own NVS namespace `a.<id>` (u8, u32, u64, strings, erase, erase all), and adopting a
  key an earlier firmware kept in `badge_fw` (`app_settings_adopt_u8/_u32`: copied once and read back, the old key
  erased on a later boot). Never open NVS.
- `lichen_fs.h`: the app's directory `/storage/<id>` and, with a card in, `/sd/lichen/apps/<id>`; drive sizes. Never
  name a path outside them.
- `lichen_notify.h`: raise a notification (toast, history, unread dot); from any task.
- `lichen_badge.h`: the count on the app's tile; set it whenever it changes.
- `lichen_radio.h`: read the Wi-Fi/LoRa/Bluetooth switches and airplane mode, listen for changes. Apps never switch
  radios.
- `lichen_sensors.h`: read the latest IMU / environment / die temperature snapshot. Never talk to the chips or I2C.
- `lichen_crashlog.h`: the crash log, for the Crash log app; not from the LVGL task for writes.
- `radio_state.h`, `tz_table.h`: types and the time zone table used by the headers above.
- `settings_store.h` is Lichen's own (`badge_fw`) and sits here only because `lichen_boot` needs it: apps do not use it.

### Other services

- `lichen_clock.h`: `clock_is_set()`, `clock_is_approximate()`, `clock_source_text()`; an app that learns the time
  (MeshCore from its contacts) calls `clock_propose(CLOCK_SRC_APP, tv)`. SNTP and a time set by hand outrank it and the
  clock never goes backwards.
- `lichen_random.h`: `lichen_random_fill()`, `lichen_random_u32()`. The OS keeps the entropy source on.
- `lora.h`, the app section: `lora_claim(owner, cb)` / `lora_release(owner)` (one owner at a time), `lora_send`,
  `lora_set_radio` / `lora_set_tx_power` (the owner's radio settings, checked by `lora_config_valid`),
  `lora_set_link_callback`, `lora_set_rx_formatter`, the stats. Everything marked "OS only" there is not for apps.
- `fw_console.h`: `console_register` / `console_reply` for `fw <name>` commands; `system_restart(why)` when a restart
  is what the user asked for (it records why).

## What an app must not do

- Touch drivers or buses: no `driver/*`, I2C, SPI, GPIO, ADC, LEDC, the display, the backlight or the keyboard chip.
- Open NVS, or erase it: `nvs_open`, `nvs_flash_erase*`. Settings go through `lichen_settings.h`.
- Set the clock: `settimeofday`, `adjtime`, `sntp_*`. Propose a time with `clock_propose`.
- Touch the entropy source: `bootloader_random_enable/disable`.
- Start, stop or reconfigure the radio for everyone: `lora_stop`, `lora_set_enabled`, the C6 calls, Wi-Fi. An app
  that must stop for an update returns true from `stop_for_update`; the OS stops LoRa after every app did.
- Restart the badge with `esp_restart()` (it is recorded as an unknown caller) or call `abort()` to get out of a state.
- Write outside its own directories, or format anything but its own app directory.
- Take `lvgl_port_lock` or call `lv_async_call`: use `lichen_ui_call` (LVGL calls outside the lock while the LVGL
  task renders corrupted the heap, #118). Nor set the default group: use `lichen_ui_with_group`.
- Pick a core and priority of its own for a task: today's exception is listed below (`lichen_task_start`, #84 T8a).

## The wraps

Five calls are wrapped at link time (`-Wl,--wrap=`). The OS's own legitimate caller reaches the real function through
`__real_*`; every other caller lands in the wrap, which logs an `E` line with the caller's return address (look it up
in `firmware/build/badge_fw.map`) and refuses:

| Call | Wrap in | Allowed | Refused with |
|---|---|---|---|
| `settimeofday` | `lichen_clock.c` | `lichen_clock` (through `__real_`); any call that writes back the time the clock already shows (under 1 s off, `clock_resync_only`): that is IDF's `esp_sync_timekeeping_timers` on every restart and before sleep | `-1`, `errno = EPERM` |
| `nvs_flash_erase` | `console.c` | nobody: the OS erases by partition (`main.c` on an unusable layout, `factory_reset.c`, past the wrap below); MeshCore's `DataStore` is built with the name changed to its own wipe | `ESP_ERR_NOT_ALLOWED` |
| `bootloader_random_enable` | `lichen_random.c` | `lichen_random_start` (through `__real_`). IDF's `nvs_sec_provider` (NVS encryption, off) would be refused, so turning NVS encryption on fails the build until the wrap allows it | nothing happens (void) |
| `bootloader_random_disable` | `lichen_random.c` | nobody today (no caller in the image); the OS's future ADC hand-over (#11, #69) goes through `__real_` in `lichen_random.c`. `nvs_sec_provider` again: covered by the same build error | nothing happens (void) |
| `nvs_flash_erase_partition` | `console.c` | `main.c` (NVS unusable at boot) and `factory_reset.c` (#66, the full erase), both through `__real_` | `ESP_ERR_NOT_ALLOWED` |

`esp_restart` is wrapped too (`console.c`), but only to record the caller: a restart is never refused.

## Exceptions still open

MeshCore still `PRIV_REQUIRES` two components past the list; each has its follow-up:

- `nvs_flash`: only the legacy `badge_fw/identity` hand-over (kept as it is, by decision; `lichen_legacy_blob_take`).
  The advert keys `mc_advint`, `mc_advlast`, `mc_advfl` moved to `a.meshcore` (#123), adopted like `mc_name_set`.
- `sdcard`: the message journal's `sd_begin`/`sd_end` batches on `/sd/lichen/messages` (#98; an SD batch call in
  `lichen_fs.h`).

Also open from #84: tasks with their own core/priority (`lichen_task_start`, T8a), the console prefix per app (T8b), a heap caps wrapper
(V12), system apps calling `board/backlight.h` directly (V13).

## New services

Each new OS service follows the same shape: a `lichen_<thing>.h` in `lichen_api` (or a component `lichen_api`
REQUIRES), the OS owning the hardware and state behind it, apps reading snapshots and asking for changes, callbacks
short and on the caller's task. Notifications (#62), the radio switches (#108), sensors (#69) and tile badges (#102)
are the examples; power (#65), audio/haptics/LED (#67, #68, #74) and input (#79, #81) go the same way.
