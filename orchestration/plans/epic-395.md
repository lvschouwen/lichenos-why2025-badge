# Plan: epic #395 Security (planner, 2026-11-13, read-only)

## 1. Issue inventory

| # | Title (short) | State | Labels of note | Real status (from code) |
|---|---|---|---|---|
| 391 | Injected keys refused on hand-only paths | OPEN | `now`, score 7 | **Code on main awaiting bench.** `board/key_origin.c/.h` (per-key origin, `phys` = injected), `lichen_sys_keys_last_injected()`, `factory_combo.c:48` passes `ev->injected`, About taps refuse injected Enter (`app_about.c:205`, #374 merged 0ef058d0), rule in `docs/architecture.md:358`. Host tests `key_origin_test.c`, `factory_plan_test.c`, `about_dev_test.c`. Remote and hand bench not recorded on the issue. |
| 211 | Refactor OTA parsers (parse_header CCN 29/nest 11, handle_piece CCN 31) | OPEN | refactor | **Not started.** Baseline lines `tools/standard-baseline.txt:28,29,34`. `ota_proto_test.c` has 142 checks but no mutation/malformed corpus; no fuzz harness exists in `host_tests/`. |
| 288 | App secrets class (MeshCore admin passwords) | OPEN | — | **Not started.** `settings_key_secret()` returns false for any namespace but `badge_fw` (`settings_secret_keys.c`); MeshCore still uses `app_settings_get/set_str` for `pw<12hex>` (`meshcore_run.c:53-66`). No `app_secret_*` symbol anywhere. |
| 289 | Badge-side on-air allow list for console sends | OPEN | area:meshcore | **Not started.** What exists: host-side guard (`tools/badge.sh`, guard hook, #287), the region console scope (`mesh_cscope.c`, #290) and the remote-key scope for UI answers (`bench_scope.c`, #377). Nothing checks `fw mesh send/dm/remote/telemetry/trace/discover` targets on the badge (`mesh_task.cpp:2630` dispatch table). |
| 116 | Encrypt NVS (HMAC key in KEY5) | OPEN | score 4 | **Code on main, off; bench and burn pending.** Wave 1A merged (65028ed3, 7bef8779, c8ddcb49): guard in every build, burned-key path never erases, OTA refused without token, `fw nvs-slots`. `LICHEN_NVS_ENCRYPT` not set in `sdkconfig.defaults:214`. Burn blocked until both slots hold guarded encrypt images. |

Open: 5. None carries `needs-lucas`/`needs-proofing`, though #391 and #116 both need bench proof (suggest `needs-proofing` on #391).

## 2. Dependencies inside the epic

- **#288 before #116's burn/migration.** Both move NVS data; #116's decision is "one migration at a time". Land and bench #288's `pw*` move first, then the HMAC migration covers the final key layout.
- **#391 bench before #289 bench.** #289's acceptance uses console lines; #391 proves injected keys cannot reach DANGER paths, so the allow list is the remaining on-air hole.
- #211 and #289/#288 are independent (disjoint files).

## 3. Cross-epic dependencies

**This epic needs first:**
- #216: #229 (CAP_SECRETS) fixes the vocabulary #288 declares against. Not a hard block: #288 can declare secret keys in `lichen_app_t` now and #229 maps CAP_SECRETS onto it. Tell plan-216 the field name.
- #216: #235 (one NVS owner) and the MVP image flash must not share a flash with #288's migration or the #116 burn.
- #216: #208 (split `mesh_task.cpp`) collides with #289's dispatch hook. Order: #289 first (small hook + new pure file), #208 after, or #208 first and #289 rebases. Decide with plan-216; do not run both in one wave.

**Others need from this epic:**
- #381: #374 and #379 were "blocked by #391"; #391's code is merged, only its bench is open. **Inconsistency:** #379's acceptance ("`fw boot keys esc+bksp` -> the #66 page shows") cannot pass: `boot_keys_play.c` injects (`keyboard_inject(..., false)`) and #391 refuses injected arming. That is the correct security outcome; #379's acceptance should read "-> no #66 page, refusal logged" unless Lucas wants a boot-only exception (not recommended: it reopens a token-holder path to a DANGER confirm).
- #216 #212: protocol decoders `mf_decode`, `lora_decode` (`shared/lora`, both chips), `rr_request_prepared`, `mesh_telem` decoders take radio input. They should reuse the malformed-input helper from E395-W1.
- #216 #233 (Clear data) and #229: must erase/declare app secrets from #288.
- #393 #169 (backup/restore), #387 (name tag): rely on the secrets class and the backup whitelist.
- #381 #378 (`fw sd cat` deny list) and #380 (gate debug commands): apply the checklist in section 6.

**Move earlier than the rest of the program:** #391 bench (W0, now), #211 (W1: the shared malformed-input helper and the OTA HTTP parser are the most exposed input, every other epic's OTA/console work sits on it), #288 (before #233, #169, #117 add more app-held secrets).

## 4. Decisions only Lucas can make, and missing prerequisites

1. **#289 "on" switch:** setting (`fw onair on|off`, default off, kept across flashes; recommended) vs build flag.
2. **#289 who may switch it off or edit the list:** recommend USB console or hand only, like `fw dev on`; otherwise a Wi-Fi token holder disables the guard with one line.
3. **#289 scope of origins:** recommend the list applies to every console origin (UART, WIFI, and LOCAL = the on-screen Console app), because injected keys can type into the Console app. UI sends stay untouched.
4. **#288 declaration form:** key prefix in `lichen_app_t` (recommended: MeshCore's keys are `pw<hex>`) vs an explicit list.
5. **#288 migration vs "v2 migrates no stored data" (2026-11-07):** #288 (2026-11-10, later) asks for a one-time `pw*` migration. Latest wins, but confirm.
6. **#379 acceptance** per section 3 (boot-injected Esc+Bksp is refused).
7. **#116 (irreversible):** burn date, at the badge, USB power, after both slots read SAFE. Already decided: KEY5 HMAC only, no flash encryption, back up NVS and /storage first. No agent ever burns, flashes or enables `CONFIG_NVS_ENCRYPTION` (IDF's own init can burn a key). Open from the doc: keep the burn command out of every post-burn image (recommend yes).
8. **Flash encryption / secure boot:** out of scope (left out, see 7); /storage (MeshCore identity) stays readable by USB.

Missing prerequisites: #391 bench evidence; current `fw nvs-slots` / `fw nvs-enc` readout on the badge.

## 5. Waves

### E395-W0 (main only, no code; can start now)
- Remote bench #391: `fw keys press esc hold:3500 bksp hold:3500` -> no factory question, log shows refusal; an injected key still navigates elsewhere; 7 injected Enters on About do not enable developer mode. Needs-lucas: a physical Esc+Bksp hold still opens #66; 7 physical taps enable dev mode. Close #391 on evidence, or label `needs-lucas` with that checklist.
- Read `fw nvs-slots`, `fw nvs-enc` (read only) and record on #116.
- Collect decisions 1-6. Post a #379 comment on the acceptance mismatch.

### E395-W1 (three parallel agents)
Prereqs: none for 1a; W0 decisions 1-3 for 1b, 4-5 for 1c.

**1a #211 OTA parsers (Opus).** Split `parse_header` into pure steps: trim -> name lookup in a table `{name, handler}` -> per-field validator; split `handle_piece` into `piece_check(req) -> status` (pure, in `ota_pieces.c` or `ota_proto.c`) plus I/O. In the same refactor, add: refuse a repeated header among Content-Length, X-Offset, X-Total-Size, Authorization, X-Path, the SHA headers (today the last one silently wins: a request-ambiguity class); `vlen` bound before every `strcpy` stays explicit (prefer `memcpy` with length). Add `host_tests/mutate.h`: seeded, deterministic byte mutation (flip, truncate, insert CR/LF/NUL/colon, repeat) over a corpus of valid heads, N iterations under ASan+UBSan, asserting no crash and `status in {0,400,404,405,413,415}`. Acceptance: CCN <=15 and nesting within the standard for both functions, baseline lines removed (shrink only), every existing `ota_proto_test` check unchanged, new tests: duplicate header -> 400, header line exactly LINE_MAX_LEN and +1, Content-Length 12 and 13 digits, token 63/64/65 hex, X-Path empty/max/max+1, mutation run. Bench (remote): OTA of a P4 image and a file push via `badge.sh`, `GET /info`, `/log?since=`, `/console`. Files: `ota/ota_proto.c/.h`, `ota/ota_server.c`, `ota/ota_pieces.c/.h`, `host_tests/ota_proto_test.c`, `host_tests/ota_pieces_test.c`, new `host_tests/mutate.h`. Conflict files: `tools/standard-baseline.txt`, `host_tests/CMakeLists.txt` (only if a new test target). Reviewer: status codes unchanged for valid clients, no read past `len` in `ota_parse_request`, `piece_check` ordering kept (the #125 "before update screen" check), timing-safe token compare untouched.

**1b #289 on-air allow list (Opus: changes what the badge transmits).** Pure `mesh/onair_allow.c/.h` (ESP-free): `onair_verdict(enabled, list, kind, target) -> ALLOW|REFUSE`; channel names exact (case rules as MeshCore), contacts by key-prefix digits; empty list with enabled = refuse all (fail closed). Hook once in `cmd_mesh` dispatch for send/dm/remote/telemetry/trace/discover (+ `advert flood`? ask; not in the issue, so leave out). Console `fw onair on|off|allow|deny|list`; on/off and edits USB-only per decision 2. List stored via the settings service in `badge_fw` (never in the repo, never in the backup whitelist, cleared by factory reset). Refusal text exactly as the issue. Host test `onair_allow_test.c` mirroring `tools/tests/guard_onair_test.sh` cases plus malformed targets (empty, overlong, non-hex, `#` alone). Bench (remote): with on and the list holding the test channel, `fw mesh send #badgetest` transmits, another channel is refused with nothing on air (CoreScope observation via the Heltec peer). Files: `mesh/onair_allow.*`, `mesh/mesh_task.cpp` (hook only), `mesh/CMakeLists.txt`, new host test. Conflict files: `host_tests/CMakeLists.txt`, `firmware/components/mesh/CMakeLists.txt`, `tools/standard-baseline.txt`. Reviewer: every transmitting console path covered (grep the sub table and `cmd_mesh_remote` subcommands), UI path untouched, origin rule, no target logged with secrets.

**1c #288 app secrets (Opus: key storage).** Gate API `app_secret_get/set/erase(ctx, key, ...)` in `lichen_api/lichen_settings.h`, implementation in `lichen_os/lichen_settings.c` + `settings_secret_keys.c` (`settings_key_secret` learns app namespaces with declared prefixes); declaration `secret_prefixes` in `lichen_app_t`. `app_settings_get_str/set_str` refuse a secret key; secrets never in the backup table, never on the factory keep list (`factory_plan.c`), never printed (`fw app settings`/registry `fw get` show masked or nothing); `app_settings_erase_all` (Clear data) removes them. MeshCore: `meshcore_run.c` switches, one-time move of existing `pw*` (read plain, write secret, read back, then erase plain; idempotent; values never logged). Host tests: class refusal per API, backup/keep exclusion, migration (fresh, partial-interrupted, already migrated). Bench (remote): a saved repeater login still works after the flash; `fw get`/app settings listing shows no password. Files: `lichen_api/lichen_settings.h`, `lichen_api/lichen_app.h`, `lichen_api/app_ns.c`, `lichen_os/lichen_settings.c`, `lichen_os/settings_secret*.c/h`, `lichen_boot/factory_plan.c`, `mesh/meshcore_run.c`, `mesh/meshcore_app_record.c`, `docs/app-api.md`, host tests. Conflict files: `tools/gate_manifest.txt`, `tools/layers.conf`, `host_tests/CMakeLists.txt`, `tools/standard-baseline.txt`. Reviewer: the store is the only enforcement point (no bypass via `get_blob`, registry, backup), migration crash-safety, buffers wiped after use.

Merge order: 1a, 1c, 1b (1b's mesh `CMakeLists.txt` and 1c's mesh files are disjoint; union-merge `host_tests/CMakeLists.txt`). #288's migration flash is its own OTA, not combined with #216's MVP or #235.

### E395-W2 #116 to the burn (main + Lucas; one small agent task)
Prereqs: W1c merged and benched; #235 and the MVP flash not pending in the same window; W0 readout.
- Main: build the burn image (sdkconfig override, not defaults) and an encrypt image; OTA so `fw nvs-slots` reads SAFE (remote).
- Lucas at the badge: backup NVS and /storage over USB, `fw nvs-enc burn YES` from USB, reboot, check the migration count, `read-flash` of NVS shows no Wi-Fi password (needs hands/USB).
- Agent (Sonnet, after the burn only): `CONFIG_LICHEN_NVS_ENCRYPT=y` in `firmware/sdkconfig.defaults` and the release defaults, burn command off; `docs/nvs-encryption.md` status. Conflict file: `sdkconfig.defaults`. Same session as the burn, so no unguarded or plain build reaches the badge afterwards.
- Bench: settings, Wi-Fi, repeater logins survive reboot and OTA; factory reset comes back encrypted (destructive: Lucas decides).

## 6. Cross-cutting security checklist (other epics, from their first wave)

1. **Parsers:** every parser of bytes from Wi-Fi, LoRa, the C6 link, SD, BLE or the console is a pure ESP-free function with a host test that feeds truncated, overlong, NUL-embedded and mutated input (`host_tests/mutate.h` once W1a lands) under ASan+UBSan; every copy is bounded by an explicit length.
2. **Repeated or conflicting fields** in a parsed message are refused, not last-wins.
3. **Console commands:** a debug/test command uses `console_register_dev` or `console_dev_gate`, or is compiled out by Kconfig; `build.sh release` contains none of its strings.
4. **Origin:** a command that enables developer mode, weakens a guard, burns, erases or wipes storage checks `console_origin() == UART`; LOCAL (Console app) counts as remote because injected keys can type there.
5. **Hand-only:** a DANGER confirm or a raw-key gesture checks `ev->injected` / `lichen_sys_keys_last_injected()`; a new dialog gets a row in `dialog_remote_policy.c` (unknown = REFUSE).
6. **Transmit:** a new path that makes the badge transmit (LoRa, BLE, 802.15.4, Wi-Fi beyond replies) is reachable from the console or injected keys only within the bench scope (#290/#377) or the #289 allow list; reviewed by Opus.
7. **Secrets:** passwords, tokens, keys go through `settings_secret_*` or `app_secret_*` (after #288), never plain settings, never in the backup whitelist or keep list, printed only with `console_reply_secret`, never logged (`ESP_LOG*`, log IDs of #328, crash log), buffers zeroed after use.
8. **Private content** (messages, files, screenshots) goes out only by `console_reply_private`, never into the log store.
9. **OTA/flash:** no change enables IDF `CONFIG_NVS_ENCRYPTION`, flash encryption, secure boot or any eFuse write; every image keeps the NVS guard bit (`nvs_mark`); `nvs_boot_policy` changes ship with a test pair (#232 shares it).
10. **NVS layout:** one data migration per flash; a migration reads back before it erases.
11. **P4 <-> C6:** a new frame type or field is length-checked on both sides and documented in the #234 contract.
12. **Registry (#307) and `fw get`:** a secret-class key is never readable or settable through the registry.
13. **Public repo:** no IPs, MACs, host names, SSIDs, tokens or allow-list contents in code, tests, issues or commits.

## 7. Risks and leave-outs

- **Risk:** after the burn, any build from a branch still at `LICHEN_NVS_ENCRYPT=n` leaves NVS down (guarded, not erased); the badge boots without settings. Mitigation: flip the default in the burn session; tell every epic's main to rebase before flashing.
- **Risk:** #211 changes status codes that `badge.sh` relies on; keep valid-client behaviour byte-identical.
- **Risk:** #289 hook missed on a transmit path (remote CLI subcommands); reviewer greps all subs.
- **Risk:** #208 and #289 both edit `mesh_task.cpp`: sequence them.
- **Leave out:** full flash encryption and secure boot (irreversible, complicates USB flashing and OTA; Lucas chose HMAC-only); fuzzing with libFuzzer/AFL (toolchain dependency; the seeded mutation test inside ctest covers the gate); the MeshCore submodule's own decoders (never written in; covered at our boundary by #212).
