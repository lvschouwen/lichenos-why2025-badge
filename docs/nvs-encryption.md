# NVS encryption (#116)

Status: the code is in and switched **off**. Nothing is burned by any build in this repository unless you
follow the procedure below by hand. The default image contains no code that writes an eFuse.

> **The burn rule.** Burn KEY5 only once `fw nvs-slots` says `both slots guarded encrypt images` (both OTA slots hold
> a complete, guarded `LICHEN_NVS_ENCRYPT=y` image). **A burned badge must never get a pre-guard image** (anything
> built before the guard, i.e. any image `fw nvs-slots` shows as `no mark` or `NO guard`), over USB or Wi-Fi: its
> plain `nvs_flash_init()` erases the encrypted NVS entry by entry on its first boot.

Why: the Wi-Fi password, the repeater and room passwords (`pw<12 hex>`, #51), the OTA token and every setting sit
readable in the `nvs` partition. Anyone with the badge and a USB cable can `esptool read-flash` them. Flash
encryption is not part of this (it would also protect the MeshCore identity in `/storage`, but costs boot time,
complicates USB flashing and OTA, and its release mode is irreversible).

## The scheme

NVS encryption with the HMAC-derived key scheme (the one IDF calls `CONFIG_NVS_SEC_KEY_PROTECT_USING_HMAC`):

- One random 256-bit key is burned into an eFuse key block with purpose `HMAC_UP` (HMAC upstream). The burn
  read-protects the block, so software and `espefuse` can never read the key back; only the HMAC peripheral can use it.
- At boot the firmware asks the HMAC peripheral for `HMAC(key, seed)` twice (IDF's two seeds `0xAEBE5A5A` and
  `0xCEDEA5A5`, repeated to 32 bytes). The two results are the XTS-AES key pair that encrypts the NVS entries.
  Same derivation as IDF's own scheme, so IDF tools can read the partition too.
- XTS-AES encrypts the 32-byte entries. The page header and the entry-state table stay in clear; that is how
  `lichen_nvs` tells a plaintext partition from an encrypted one without a key (the CRC of the first written entry of
  each page, `host_tests/nvs_image_test.c` against real IDF-made images).
- No `nvs_keys` partition is needed (that is the flash-encryption scheme), so **the partition table does not change**.

## What gets burned

- **Block: `KEY5`** (`CONFIG_LICHEN_NVS_HMAC_KEY_ID`, default 5), the last of the six key blocks `KEY0..KEY5` of the
  ESP32-P4, so `KEY0..KEY4` stay free for secure boot or flash encryption later.
- **Purpose: `ESP_EFUSE_KEY_PURPOSE_HMAC_UP` (value 8).**
- Written by `esp_efuse_write_key()`: key, purpose, read-protect and write-protect bits in one batch.
- Secure boot and flash encryption are off in this firmware (`firmware/sdkconfig`), so every key block is unused as far
  as the firmware's configuration goes. `fw nvs-enc burn` still checks `esp_efuse_key_block_unused()` on the real chip
  and refuses otherwise. `CONFIG_EFUSE_VIRTUAL` is not set: the burn is real.

### IRREVERSIBLE

An eFuse bit cannot be unset. After the burn, KEY5 is the NVS key for the life of this chip: it can not be erased,
changed or reused for another purpose. Lose the NVS contents and nothing is recoverable from the key side (the key
is fine, the data is gone), but there is no way back to "no key". Burn only after Lucas has OK'd it at the badge.

## Ways the IDF burns a key by itself (and why this repo avoids them)

With `CONFIG_NVS_ENCRYPTION=y` plus the HMAC scheme, IDF's `nvs_flash_init()` does this
(`components/nvs_flash/src/nvs_api.cpp:187-195`): read the keys; if that fails, log "Generating NVS encr-keys" and call
the scheme's key generator, which (`components/nvs_sec_provider/nvs_sec_provider.c:172-181`) fills 32 random bytes and
calls `esp_efuse_write_key(..., ESP_EFUSE_KEY_PURPOSE_HMAC_UP, ...)` **if the configured block does not already hold an
HMAC_UP key**. So on a badge with an empty block, the first boot of such a build burns the eFuse silently.

Therefore Lichen keeps IDF's `CONFIG_NVS_ENCRYPTION` off (`lichen_random.c` has a `#error` if somebody turns it on) and
does its own init, `lichen_nvs_init()` (`firmware/components/lichen_nvs/`), which calls the plain `nvs_flash_secure_init`
with keys derived from an existing eFuse key:

| Situation | What `lichen_nvs_init()` does |
|---|---|
| `LICHEN_NVS_ENCRYPT` off (default), no HMAC_UP key in KEY5 | exactly `nvs_flash_init()`; no secure init, no burn code compiled |
| off (default), **key present** | the guard: one E line `HMAC key burned in KEY5 but this image has no NVS encryption: NVS not initialised, nothing erased`; no `nvs_flash_init()`, NVS stays down |
| on, no HMAC_UP key in KEY5 | plain `nvs_flash_init()`, a W log line. **Never creates a key.** |
| on, key present, partition empty or encrypted | `nvs_flash_secure_init()` with the derived keys |
| on, key present, partition plaintext | one-time migration (below) |
| on, key present, HMAC derivation fails | encrypted partition: NVS unavailable (error returned, nothing erased); plain partition: plain init |

Every build reads the KEY5 purpose (read only, `esp_efuse_get_key_purpose`) before touching NVS: the guard.
`nvs_boot_init(encrypt_build, key_burned)` in `nvs_boot_policy.c` picks plain init, the keyed path, or (a build without
the option on a burned chip) no init at all. `LICHEN_NVS_HMAC_KEY_ID` no longer depends on `LICHEN_NVS_ENCRYPT`, so the
default build reads the same block (5).

### What boot does with the result (`nvs_boot_policy.c`)

`main.c` hands every `lichen_nvs_init()` result to `nvs_boot_next()` together with the path that init took
(`lichen_nvs_boot_path()`: KEYED when it found an HMAC_UP key in KEY5, in any build). Pure, `host_tests/nvs_boot_policy_test.c`.

| Path | Result | Boot does |
|---|---|---|
| either | `ESP_OK` | NVS up |
| plain (option off, or no key) | `NO_FREE_PAGES` / `NEW_VERSION_FOUND` | erase once, init again (as before); a second failure stays down |
| plain | anything else | stays down, nothing erased |
| keyed (key burned) | **any error**, the two layout codes included | stays down, **nothing erased**: E line `NVS init failed: <err> (HMAC key burned: nothing erased, settings unavailable this boot)` |

A badge that stays down boots without settings (defaults, no Wi-Fi password, Wi-Fi updates refused, see below);
`fw nvs-enc` then says `this boot down (nothing erased)`. The data is intact in flash for a later boot or a USB
restore of the backup. To start over on purpose: `esptool erase-region 0x9000 0x6000` over USB; the next boot finds an
empty partition and opens it encrypted.

Factory reset (`factory_reset.c`) keeps its own erase-and-retry: it erases NVS on purpose anyway.

**Seeing the down boot on the bench** (no key needed): build with `CONFIG_LICHEN_NVS_DEBUG_FORCE_DOWN=y` (default n,
an sdkconfig override, never `sdkconfig.defaults`). `fw nvs-down YES` from the USB console (refused over Wi-Fi or
without the exact `YES`) sets a flag in no-init RAM and restarts; the next boot takes the KEYED "stays down" path once
(E line `nvs-down (bench): this boot without NVS, nothing initialised or erased`), without initialising or erasing
NVS. The boot after is normal. Nothing is written to flash or eFuse; a power cut clears the flag.

### Wi-Fi updates without a token

The OTA token lives in `/storage/ota_token.txt` with a copy in NVS (`ota_token`). If neither can be read nor written this
boot (NVS down and no `/storage`), the update server does not start: one E line `Wi-Fi updates refused this boot: no
saved token (no /storage, no settings); USB flashing works`, `fw ota` says `update server over Wi-Fi: refused this boot
(no saved token); USB flashing still works`, `fw token` shows none, `ota_server_enabled()` is false and Settings › Updates over Wi-Fi shows `Off (no token)`.
Before, the server listened with a fresh random
token that no tool knew and the next boot replaced. With NVS down but `/storage` mounted the file token still works.
USB flashing (`badge.sh flash-fw`) never needs the token. Pure part: `ota_token_kept()` and `ota_server_listens()` in `ota_proto.c`.

### Both OTA slots: `fw nvs-slots`

Every image now carries a 12-byte mark (`nvs_mark.h`) in IDF's custom app description section
(`.rodata_custom_desc`), which the linker places right after `esp_app_desc_t`: offset 288 from the start of the slot.
Tag `LICHENNV`, version 1, flags bit 0 = built with `LICHEN_NVS_ENCRYPT`, bit 1 = with the burn command, bit 2 =
the guard (checks the eFuse before any NVS init; every image from this one on). `fw nvs-slots` (every build, read
only, `esp_partition_read` of the app description head and the mark, no mapping) prints both slots:

```
nvs-slots: ota_0 (running) fw-1234abc: encrypt, guard
nvs-slots: ota_1 fw-5678def: no mark (built before it: unguarded)
nvs-slots: a slot without the guard: do not burn (a rollback would erase an encrypted NVS)
```

Per slot: `no app`, `no mark (built before it: unguarded)`, or `plain|encrypt[ + burn command], guard|NO guard`.
Verdict (`nvs_slots_verdict`): `both slots guarded encrypt images: a rollback keeps an encrypted NVS readable` (the
only one that allows the burn); `both guarded, NOT both encrypt: do not burn yet (a rollback would leave NVS down)`;
`a slot without the guard: do not burn (...)`. An empty slot counts as unsafe: the next OTA lands there. A burn image
adds `a burn image is on the badge: replace it once the key is burned`.

Limits: the mark is read from the start of the slot, so **a half-written image (an OTA that broke off) reads like a
complete one**. The mark says what the image was built with, not that it boots. Hence the checklist step below.

The only code that writes an eFuse is `burn()` in `lichen_nvs.c`, compiled only with `LICHEN_NVS_ENCRYPT_BURN_CMD`
(default n, depends on `LICHEN_NVS_ENCRYPT`). It runs only from `fw nvs-enc burn YES` (the exact text `YES`), refuses a
block that is not unused, and takes its random bytes from `lichen_random_fill` (the entropy source the OS already
switched on at boot). Note that `fw` commands also arrive over Wi-Fi (`POST /console`, token) when `OTA_WIFI_CONSOLE` is
on: build the burn image, run the command from the USB console, and do not leave that image on the badge.

Verified on the default build: `nm badge_fw.elf` has no `esp_efuse_write_key`, no `esp_efuse_batch_write*`, no
`nvs_flash_secure_init`, no `nvs_flash_generate_keys`; `nvs_sec_provider` is only IDF's empty linker hook.

## ESP32-P4 rev v1.0 support (read from IDF 6.1 sources only, not tried on silicon)

- `SOC_HMAC_SUPPORTED 1`: `components/soc/esp32p4/include/soc/soc_caps.h:75`.
- Six key blocks with purpose fields: `components/efuse/esp32p4/esp_efuse_table.csv:194-199` (`KEY_PURPOSE_0..5`),
  `HMAC_UP = 8`: `components/efuse/esp32p4/include/esp_efuse_chip.h:79`.
- `esp_efuse_write_key` read-protects HMAC keys: `components/efuse/include/esp_efuse.h:738`.
- The P4 HMAC path (`components/esp_security/src/esp_hmac.c:57-110`) is the hardware one with the Key Manager's
  "use eFuse key" selector; the only chip-revision checks in `esp_security` are for other chips (`init.c:83`, H2).
  I found no P4 v1.0 exclusion in IDF, but that is not proof the silicon works: **`fw nvs-enc` on a build without the
  burn command, then the first derivation after the burn, is the test** (the migration log line and a read-back of every
  entry are the check; see "Verify").

## Migration (plaintext to encrypted, once)

On a boot with the option on, a key in KEY5 and a plaintext partition:

1. plain `nvs_flash_init`, every entry of every namespace read into RAM (`nvs_snap`), strings and blobs through a
   temporary that is zeroed; **if any entry cannot be read, nothing is erased** and the boot continues plain;
2. `nvs_flash_deinit`, erase the partition, `nvs_flash_secure_init`;
3. each entry written back and read back and compared; a count is logged ("N of N entries written back encrypted");
4. the RAM copy is zeroed, then freed. Values are never logged, only counts and entry names on failure.

If the encrypted init fails after the erase, the entries go back unencrypted and the next boot tries again.

**The window**: between step 2's erase and the end of step 3 the RAM copy is the only copy. A power cut or reset in
that window (well under a second for ~100 entries) loses the settings. That is why the procedure below starts with a
backup, and why the migration should be done on USB power with Lucas at the badge. Restoring the NVS backup (plaintext)
simply migrates again. Not done: a scratch copy in flash. There is no spare partition (`partitions.csv`: `nvs`,
`otadata`, `phy_init`, `ota_0/1`, `coredump`, `assets`, `storage`); the 128 KB `coredump` partition could serve as one
(plaintext secrets in flash until wiped) at the price of losing a crash dump. Decision for Lucas if the window worries him.

**The apps' secrets class (#288) and this migration.** The class (`app_secret_*`, MeshCore's repeater passwords stored
as `~pw<hex>`) keeps a secret from every software path: plain settings calls, backups, the reset's keep list, console
output. It does not protect the flash. Until the partition is encrypted, both the stored values and the bytes of the
old plain `pw<hex>` entries the password move erased (NVS marks an entry erased, it does not overwrite it) can be read
from a flash dump. This migration erases the whole partition before it writes back, which removes those leftovers. The
password move and this migration never share a boot (`lichen_nvs_migrated()` defers the move), and the image with the
password move must never be the same flash as the encryption flash: flash it, let it boot and move, then flash for
encryption separately.

