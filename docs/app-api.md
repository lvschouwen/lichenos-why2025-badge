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
| `lichen_api` | the app API below (it brings `lvgl` along; `lichen_notify.h` is one of its headers since #245) |
| `lichen_clock` | reading the time and proposing one |
| `lichen_random` | random bytes |
| `lora` | the LoRa radio, as its one owner |
| `fw_console` | console commands and a deliberate restart |
| `psram_guard` | `psram_speed.h`, the PSRAM speed switch some large-buffer code asks about |
| `freertos`, `esp_timer`, `heap` | queues, semaphores, timers; tasks through `lichen_task.h` and memory through `lichen_mem.h` (below) |
| plain libraries | e.g. `mbedtls` (PSA Crypto); no driver, no storage, no network component |

### `lichen_api` headers, one line each

- `lichen_app.h`: register the app (`lichen_register_app`); `id` (a-z 0-9 _, at most 13) names its settings and
  directories; the hooks (`open`, `start_background`, `stop_for_update`, `first_run_set`...) are how the OS calls it.
  An app's `*_register()` runs on the main task during boot, not on the LVGL task: it makes no LVGL objects or timers
  itself and hands that work over with `lichen_ui_call` (the Timer app's poll, #242). With the lock-owner check on, a
  direct LVGL call there is reported (#225).
- `lichen_ui.h`: text you do not control (names, file names, device values) goes in `lichen_label_line` (one line,
  "...") or `lichen_label_wrap`, never a bare `lv_label_create`; `fw ui overflow` lists labels that do not fit (#133).
- Screen identity (#148): a screen is (`screen_id`, key); `shell_open` uses key 0. The shell never stacks a second copy of
  an identity that is already open: on top it does nothing, lower down it goes back to it (`shell_back_to`; refused
  while a dialog is open), so a screen may keep its state in file statics. A screen id that shows one of many things (a
  chat, a contact, a file) opens with `shell_open_keyed(build, id, key)`, the key from `shell_key_hash(bytes, len)` of what
  identifies it; different keys stack. `SHELL_KEY_UNIQUE` means always new (the state must then hang on the screen).
  Give each screen its own `screen_id` across the whole firmware: the same id with key 0 is the same screen.
- `lichen_ui.h`: navigation (`shell_open`, `shell_page_create`, `shell_back_then/_to`, `shell_stack_screen`, `shell_home`, `shell_on_esc`,
  `shell_on_search`, scroll helpers), the countdown dialog, `lichen_screen_locked()`. Screen code runs on the LVGL task;
  never swap the default group or the active screen yourself. `lichen_ui_call(fn, arg)` hands a result from any task
  to the LVGL task (fn runs there, soon; false when it could not be queued) and `lichen_ui_with_group(group, build,
  arg)` adds widgets to a screen's group (LVGL context). `shell_back()` pops the top screen like Esc, for a screen that
  takes Esc itself (the Keyboard test).
- `lichen_ui.h` for home tiles and clocks (#180): `lichen_icon_box(parent, w, h, x, y, color, radius)` is a filled box
  centred off the icon's middle (`draw_icon` builds its picture from them, `LV_RADIUS_CIRCLE` for a dot);
  `lichen_station_clock(screen, update_line2)` fills a screen with the screensaver's station clock (false without PSRAM
  for the dial: show the time another way) and `lichen_date_text(out, cap)` is its date line ("~ " while unverified).
- `lichen_keys.h` (#180): `lichen_keys_raw_take(fn, arg)` / `lichen_keys_raw_release(fn, arg)` give one app at a time
  every keyboard event below LVGL (shape keys, Fn, Ctrl), on the LVGL task, as `lichen_raw_key_t` (code, pressed,
  shift); no key acts as a normal key meanwhile, Esc included. Only for a screen that tests the keyboard; release it in
  the screen's delete event. `key_matrix.h` names the codes and holds the printed layout, `hold_exit.h` is hold-Esc-to-
  leave (both pure).
- `menu.h`, `menu_filter.h`, `list_filter.h`, `scroll_anchor.h`, `text_entry.h`: the shared list, filter and text
  widgets, so every app behaves the same under the keyboard. `text_entry_open(...)` returns `bool`: false when an entry
  is already open (there is one at a time; nothing is opened and `done` is never called), so a caller can say so.
- Shared UI widgets (#135; the UI standard's rules R1-R11). Apps must use these, not their own labels and hint lines:
  - Headings: `menu_create_named(parent, kind, name)` / `lichen_heading` put a small dim kind line ("Repeater settings")
    over the name in the large font on its own line (two lines, "..." only past that). `menu_create` stays for fixed
    titles ("Display"). Never write "Kind: name" in one line.
  - Row kinds: `menu_set_kind(row, MENU_OPEN | MENU_CYCLE | MENU_TOGGLE | MENU_DANGER | MENU_INFO)`. OPEN shows a dim
    "›" after the value, TOGGLE takes `menu_set_on(row, bool)` ("On"/"Off"), DANGER (`menu_set_danger(row,
    question, confirm_label)`) asks in the careful dialog before `on_enter` runs, INFO is dim and cannot be selected.
    A row with no kind acts at once.
  - `shell_page_hint(screen, "Enter", "open", "Backspace", "delete", NULL)`: a dim hint pinned to the screen bottom, not part
    of the scroll. No "Esc: back" pair; Esc is predictable.
  - `menu_status_begin / _progress / _done / _fail`: a status line that ends ("Reading 3 of 17", a final text, or a
    failure in plain words plus what to do). A begin that never ends is logged. `menu_empty(menu, "contacts", "they appear
    when a node adverts")` explains an empty list and goes away when a row is added.
  - `shell_dialog_t`: the dialog writes "(cancels in N s)" itself, leave `count_prefix`/`count_suffix` NULL; give
    `confirm_label` ("restart") and the hint "Enter: restart • Esc: cancel" is generated; the hint is dim unless `careful`.
  - The text and choices are pure (`ui_text.h`, `host_tests/ui_text_test.c`).
- `theme.h`, `fonts.h`: Lichen's colours, sizes and fonts. Use them instead of literals.
- `lichen_settings.h`: the app's own NVS namespace `a.<id>` (u8, u32, u64, strings, erase, erase all), and adopting a
  key an earlier firmware kept in `badge_fw` (`app_settings_adopt_u8/_u32`: copied once and read back, the old key
  erased on a later boot). Never open NVS.
- `lichen_fs.h`: the app's directory `/storage/<id>` and, with a card in, `/sd/lichen/apps/<id>`; drive sizes. Never
  name a path outside them. Every file call on the card is inside one batch: `lichen_fs_sd_begin(app, wait_ms)` (false:
  no card, or busy past `wait_ms`; then no file work and no end), the work, `lichen_fs_sd_end(app, errno_or_0)` (only
  EIO/ENODEV/ETIMEDOUT count toward the auto-unmount). `lichen_fs_sd_mounted()` is a lock-free check. Never from the
  LVGL task; get `lichen_fs_sd_dir` before the batch (it takes the card's lock itself). Never include `sdcard.h`.
- `lichen_notify.h`: raise a notification (toast, history, unread dot); from any task.
- `lichen_badge.h`: the count on the app's tile; set it whenever it changes.
- `lichen_haptics.h`: a short buzz of the vibration motor for feedback of your own; non-blocking, silent when the person switched it off. Notifications buzz by themselves, do not call it for them. Never drive GPIO8.
- `lichen_radio.h`: read the Wi-Fi/LoRa/Bluetooth switches and airplane mode, listen for changes. Apps never switch
  radios. An app that needs the LoRa radio for itself (the LoRa spectrum, #161) borrows it from MeshCore:
  `lichen_radio_lora_pause(&was_running, keep_going)` (blocks until MeshCore stopped; false when it did not or
  `keep_going()` turned false), then `lora_claim`; afterwards `lora_release` and, when `was_running`, always
  `lichen_radio_lora_resume()`. Both block: never from the LVGL task.
- `lichen_sensors.h`: read the latest IMU / environment / die temperature snapshot. Never talk to the chips or I2C.
- `lichen_crashlog.h`: the crash log, for the Crash log app; not from the LVGL task for writes.
- `lichen_task.h` (#181): `lichen_task_start(app, name, fn, arg, stack_bytes, prio, &handle)` with a priority class
  instead of a core and a number: `LICHEN_PRIO_BACKGROUND` (2, core 0), `_SERVICE` (3, core 0), `_JOB` (4, core 0),
  `_URGENT` (5, core 0, power off only) and `_APP_LOOP` (3, core 1 with LVGL, an app's main loop). Names are cut to 15
  characters (logged). `fn` ends by returning, never with `vTaskDelete(NULL)`: the OS takes it out of its book first.
  `fw tasks` lists every task with its owner app; after a stop `lichen_task_leftovers(app, wait_ms)` names what of
  the app still runs (MeshCore logs "MeshCore stopped: N task(s) left over").
  `lichen_task_start_in(..., prio, mem, &handle)` also picks the stack's memory (#269): `LICHEN_STACK_INTERNAL` (the
  default, right for nearly every task), `_LP_RAM` (the P4's 32 KB LP RAM: internal, so flash/NVS/`/storage` writes are
  fine, but not DMA-capable, slower and no PIE/SIMD: low-rate tasks only, and no large flash or SD I/O buffers on the
  stack, since esp_flash treats LP RAM as not-DRAM: reads bounce through a DMA buffer that can fail with
  `ESP_ERR_NO_MEM`, writes go in 32 B pieces) and `_PSRAM` (only a task that never touches flash, NVS, `/storage` or
  OTA; the first one logs that rule). Both are for tasks that **never return**: a return is logged as an error and
  parks the task for good (deleting it would need an internal-RAM helper task and aborts without one). A region that
  is short gives internal RAM instead; `fw tasks` shows each stack's memory.
- `lichen_mem.h` (#181): `lichen_alloc(app, size, LICHEN_MEM_BULK | LICHEN_MEM_INTERNAL [| LICHEN_MEM_ZERO])` and
  `lichen_free(app, ptr)`. BULK is PSRAM, for anything big; INTERNAL counts against the app's internal-RAM quota of
  64 KB (`MEM_BOOK_INTERNAL_QUOTA`) and is refused past it. `fw mem` lists what every app holds ("mem app <id> ...").
  Free with `lichen_free`, never `free()`, or the app's count stays up.
- `radio_state.h`, `tz_table.h`: types and the time zone table used by the headers above.
- `settings_store.h` is Lichen's own (`badge_fw`) and sits here only because `lichen_boot` needs it: apps do not use it.

### Other services

- `lichen_clock.h`: `clock_is_set()`, `clock_is_approximate()`, `clock_source_text()`; an app that learns the time
  (MeshCore from its contacts) calls `clock_propose(CLOCK_SRC_APP, tv)`. SNTP and a time set by hand outrank it and the
  clock never goes backwards.
- `lichen_random.h`: `lichen_random_fill()`, `lichen_random_u32()`. The OS keeps the entropy source on.
- `lora_app.h`: `lora_claim(owner, cb)` / `lora_release(owner)` (one owner at a time), `lora_send(owner, ...)` /
  `lora_send_state(owner)`, `lora_set_radio(owner, ...)` / `lora_set_tx_power(owner, ...)` (the owner's radio
  settings, checked by `lora_config_valid`), `lora_set_link_callback(owner, ...)`, `lora_set_rx_formatter(owner, ...)`,
  `lora_sweep(owner, ...)` (an RSSI sweep, receive only, blocking: never from the LVGL task, #161), and the read-only
  stats (`lora_up`, `lora_get_radio`, `lora_get_freq`, `lora_get_counts`, ...). Every call that takes `owner` is
  refused, with a rate-limited log line, unless that name holds the claim (#179). `lora_os.h` (start, stop, the radio
  switch, the C6 hooks) is the OS's: apps never include it.
- `fw_console.h`: `console_register` / `console_reply` for `fw <name>` commands; `console_reply_secret` for a line
  that holds a key or a password (only the one who asked gets it: the USB console, or the Wi-Fi console's reply; never
  the log store or the Console app, #123); `system_restart(why)` when a restart is what the user asked for (it records
  why). The Wi-Fi console (`POST /console`) is plain HTTP on the LAN, guarded by the update token only: a secret in its
  reply crosses the network unencrypted, as the token itself does.

## The two surfaces, the gate, capabilities and lifecycle

Layers and the call rules: [architecture.md](architecture.md) "Layers". The surfaces and the gate are in force since
OS v2 (#216); capabilities and the lifecycle are still to come.

**App surface:** `lichen_*.h` (and the helper headers listed above), what this page lists above. Available to every app.

**System surface:** `lichen_sys_*.h`, for the shell (L5) and `main`, never for an app: `forbid L6 lichen_sys_*` in
`tools/layers.conf` makes any app use a rule C violation (an include also fails rule B). The shell uses it instead of
reaching drivers, `board` or NVS. The headers, all in `lichen_api`:

| Header | Purpose |
|---|---|
| `lichen_sys_ui.h` | the LVGL lock (`lichen_sys_ui_lock`/`_unlock`) and `lichen_sys_ui_call` for the shell and main; the UI service owns the lock (#225); the lock-check hit count for the Stress app (#278) |
| `lichen_sys_display.h` | screen and keyboard backlight levels, the attention light, timeout and thermal shed; the levels reach the C6 over UART1 (#226) |
| `lichen_sys_power.h` | the display's share of power-off: both backlights off, then the panel, before the C6 is held in reset (#226) |
| `lichen_sys_storage.h` | the storage drive and the SD card: sizes, mount state, policy, for About, Files, SD card and notices (#227) |
| `lichen_sys_update.h` | the Wi-Fi update server's on/off setting and token state for Settings > Updates (#227) |
| `lichen_sys_crash.h` | whether this boot follows a crash (#227) |
| `lichen_sys_factory.h` | ask for a factory reset (settings or all): saved as pending, done by the next boot (#227) |
| `lichen_sys_mem.h` | heap figures (free, least free, largest block) for About, Benchmark and the boot log, and the diagnostic walk (#228); the Wi-Fi frame pools and every task's stack for the Stress app (#278) |
| `lichen_sys_clock.h` | the clock service's view for Time settings, the status bar, About and notices (#244) |
| `lichen_sys_net.h` | Wi-Fi and mDNS: saved networks, scan, connect state, the analyser's pure helpers (#244) |
| `lichen_sys_boot.h` | boot progress steps and the console log tap for the boot screen and the log store (#244) |
| `lichen_sys_keys.h` | raw key hooks, the screen-wake hook and the remote key gate (an app's raw keys are `lichen_keys.h`) (#244) |
| `lichen_sys_led.h` | the status LED setting for Settings > Notifications > Status LED (#244) |

`notify_os.h` (history of every app, do-not-disturb, toast queue, feedback hooks) is also the shell's side, forbidden
to apps by name in `layers.conf`; apps have `lichen_notify.h` only (#245). Types that live in a lower component are
repeated in these headers and `lichen_os` asserts they stay equal.

**One gate behind both:** the gate is its headers (`lichen_api`, L4, no hardware dependencies). The implementations are
`lichen_os` and `lichen_notify` (both L3, `peer ... lichen_api` in `layers.conf`; a call to a function a gate header
declares counts as a call to the gate). The shell's navigation goes through the ops table the shell fills at boot
(`lichen_ui_ops_t`), so `lichen_api` does not depend upward. `tools/gate_manifest.txt` lists every declared function
with its capability and thread rule, and the build checks that it matches the headers. No syscall trap.

**Capabilities (#229, still to come):** `LORA_TX`, `LORA_RX`, `LORA_BORROW`, `SD`, `NET`, `SECRETS`, `RAW_KEYS`,
`CONSOLE`, `RESTART`, `BACKGROUND`, `HAPTICS`, `SENSORS`, `NOTIFY`, `RADIO_SCAN`, `BT`, `AUDIO`, `BATTERY`, the quotas
`INTERNAL_RAM=` and `PSRAM=`, and `SYSTEM`. Today the manifest's capability column is only a per-header seed.
- To be declared per app in `tools/layers.conf` (reviewed like code) and mirrored in `lichen_app_t`.
- To be checked at build time (`nm`: every symbol an app uses maps to a capability it declared) and at run time (a call
  without the capability is refused and logs a line).
- **An accident and audit guard, not a security boundary:** apps are compiled in and share the address space.
- Identity: an opaque `lichen_ctx_t` handed out at registration, plus the calling task's TLS (#230).

**Lifecycle (#231, after the MVP):** an app is registered, started, running, suspended (screen off or another app in
front), stopping (cooperative, with a deadline; the OS never deletes an app's tasks) and stopped. On stop the OS frees
the app's grants whole (memory, PSRAM quota, booked timers). Two crashes attributed to one app in three boots turn its
background part off and notify (#232). Settings > Apps shows state and capabilities (#233). Today the OS already books
every app's tasks and allocations (`lichen_task.h`, `lichen_mem.h`; `fw tasks`, `fw mem`) and
`lichen_task_leftovers` names what runs after a stop.

**Not in v2:** loadable apps. A future "Scripts" app may host WAMR (WebAssembly) modules, each with its own
capabilities (#78).

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
  task renders corrupted the heap, #118; `fw ui lockcheck` counts such calls). Nor set the default group: use
  `lichen_ui_with_group`.
- Pick a core and priority of its own for a task, or call `xTaskCreate*`: use `lichen_task_start` (#181; the boundary
  check refuses `xTaskCreate*`). Nor place memory with `heap_caps_*`: use `lichen_alloc`.

## The wraps

Six calls are wrapped at link time (`-Wl,--wrap=`). The OS's own legitimate caller reaches the real function through
`__real_*`; every other caller lands in the wrap, which logs an `E` line with the caller's return address (look it up
in `firmware/build/badge_fw.map`) and refuses:

| Call | Wrap in | Allowed | Refused with |
|---|---|---|---|
| `settimeofday` | `lichen_clock.c` | `lichen_clock` (through `__real_`); any call that writes back the time the clock already shows (under 1 s off, `clock_resync_only`): that is IDF's `esp_sync_timekeeping_timers` on every restart and before sleep | `-1`, `errno = EPERM` |
| `nvs_flash_erase` | `console.c` | nobody: the OS erases by partition (`main.c` on an unusable layout, `factory_reset.c`, past the wrap below); MeshCore's `DataStore` is built with the name changed to its own wipe | `ESP_ERR_NOT_ALLOWED` |
| `bootloader_random_enable` | `lichen_random.c` | `lichen_random_start` (through `__real_`). IDF's `nvs_sec_provider` (`CONFIG_NVS_ENCRYPTION`, off, stays off: its init burns an eFuse key by itself, #116) would be refused, so turning it on fails the build; Lichen's own encrypted NVS (`lichen_nvs`) does not call it | nothing happens (void) |
| `bootloader_random_disable` | `lichen_random.c` | nobody today (no caller in the image); the OS's future ADC hand-over (#11, #69) goes through `__real_` in `lichen_random.c`. `nvs_sec_provider` again: covered by the same build error | nothing happens (void) |
| `nvs_flash_erase_partition` | `console.c` | `main.c` (NVS unusable at boot), `factory_reset.c` (#66, the full erase) and `lichen_nvs.c` (#116, the one-time migration to encrypted NVS, only with `LICHEN_NVS_ENCRYPT`), all through `__real_` | `ESP_ERR_NOT_ALLOWED` |

`esp_restart` is the sixth wrap (`console.c`), but only to record the caller: a restart is never refused, so it has no row above.

## Exceptions still open

No component or header exception is open: MeshCore's identity hand-over goes through `lichen_legacy_blob_take`
(`lichen_legacy.h`: the OS reads the old `badge_fw/identity` blob, the app stores it, and only then is it erased) and the
message journal's SD batches through `lichen_fs_sd_begin`/`lichen_fs_sd_end` (`lichen_fs.h`). The journal still lives at
`/sd/lichen/messages` instead of `lichen_fs_sd_dir`: moving the history is a separate decision. MeshCore's `DataStore`
includes `<nvs_flash.h>`; `shared/arduino_shim/nvs_flash.h` stands in for it.

Also open from #84: the console prefix per app (T8b) and system apps calling `board/backlight.h` directly (V13).
`lichen_task_start` (T8a) and the heap caps wrapper `lichen_alloc` (V12) landed in #181; MeshCore's chat rings
(`store_alloc_ring` in `mesh_task.cpp`, freed with `free()` by the pure `mesh_store.c`) and the Arduino shim's
`fs_shim.cpp` still call `heap_caps_*` directly.

## Boundary check

The layer checker `tools/check_layers.sh` runs at the end of `firmware/build.sh build` (after the IDF build) and fails
the build with `file:line` and the rule for any violation: the baseline `tools/layers-baseline.txt` is empty (0, #237)
and may not grow (#218; `BADGE_LAYERS_ALLOW_GROW` is the one-off hatch, architecture.md "Enforcement"). Apps are
layer L6, a closed profile in `tools/layers.conf` (allowed peers, libraries, system headers, own include roots and
forbidden symbols); a new app component is added there with `component <name> L6`. `tools/check_app_boundary.sh` runs
just that L6 profile.

- Rule A: the app's `REQUIRES`/`PRIV_REQUIRES` (variables expanded) must be on the allow-list above.
- Rule B: every `#include` in the app's sources is its own or bundled third-party header, a header of an allowed
  component, or a listed libc/plain-library header. `#include "backlight.h"` fails here.
- Rule C: `nm -u` on the app's built archive may not show `esp_ota_*`, `esp_wifi_*`, `esp_efuse_*`, `gpio_*`,
  `esp_partition_*`, `nvs_flash_init*`/`nvs_flash_erase*`, `ledc_*`, `i2c_*`, `esp_restart` or `xTaskCreate*` (apps use
  `lichen_task_start`, #181).

`check_layers.sh` (#218) generalises this check to every layer and absorbs these rules. Rule C also forbids the
system surface (`lichen_sys_*`), `lichen_ui_ops_set` and the shell's side of notifications to apps.

The eight plain apps (Timer, Calculator, QR code, Clock, Keyboard test, LoRa spectrum, Weather, Notepad) live in the
`lichen_apps` component (#180) and are checked as layer L6, like `mesh`. The system apps in `lichen_shell` are not.
`lichen_apps` is the only component allowed `dirent.h` (Notepad lists a directory; `lichen_fs` has no listing call yet).
The navigation calls (`shell_open` and friends) are the gate's own since #221: they forward to the shell's ops table.

## New services

Each new OS service follows the same shape: a `lichen_<thing>.h` in `lichen_api` (or a component `lichen_api`
REQUIRES), the OS owning the hardware and state behind it, apps reading snapshots and asking for changes, callbacks
short and on the caller's task. Notifications (#62), the radio switches (#108), sensors (#69) and tile badges (#102)
are the examples; power (#65), audio/haptics/LED (#67, #68, #74) and input (#79, #81) go the same way.
