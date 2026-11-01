# Lichen OS

Lichen OS is a new operating system for the WHY2025 camp badge, replacing the badge's original BadgeVMS. It turns
the badge into a standalone
[MeshCore](https://github.com/meshcore-dev/MeshCore) messenger, with Wi-Fi, a home screen and settings around it.

Like a lichen, which is a fungus and an alga living as one, the badge has two chips that work as one:

- **ESP32-P4 (Lichen):** screen, keyboard, storage, the apps and all the UI. One plain ESP-IDF 6.1 application.
- **ESP32-C6 (radio coprocessor):** Wi-Fi and BLE through [ESP-Hosted](https://github.com/espressif/esp-hosted-mcu),
  plus our LoRa service for the SX1262. It has no application logic. The P4 flashes it at boot when its image
  changes.

Apps are built in: **MeshCore** (the first app), Settings and About Lichen. You can't download apps. More on the design
in [docs/architecture.md](docs/architecture.md) and [docs/shell-design.md](docs/shell-design.md).

On the badge it's simply called Lichen. It started as a fork of
[BadgeVMS](https://codeberg.org/Peetz0r/why2025-badge-firmware-meshcore), the badge's original OS, and replaced it
completely. A few files still come from BadgeVMS, see [Acknowledgements](#acknowledgements).

## What you need

- A WHY2025 badge.
- **Two bodge wires to the LoRa module.** The schematic draws an Ra-01 (SX1276), but an Ai-Thinker RA-01SH (SX1262)
  is fitted, and its DIO1 and BUSY pads are not routed. Without the wires the radio answers over SPI, but MeshCore's
  radio init fails.

  | RA-01SH pad | Signal | ESP32-C6-WROOM-1 module pin | C6 GPIO |
  |---|---|---|---|
  | 6 | DIO1 | 5 | GPIO5 |
  | 10 | BUSY | 12 | GPIO11 |

  Count pads from the RA-01SH datasheet, not the KiCad symbol. Module pin 12 is GPIO11; GPIO12 is pin 13 (USB D-).
  Check the neighbours for bridges: module pins 4 and 6 (LoRa NSS, SCK) and 13. Background and a photo:
  <https://wiki.why2025.org/Project:Meshtastic_on_the_WHY2025_badge>.
- **An external 868 MHz antenna before any transmit.** The stock antenna is poorly matched at 868 MHz (SWR about 1.7
  even after tuning), and MeshCore transmits at up to 22 dBm.
- To build: Linux, [ESP-IDF 6.1](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32p4/get-started/index.html),
  CMake, Ninja and a host C/C++ compiler (for the host tests).

## Build

Lichen needs exactly ESP-IDF 6.1. Keep a 6.1 checkout next to your usual one and point `BADGE_IDF_PATH` at it:

```
git clone --recurse-submodules https://github.com/lvschouwen/lichenos-why2025-badge.git
git -C ~/esp/esp-idf worktree add --detach ~/esp/esp-idf-v6.1 v6.1
(cd ~/esp/esp-idf-v6.1 && git submodule update --init --recursive && ./install.sh esp32p4,esp32c6)
git -C ~/esp/esp-idf-v6.1 apply "$PWD/tools/idf-patches/0001-esp32p4-psram-120mhz.patch"
BADGE_IDF_PATH=~/esp/esp-idf-v6.1 ./firmware/build.sh build
```

The patch adds a 120 MHz PSRAM clock to the IDF (its own choices are 80 and 200; see `tools/idf-patches/` for why and
the license note). `build.sh` refuses to build without it while `sdkconfig.defaults` asks for 120 MHz.

`firmware/build.sh build` runs the host tests (`host_tests/`), builds the C6 image (`c6/`), puts it on Lichen's
storage drive together with `firmware/lora.toml`, and builds the P4 image into `firmware/build/`. Any other arguments go
to `idf.py`. A `sdkconfig` older than its `sdkconfig.defaults` is regenerated (the old one is kept as
`sdkconfig.old`). The version name comes from `git describe` at configure time, so run
`./firmware/build.sh reconfigure build` after a commit.

If you cloned without `--recurse-submodules`, run `git submodule update --init --recursive`.

## Install and update

The first install goes over USB. `tools/badge.sh` drives a badge that is plugged into another machine reachable over
SSH (see [Flashing through a remote USB host](#flashing-through-a-remote-usb-host)):

```
tools/badge.sh backup <port>                  # full flash dump into ~/badge-backups: do this first
tools/badge.sh flash-fw <port>                # first install: bootloader, partitions, Lichen, assets and storage
tools/badge.sh flash-fw <port> --no-storage   # later USB updates: keeps /storage
```

Leave out `--no-storage` only on the first install: it rewrites `/storage`, and with it MeshCore's identity, contacts
and channels.

After that, update over Wi-Fi (the update server runs whenever Wi-Fi is up; put the token from `fw token` in
`~/.config/why2025-badge/ota-token`):

```
tools/badge.sh ota-firmware <ip>                       # firmware/build/badge_fw.bin, then reboot
tools/badge.sh ota-file <ip> <file> FLASH0:<name>      # one file into /storage
tools/badge.sh ota-assets <ip>                         # firmware/build/assets.bin (fonts), then reboot
tools/badge.sh ota-c6 <ip> [dir]                       # C6 bundle (default firmware/storage_staging/c6), then reboot
tools/badge.sh ota-info <ip>                           # the version the badge runs
```

`ota-c6` checks that the six C6 files agree (each `.md5` against its `.bin`), sends every `.bin` right before its
`.md5` (the badge refuses a pair that disagrees), reboots only after all six arrived and waits for Wi-Fi. Use it when
`c6/` changed; the next boot reflashes the C6. If it stops half way the badge was not rebooted: run it again.

The tool first asks the badge to prepare (`POST /prepare`: MeshCore and the other apps stop and free their memory),
then pushes the image. The badge shows **Updating Lichen** with progress and reboots by itself. A new image runs on
trial and marks itself valid once the screen and the C6 link are up. If it crashes or never gets there, the next reset
goes back to the previous image, and home says "Update failed, back on <version>". Uploads are checked against a
SHA-256 before they replace anything. The token travels as plain HTTP, so only use this on a network you trust.

**Back to stock BadgeVMS:** `tools/badge.sh restore <port> <backup> --yes` writes your backup back (USB).

Partitions (16 MB flash, `firmware/partitions.csv`):

| Partition | Offset | Size | Holds |
|---|---|---|---|
| `ota_0`, `ota_1` | 0x20000, 0x420000 | 4 MB each | the two firmware slots |
| `coredump` | 0x820000 | 128 KB | the last crash |
| `assets` | 0x840000 | 3 MB | read-only files such as fonts, packed from `firmware/assets/` by `tools/pack_assets.py` and read into PSRAM at boot, never memory-mapped (#31) |
| `storage` | 0xB40000 | 4.75 MB | FAT, mounted at `/storage`: the C6 image, settings files, MeshCore's data |

Assets are separate from the firmware: `flash-fw` writes them, and over Wi-Fi `ota-assets` replaces them (checked before
anything is erased, then read back). If they are missing or damaged, Lichen logs it and falls back to its built-in
fonts; `fw assets` shows their state.

## Using the badge

- **Boot:** the Lichen logo, the start-up steps, version and uptime. A step that fails is named.
- **First run:** a welcome screen, then a Wi-Fi scan to join a network (skippable). It shows once (`fw setup reset`
  shows it again after a reboot).
- **Home:** one tile per app with a line or two of live text, e.g. MeshCore's unread count and node count. A switch in
  Settings turns the tiles into a grid of icons.
- **Status bar:** local time (`~14:05` in amber while it only comes from mesh contacts, `--:--` when the badge has no
  time: it has no backup battery, so a power-off loses it), Wi-Fi bars (red when the link is down) and the LoRa radio
  state.
- **Settings:** Wi-Fi (scan, join; a new network is tried first and the badge goes back to the old one if it doesn't
  connect), Display (brightness, screen timeout: dim after 1 min, off after 5 min; home layout), Time (set date & time by hand, time zone with the Netherlands as the
  default, where the time came from), About Lichen, and one page per app that has settings. SNTP still wins over a
  time set by hand; `fw clock set <ISO8601>` and `fw clock sync` do the same from the console.
- **Settings > MeshCore:** Start at boot (on by default; off leaves the LoRa radio unclaimed, the status bar's LoRa dim,
  and the MeshCore tile says "Not running / Enter to start": Enter starts it); Start/Stop MeshCore; name; Region &
  radio (presets such as Netherlands, the default, plus Custom: frequency,
  bandwidth, SF, CR and, under Advanced, TX power); Channels (add a name with a base64 key or `#hashtag`, remove one;
  Public stays); Send advert; node id, radio, TX power and MeshCore version. The first time the MeshCore app opens it
  asks for a name and a region.
- **MeshCore app:** opens on its own grid of tiles (Chat, Contacts, Repeaters, Heard, Advertise, Settings) with badges
  (unread, contacts, repeaters, last heard, last advert). Arrows move, Enter opens, Esc goes back to the grid and from
  there to Lichen home.
- **Keyboard:** arrows and Enter move and select, Esc goes back one screen (blocked while a firmware update runs),
  Tab moves between rows, Backspace edits text.

MeshCore keeps its files in `/storage/meshcore` (identity, made on first boot; contacts; channels), so they survive
reboots and firmware updates. Limits: 1000 contacts, 40 channels (Public included), 256 queued messages. The screen and
`fw msgs` show the last 200 messages, which are kept in memory only. The clock comes from SNTP once Wi-Fi is up;
adverts sent before that carry a wrong timestamp. Text uses Lichen's own fonts from the assets partition (Latin, Cyrillic, Greek, monochrome emoji; regenerate with
`tools/make_fonts.py`); a character none of them has shows as a box.

The LoRa settings Lichen starts with are in `/storage/lora.toml` (`frequency_mhz`, `bandwidth_khz`,
`spreading_factor`, `coding_rate`, `sync_word`, `preamble`); the default is the Dutch MeshCore preset, 869.618 MHz,
62.5 kHz, SF7, CR 4/5.

## Screenshots

```
tools/badge.sh screenshot <ip> [out.png]    # default: a private (mode 700) temp dir; prints the path
```

While the badge is on Wi-Fi it renders the active screen, the status bar and the update banner (LVGL's snapshot, upright,
720x720) and sends it as a BMP, which the tool turns into a PNG (needs python3-pil). It needs the update token like
every other Wi-Fi command and is never logged. The screen can show private messages and contacts: keep the files out
of the repo and out of issues. A busy UI answers 503 (try again); during a firmware update it answers 409.

## Log and console over Wi-Fi

```
tools/badge.sh log <ip> [--follow]     # the badge's log since boot; --follow polls every second
tools/badge.sh fw <ip> <command...>    # e.g. fw 192.0.2.7 mem, fw 192.0.2.7 ui, fw 192.0.2.7 keys send down enter
```

The badge keeps every log line and every console reply in a 96 KB ring in PSRAM (`GET /log?since=<seq>`: lines with a
sequence number and uptime; the answer's `X-Next-Seq` header is the number to ask for next, `X-Log-Missed` counts lines
that were dropped from the ring before you asked). `POST /console` runs one `fw` command line (at most 95
characters) like the USB console would and returns its output; commands that restart the badge (`fw reboot`, `fw slot
other`, a C6 reflash) answer first. Both need the update token like every other Wi-Fi command, and the log and console
output can show node names and messages, so treat them like screenshots. Not in the log: lines printed by ROM code
(panics, the display underrun note, the bootloader), plain `printf` output (`fw crash dump`, the C6 flasher's progress)
and anything before `app_main`; `fw token` never enters it. Control characters and invalid UTF-8 in the log are shown as `?`, and `badge.sh` strips terminal control codes from what it prints. `fw keys on` writes every key the screen receives into the log (typed text included; it skips password fields, but treat the log as sensitive while it is on). When the ring is busy a line can be dropped; the next stored line then reads `(N log lines lost)`. The console answers 409 during a firmware update. To
switch the Wi-Fi console off, disable `OTA_WIFI_CONSOLE` (menuconfig, "Lichen Wi-Fi update server"; the log stays).

## Console and crashes

The serial console (115200 baud) takes `fw <command>` lines, e.g. `tools/badge.sh console <port> "fw help"`:

- Wi-Fi and device: `fw wifi [scan | reconnect | <ssid> <password> | --clear]`, `fw net`, `fw token`, `fw mem` (heaps,
  task stacks, per-task core and CPU %), `fw slot [other]`, `fw assets`, `fw reboot`, `fw bl <display> <keyboard>`,
  `fw idle [test <s>]` (screen timeout; `test` fakes s seconds without a key).
- Sensors: `fw sensors` (BMI270 accel and gyro, BME690 temperature, humidity, pressure and gas, P4 die temperature),
  `fw sensors raw` (chip ids, status registers, counters), `fw sensors start|stop`, `fw i2c scan`. The BME690's gas
  heater (300 C for 100 ms each second) and the badge's own warmth make its temperature read somewhat above the room
  and humidity somewhat below; menuconfig "Lichen sensors" can shorten the heater or switch it off (no gas reading).
- LoRa and the C6: `fw lora [raw on|off]`, `fw c6log [ms]` (the C6's own console, up to 60 s), `fw c6 reflash`, `fw c6` (link state, recoveries), `fw c6 reattach` (re-attach the C6 now), `fw c6 reset [rom]` (pulse C6 EN to test the recovery; `rom` only with someone at the badge).
- MeshCore: `fw mesh` (identity, clock, packet counters; or why it is not running), `fw mesh start`, `fw mesh stop`,
  `fw mesh name <name>`, `fw mesh radio <preset | freq bw sf cr>
  [txdbm]`, `fw contacts`, `fw msgs [n]`, `fw channels`, `fw channel add <name> <base64 key>` or `fw channel add #name`
  (hashtag channel: key = first 16 bytes of SHA-256 of the lowercased name, `#` included), `fw channel rm <index|name>`,
  `fw advert` (send a zero-hop advert now).
- Crashes: every crash keeps a core dump and a log line. `fw crash` shows them, `fw crash dump` prints the dump,
  `fw crash clear` drops it. `tools/badge.sh crash <port> [elf]` fetches the dump and decodes it against the crashed
  image's ELF; `flash-fw` and `ota-firmware` keep every ELF in `~/badge-backups/elf` under its SHA-256 and version.
- Bench: `fw boot [fail <step>]`, `fw home layout tiles|grid`, `fw setup reset [mesh]`, `fw keys on|off|send <keys>` and
  `fw ui` (press keys and read the screen without looking at it), `fw uicheck` (walk every LVGL object now, #89), `fw crash test panic|wdt`.

## Flashing through a remote USB host

`tools/badge.sh` works on a badge plugged into another machine reachable over SSH (set its SSH name in
`~/.config/why2025-badge/host` or `BADGE_HOST`). That host needs `esptool` 5.x and `pyserial` in `~/badge-venv`. The script only touches Espressif
USB devices (vendor ID `303a`); a badge behind a generic USB-serial bridge such as a CH340 must be named explicitly with
`BADGE_EXTRA_PORTS=/dev/ttyUSB1`. Over a CH340, keep the baud rate at the default 460800 (`BADGE_BAUD`): 921600
corrupts reads. A full 16 MB backup takes about 7 minutes. Run `tools/badge.sh` without arguments for all commands.

## Repository layout

| Path | What |
|---|---|
| `firmware/` | Lichen, the P4 firmware (ESP-IDF project; components under `firmware/components/`) |
| `c6/` | The C6 image: ESP-Hosted 3.0 plus the LoRa service and backlight control |
| `shared/` | Code both chips and the host tests use: the LoRa protocol, the Arduino shim MeshCore runs on |
| `host_tests/` | Host tests for the pure logic (run by every build) |
| `tools/badge.sh` | Backup, flash, console, crash decoding and Wi-Fi updates |
| `meshcore/` | MeshCore (submodule: our fork, upstream releases plus badge commits) |
| `esp_hosted/` | ESP-Hosted MCU for the P4 (submodule: our fork, 3.0.10 plus an SDIO receive fix) |
| `third_party/` | rweather's Crypto and base64 (submodules), used by MeshCore; Bosch's BMI270 and BME69x SensorAPIs (submodules) |
| `docs/` | Architecture and shell design |

## Acknowledgements

Lichen stands on a lot of other people's work. Thank you!

| Project | Used for | License |
|---|---|---|
| [BadgeVMS](https://codeberg.org/Peetz0r/why2025-badge-firmware-meshcore) by HP van Braam and contributors (see [AUTHORS](AUTHORS), [THANKS](THANKS)), with Peter Hazenberg's MeshCore work | Where Lichen started: the C6 flasher glue, the keyboard tables, the console command splitter, the Wi-Fi update protocol | GPL-3.0 |
| [MeshCore](https://github.com/meshcore-dev/MeshCore) | The mesh protocol and the companion radio code the MeshCore app runs | MIT |
| [ed25519](https://github.com/orlp/ed25519) by Orson Peters (in MeshCore) | MeshCore's identity keys | Zlib |
| [ESP-Hosted MCU](https://github.com/espressif/esp-hosted-mcu) | Wi-Fi and BLE through the C6 | Apache-2.0 |
| [ESP-IDF](https://github.com/espressif/esp-idf) and Espressif's components (esp_lvgl_port, esp-serial-flasher, esp_wifi_remote, i2c_bus, USB host drivers) | The base of both firmwares | Apache-2.0 |
| [LVGL](https://github.com/lvgl/lvgl) 9.6 | The user interface (with the Montserrat font, OFL-1.1) | MIT |
| [DejaVu Sans](https://dejavu-fonts.github.io), [Noto Emoji](https://github.com/googlefonts/noto-emoji) | Glyphs merged into Lichen's fonts (Bitstream Vera license, OFL-1.1) | free font licenses |
| [RadioLib](https://github.com/jgromes/RadioLib) 7.8 | The SX1262 driver on the C6 | MIT |
| [rweather/arduinolibs](https://github.com/rweather/arduinolibs) Crypto | Ed25519, AES and SHA for MeshCore | MIT |
| [base64_arduino](https://github.com/Densaugeo/base64_arduino) | Base64 for MeshCore | MIT |
| [tomlc17](https://github.com/cktan/tomlc17) | Reading `lora.toml` | MIT |
| [BMI270 SensorAPI](https://github.com/boschsensortec/BMI270_SensorAPI), [BME69x SensorAPI](https://github.com/boschsensortec/BME69x_SensorAPI) (Bosch Sensortec) | The IMU and environment sensor drivers | BSD-3-Clause |
| esp_lcd_st7703 (Espressif) | The display panel driver | Apache-2.0 |
| [esp_tca8418](https://github.com/atanisoft/esp_tca8418) by Mike Dunston | The keyboard controller driver | BSD-2-Clause |

The WHY2025 badge team made the hardware. The LoRa bodge comes from the WHY2025 wiki page linked above.

The table names the main projects. ESP-IDF also brings in code from others (FreeRTOS, lwIP, TLSF, FatFs, protobuf-c,
Picolibc, wpa_supplicant and more), and LVGL brings in Marco Paland's printf and the Font Awesome symbols.
[THIRD_PARTY_LICENSES.txt](THIRD_PARTY_LICENSES.txt) lists everything linked into each image, with its license text.

## License

GPL-3.0, see [COPYING](COPYING), because Lichen started from BadgeVMS. The third-party code keeps its own license; all
of it is GPL-3.0-compatible.

The C6 image is the exception: it is a separate program on its own chip and links Espressif's binary-only Wi-Fi,
Bluetooth and PHY libraries, so our code in it (`c6/main`, `shared/lora`) is Apache-2.0, as each file's SPDX header
says.

Every released image ships with [THIRD_PARTY_LICENSES.txt](THIRD_PARTY_LICENSES.txt), because the MIT, BSD, Zlib, ISC and
OFL licenses ask for their notice in binary copies too. `tools/third_party_licenses.py` builds that file from the real
license files, and `firmware/build.sh` stops with an error when it no longer matches the dependencies. When you add
or enable a library (an LVGL decoder, say), add it to the list in the script and run it again.