If the partition table were ever changed for this (not needed for the HMAC scheme): the `nvs_keys` partition is only for
the flash-encryption scheme (`CONFIG_NVS_SEC_KEY_PROTECT_USING_FLASH_ENC`, 4 KB, subtype `nvs_keys`, e.g. at `0x12000`
in the free gap before `ota_0`). The partition table is written only by USB flashing (`0x8000`), never by Wi-Fi OTA,
so such a change reaches a badge only via `flash-fw`, and an old image on the other OTA slot would then see a table it
does not know.

## Factory reset (#66)

`factory_reset.c` erases NVS and re-inits through `lichen_nvs_init()`, so after a full reset an encrypted badge comes
back with a fresh **encrypted** NVS (partition empty, key present). Settings reset goes key by key through the open
(encrypted) handle. Neither touches an eFuse; there is no `nvs_keys` partition for a reset to wipe, and the reset code
never opens any partition but `nvs`, `storage` (see its header). The key itself survives a reset by design.

## What it means for flashing and updates

- **`badge.sh flash-fw ...` (USB, keeps `/storage` by default)**: unchanged. `build/flash_args` does not contain `0x9000`, so NVS is
  not rewritten; the eFuse key and the encrypted NVS stay. It also writes the partition table (unchanged here).
- **A MacBook recipe**: `esptool --chip esp32p4 write-flash @flash_args` as before, without `0x9000` and without
  `erase-flash`. `erase-flash` wipes NVS (settings, not the key); the next boot of an encrypted-capable image brings up
  a fresh encrypted NVS and the setup screens run again.
- **Wi-Fi OTA**: only the app slots change; NVS and the OTA token are untouched and keep working.
- **Rollback to an image without this feature is destructive.** An older image calls plain `nvs_flash_init()` on an
  encrypted partition. IDF's page loader (`nvs_page.cpp`, `mLoadEntryTable`) checks each written entry's CRC
  (`Item::checkHeaderConsistency`), which an encrypted entry fails, and erases that entry and its span: every setting
  and the OTA token copy are gone after that one init, whatever `main.c` does next. Read from IDF 6.1 source, not tried.
  An image with the guard (bit 2, every build from the guard on) built with the option off does not do this: it sees
  the key and leaves NVS down and intact (no settings that boot, nothing lost). An image without the guard always
  does. So before the burn both OTA slots must hold a guarded encrypt image (`fw nvs-slots`), and every later build
  must keep `LICHEN_NVS_ENCRYPT=y` (with `BURN_CMD` off). **Never flash a pre-guard image onto a burned badge**, not
  over USB either.
- **A badge handed on**: the key is bound to the chip; another chip's flash image cannot decrypt this NVS.

## By-hand enable procedure

Everything here is done by hand, one step at a time, with the badge on USB power. Flash and console commands are the
usual ones from the flash host (`tools/badge.sh`).

1. **Back up first (no exceptions).** With the badge reachable over USB:
   `esptool --chip esp32p4 -p <port> read-flash 0x9000 0x6000 nvs-plain.bin` and
   `esptool ... read-flash 0xB40000 0x4C0000 storage.bin`. Keep both off-repo: `nvs-plain.bin` contains the passwords.
2. Build the **burn image**: `CONFIG_LICHEN_NVS_ENCRYPT=y` and `CONFIG_LICHEN_NVS_ENCRYPT_BURN_CMD=y` in an sdkconfig
   override (not in `sdkconfig.defaults`), `firmware/build.sh build`. Flash with `badge.sh flash-fw <port>` (storage is kept by default).
3. Boot. `fw nvs-slots` must say `both slots guarded encrypt images` before step 4 (the running slot `encrypt + burn
   command, guard`, the other `encrypt, guard`): otherwise a rollback right after the burn can reach an image that
   erases or cannot read NVS. The other slot must hold a **complete** image: it came from an OTA that ended with
   the badge rebooting into it (or was booted once with `fw slot other` and switched back), since a broken-off
   upload carries the same mark. `fw nvs-enc` must say `key KEY5 not burned, nvs partition plaintext, this boot plain`. The log has the W line
   "no HMAC key in KEY5: NVS stays plaintext".
4. **IRREVERSIBLE.** `fw nvs-enc burn YES` (from the USB console). Expected reply `burn KEY5: ESP_OK`. Any other
   text only prints the warning. If the block is not unused it says so and burns nothing.
5. Reboot. Expected log: `migrating N plaintext entries into encrypted NVS`, `NVS is encrypted (HMAC key in KEY5)`,
   `N of N entries written back encrypted`. If it says `incomplete`, restore the backup (step 1, `write-flash 0x9000
   nvs-plain.bin`): the next boot migrates again.
6. Build the **normal image** with `LICHEN_NVS_ENCRYPT=y` and the burn command off, flash it over USB and OTA it so
   both slots hold it (see rollback above). `fw nvs-slots` must say `both slots guarded encrypt images` and name no
   burn image.
   Make that the new `sdkconfig.defaults` value only then.
7. `fw nvs-enc` now says `burned (HMAC_UP), nvs partition encrypted, this boot encrypted`.

## Verify

- `esptool read-flash 0x9000 0x6000 nvs-after.bin`, then `strings nvs-after.bin | grep -i <the Wi-Fi password>` finds
  nothing, and `nvs-plain.bin` from the backup does find it.
- Settings, Wi-Fi join, MeshCore contacts and the repeater passwords survive a reboot and an OTA.
- `fw nvs-enc` as above; `espefuse summary` (read-only) shows `KEY5` purpose `HMAC_UP` with the key reading as
  protected.
- Factory reset (settings and everything) still works and the badge comes back encrypted: `fw nvs-enc`.

## Decisions left

- Whether to burn at all, and when (Lucas, at the badge).
- Whether the migration window needs a scratch copy in the `coredump` partition.
- Key block (KEY5 chosen) and whether to keep the burn command compiled in any image after the burn (no).
- Flash encryption for `/storage` (the MeshCore identity): separate decision, see #116's first option.
