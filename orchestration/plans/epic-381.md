# Epic #381 plan: remote-bench tooling and developer mode (2026-11-13)

Base: local main 0746fd86 (#374 fast-forwarded and size lines committed, not pushed). The badge runs fw-664dea27, which is wave 1 only (#373, #375, #376).

## 1. Issue inventory

| # | Title (short) | State | Labels of note | Real status (from code) |
|---|---|---|---|---|
| 373 | Dev mode core (`LICHEN_DEV_COMMANDS`, `fw dev`) | closed | - | done, on the badge |
| 375 | Key injection, real path, hold | closed | - | done, on the badge |
| 376 | Dialog remote policy table | open | needs-proofing | on the badge (wave 1), its remote proof is still owed |
| 377 | `fw ui answer` (closes #182) | open | now | code on main (13968a18..d276cfed), awaiting bench |
| 182 | keys cannot answer dialogs | open | needs-lucas | Lucas has decided. Closes when #377 is proven. The needs-lucas label is now stale |
| 378 | `fw sd cat` with a deny list | open | now | code on main (`sd_console.c`, `sd_path_policy_test`), awaiting bench |
| 379 | `fw boot keys` one-shot | open | now | code on main (`boot_keys*.c`, `main/boot_keys_play.c`), awaiting bench. **Its #66 bench step can no longer pass (see 4.2)** |
| 391 | Injected keys reach hand-only paths | open | bug, now | code on main (79eebb9a, c53ba56d, `key_origin_test`, `factory_plan_test`), awaiting bench |
| 396 | Release build fails on boot usage text | closed | - | done |
| 374 | Dev mode UI (7 taps, DEV chip) | open | now | on local main, not pushed, `agent-check` not yet run, release build not run |
| 380 | Gate the existing debug commands | open | - | not started. Only `ui`, `keys`, `boot keys`, `sd test` and `sd cat` are gated today; about 30 files register the rest ungated |
| 382 | Host UI build (SDL2), screenshot tests | open | ease:1 | not started. No `ui_sim`, SDL2 not installed |
| 383 | Health page and `fw health` | open | - | not started. No `health_model` |
| 385 | App SDK template, `badge.sh new-app` | open | documentation | not started. Lucas: build it after #229 and #231 |
| (wave 3) | compile-out, `dev_only` app flag, HTTP route gating, `check_dialog_policy.py`, bench docs | no issue yet | - | not started, waits on the `?` decisions |
| (to file) | ✕ shape key restarts a stopped MeshCore (ungated start) | no issue | - | open follow-up from the epic comment |
| (to file) | Release build not in the check (#396 slipped through) | no issue | - | `CHECK=` builds the dev image only |

That is 10 open issues (376, 377, 182, 378, 379, 391, 374, 380, 382, 383, 385; #182 closes alongside #377), plus 3 items still to file.

## 2. Dependencies inside the epic

- **#374 before everything else.** It is the unpushed head of local main. Every later merge rebases on it.
- **#377 proven, then #182 closed.** #182 is only the decision record.
- **#380 after the W0 bench pass.** #380 changes which commands the bench may run, and the W0 pass uses today's set.
- **#380 before wave 3.** Wave 3's release compile-out of whole files and the release-build gate assume every DM command is registered through `console_register_dev` or a `CON_GATE_DEV` row of its sub-command table (#464; `console_dev_gate` before).
- **Bench docs update in the same wave as #380.** Otherwise the two-chip gate breaks without a sign: `fw tasks`, `psram`, `c6log` and `wifi tx` become DM, and `ui_overflow_sweep.py` needs `fw dev on` first.
- **Follow-up "release build in the check" before wave 3.** The compile-out work needs a release build that is gated on every commit.
- **#383, #382 and #385 are independent of #380**, apart from the conflict files. #385 is the last item.

## 3. Cross-epic dependencies

**This epic needs from the others:**
- **#216 platform:**
  - #229 capabilities: `lichen_app_t` flags. The `dev_only` app flag must not collide with `CAP_*`, and #385's template needs #229.
  - #231 lifecycle: #385 needs it.
  - #234 P4 <-> C6 contract: #383's C6-link probe.
  - #307 registry read-backs: #383 rows.
- **#394 hardware:** #11 battery bodge. Without it #383's battery row reads "n/a" and nothing can be measured for power.
- **#395 security:** shares the hand-only rule (#391) and the HTTP route policy. Agree who owns `/screenshot`, `/log` and `/crashes` in release. I propose this epic implements it and #395 reviews.
- **#392 MeshCore:**
  - #380 edits `mesh_task.cpp`, which carries 6 registrations. Sequence it against #392's waves and against #262's #271 (`ob_mux`).
  - The ✕-key follow-up sits in their start path.
- **#262 memory:** #380 touches `psram_guard.c`, `lichen_mem.c` and `lichen_task.c`. #384 (boot-time budget, area:tooling) stays with #262.
- **#393 UI:**
  - #374 touches About and the status bar.
  - #383 adds a Settings page.
  - #382 serves their proofing.

**The others need from this epic (the key unlocks):**
- **W0 proofs:**
  - #377 makes the non-DANGER confirm steps remote in #66, #162, #167, #169, #189, #264, #273, #274 and #286.
  - #378 makes file read-back remote for #169 (backup) and #155 (weather SD log).
  - #379 `down` makes #128's boot screen path remote.
- **Dev mode on a dev build is now required for every remote UI check:**
  - `ui` and `keys` are already DM.
  - After #380, `tasks`, `psram`, `mem blocks`, `c6log`, `wifi tx/bw/proto/ps`, `lora log/raw` and `sensors raw` are DM too.
  - The orchestrator runs `fw dev on` once over USB. It persists in `dev.mode`, and a factory reset clears it.
  - A release image has no remote-bench tooling at all.
- **#383 `fw health`:** one-line hardware pass/fail for #394's waves.
- **#382 sim:** layout proofing without the badge, for #393.

**Remote-check capability map (what unlocks what):**

| Kind of check | Tooling | State |
|---|---|---|
| Screen capture | HTTP `/screenshot` (`badge.sh screenshot`) composites the layers; `fw ui card` | exists |
| Screen capture without the badge | #382 sim goldens | W5 |
| Key injection, focus-guarded | `fw keys send/press`, `hold:`, `phys`, the remote key lock (#190, #375) | exists |
| Dialog answers | `fw ui answer yes <label> #n`, non-DANGER only, by policy (#376, #377) | on main, bench owed |
| State read-back | `fw ui` dump/overflow, `fw get` (#307), `fw mem/boot/c6/mesh/tasks/airplane`, `badge.sh log/crashes` | exists |
| State read-back | `fw sd cat` (#378) | on main, bench owed |
| State read-back | `fw health` (#383) | W4 |
| Boot paths | `fw boot keys down` (#379), `fw reboot`, `fw c6 reset` | on main |
| Radio observation | `fw mesh` rx/tx counters; the Heltec peer session via RemoteTerm/CoreScope, `#badgetest` only, console scope (#290) | exists |
| Power measurement | none: no meter, the USB power switch is parked, #11 is not fitted | **hands-only** |

**Hands-only after this epic:**
- Physical TCA8418 key presses (two-chip gate step 4), by design since #391.
- The seven About taps.
- The #66 combo and confirm.
- DANGER dialogs: factory reset, SD format, power off, forget Wi-Fi, remove contact, repeater admin, radio and password; also "switch off updates over Wi-Fi" and "remove channel".
- Panel truth (flicker, colour, backlight), LED, vibration and audio. These stay with Lucas while the webcam is parked.
- Cold power cycle, the power button, the IP5306 32 s cut, USB replug, SD insertion and battery or charge checks.
- Any change to radio settings that alters what the badge transmits, unless Lucas asks.

## 4. Decisions for Lucas, and missing prerequisites

1. **The `?` rows** (each one is DM, CO or keep):
   - `lock unlock`, `crash dump`, `c6 reflash/verify/reset`, `set`, `qr`, `slot other`
   - the LoRa spectrum app and the DMA alloc hook
   - HTTP `/screenshot`, `/log` and `/crashes` in release
   - the release default for the update server
   - the dialog rows "badge radio settings" and "backup load"

   These block wave 3 and the `?` part of #380.
2. **#379 against #391.** Boot keys play through `keyboard_inject`, so since #391 an `esc+bksp` boot shot can no longer arm the #66 question, and #379's bench step "the #66 page shows" will fail. Options:
   - (Recommended) Keep the refusal and turn `esc+bksp` into a negative proof ("an injected combo is refused"). Retarget #379's bench step.
   - Drop `esc+bksp` from the parser.
   - Give boot keys an origin that may *arm* but never confirm. This weakens the hand-only rule and is not advised.
3. **Two dialogs #376 made physical-only on its own call:** "switch off updates over Wi-Fi" and "remove channel". Confirm or open them up.
4. **Release build in the check.** It adds gate time per commit. Measure it before deciding (pre-push only, or every `agent-check`).
5. **#382 needs SDL2 on the dev host**, which is a package install. Also decide when the PNG goldens enter the gate.
6. **Parked hardware:** the webcam and the USB power switch. While both are parked, panel, power-cycle and power checks stay needs-lucas. Unparking needs Lucas's OK, since it is a purchase or mod.
7. **Missing prerequisites:**
   - #11 bodge, for the battery and power rows
   - SDL2
   - the `?` answers
   - #229 and #231, for #385

## 5. Waves

### E381-W0: land #374, bench wave 2, file follow-ups (main only, no agents)
- **Issues:** #374, #376, #377, #378, #379, #391, #182.
- **Prerequisites:** none. Can start now.
- **Tasks:**
  1. `agent-check` on local main, then commit the size lines.
  2. `./firmware/build.sh release build`, push, `agent-wt done wt-1`.
  3. `reconfigure build`, then OTA the P4 (the C6 is unchanged).
  4. Wait 75 s, run the two-chip gate, then `fw dev on` over USB.
  5. Run each issue's remote checks:
     - #376/#377: Discover nearby → `fw ui answer yes <label> #n` → "Listening, N s left"; Reset order allowed; Reset everything refused; a remote Enter on a REFUSE dialog dropped with "dialog '<label>' is hand-only".
     - #378: an app file reads back; `/storage/ota_token.txt` refused; `GET /log` lacks the content.
     - #379: `down` takes the #128 path; `esc+bksp` per decision 4.2.
     - #391: `fw keys press esc hold:3500 bksp hold:3500` → no factory question.
     - #374: after `fw dev on`, `fw ui` on About shows the DEV chip and the row.
  6. File the two follow-up issues and the #379 retarget.
  7. Close #182 with #377.
- **Hands (Lucas):**
  - #374: seven physical taps.
  - #391: a physical Esc+Backspace still opens #66.
- **Security checks:**
  - Over Wi-Fi: `fw dev on` is refused.
  - The release image passes `release_checks`.

### E381-W1: follow-ups (gate hardening)
- **Prerequisites:** W0. Decision 4.2 for task C; decision 4.4 for A.
- **A (Sonnet).** Run the release build inside the check or the pre-push gate.
  - Files: `firmware/build.sh`, `.claude/agent-wt.conf`, `tools/gate_manifest.txt` (conflict).
  - Acceptance: re-adding the #396 text fails the gate.
  - Measure the time first.
- **B (Opus: input policy, starting MeshCore transmits).** An injected or remote ✕ never starts a stopped MeshCore.
  - Files: the shell shape-key path (`shell.c`, plus the `ui_remote` policy file) and `host_tests/` (conflict: `host_tests/CMakeLists.txt`).
  - Host test: the policy refuses an injected ✕ start. It fails before the change and passes after.
  - Bench: remote `fw keys send cross` with MeshCore stopped → it stays stopped.
- **C (Sonnet).** The #379 retarget.
  - Files: `boot_keys.c/.h`, `boot_keys_test.c`.
  - Host test: the parse table.
- **Parallel:** A, B and C touch disjoint files. B and C both touch `host_tests/CMakeLists.txt` only if C adds a test, so union-merge B first.
- **Reviewer focus:** for B, that origin travels per key (as in c53ba56d) and that Esc is never blocked.

### E381-W2: #380 gate the existing debug commands, plus the bench docs
- **Prerequisites:** W0 bench pass. Merge after any #392, #262 or #271 branch that is in flight on `mesh_task.cpp`.
- **Split by file, with no shared files (all Sonnet):**
  - **a:** shell and OS
    - `screen_idle.c`, `screen_saver.c`, `ui_check.c`, `app_bench.c`, `app_stress.c`, `setup_screens.c`, `home.c` (whatsnew)
    - `thermal.c`, `disp_state.c`, `lichen_task.c`, `lichen_mem.c`, `fonts.c`
    - `crash.c`, `boot_progress.c`, `main.c` (`noscreen`, `nvs-down`)
  - **b:** drivers and storage
    - `psram_guard.c`, `lichen_nvs.c` (nvs-slots), `storage_console.c`, `sensors_cmd.c`, `board_i2c.c`
    - `haptics_cmd.c`, `notify_cmd.c`, `lichen_app.c` (app ctx), assets
  - **c:** radios
    - `wifi_service.c`, `c6link.c` (c6log), `lora.c`, `mesh_task.cpp` (notify test, ident simulate)
  - **d (after a, b and c merge):**
    - A checker, `tools/check_dev_commands.py`, plus its selftest under `tools/tests`. It holds the DM manifest copied from the epic inventory and fails when a listed subcommand is reachable without `console_register_dev` or a `CON_GATE_DEV` row (#464; `console_dev_gate` before). This is #380's "fails before, passes after" test: it would fail on today's main.
    - Wire it into `static_checks.sh` (conflict: `tools/gate_manifest.txt`).
    - Update `.claude/skills/lichen-bench/SKILL.md`, `docs/mvp-bench.md` and the `ui_overflow_sweep.py` header with the `fw dev on` precondition.
- **Rows marked `?`:** left untouched.
- **Conflict files:** `tools/gate_manifest.txt` (d only), and possibly `tools/standard-baseline.txt` size lines (commit them per merge). Console slots are unchanged: no new entries.
- **Acceptance:**
  - Full check green, including the release build from W1-A.
  - In the dev image with dev mode off, every listed subcommand answers `DEV_MODE_OFF_NOTE`.
- **Bench (remote, main):**
  - Each command once with dev mode off and once with it on.
  - `ui_overflow_sweep.py` after `fw dev on`.
  - The two-chip gate with the updated skill.
- **Reviewer focus:**
  - No user-facing command is gated by mistake (the Keep list: `mem`, `get`, `mesh`, `net`, `airplane`, `led`, ...).
  - The usage text of release builds (#396 class).
  - `mesh_task.cpp` gets no logic change.

### E381-W3: release compile-out and HTTP routes (epic's wave 3)
- **Prerequisites:**
  - W2, plus decision 4.1 and decision 4.3.
  - #229 merged (#216) for the app flag; otherwise use a temporary flag field agreed with #216.
- **A (Opus: auth surface).** HTTP route gating in release per the `?` answers.
  - Files: `ota_server.c`, `sdkconfig.release.defaults`, and the build.sh `release_checks` markers.
  - Host test: a pure route table (`ota_routes.c`) giving allowed or refused per build and route.
- **B (Sonnet).** A `dev_only` app flag. Keyboard test, Bench and Test card compile out in release, and while dev mode is off they are hidden from home.
  - Files: `lichen_api/lichen_app.h` (coordinate with #229), `lichen_app.c`, `lichen_apps.c`, `home.c`.
  - Host test: tile filtering.
- **C (Sonnet).** `tools/check_dialog_policy.py`: every `shell_dialog` call site has a row in `dialog_remote_policy.c`, failing closed.
  - Files: `tools/`, `tools/gate_manifest.txt`.
- **D (Sonnet).** `badge.sh` helpers for `ui answer`, `sd cat` and `dev status`, plus the docs.
  - Files: `tools/badge.sh`, `docs/app-api.md`, `docs/architecture.md`.
- **Parallel:** A, B, C and D. C and D merge last on `gate_manifest.txt`.
- **Security checks:**
  - The release image holds no dev markers and no gated route strings.
  - Token auth is unchanged.

### E381-W4: #383 health page and `fw health`
- **Prerequisites:**
  - #234 (#216) for the C6 probe, and #307 read-backs.
  - #11 is optional: the battery row shows "n/a".
  - Coordinate the Settings files with #393.
- **A (Sonnet).** `lichen_os/health_model.c` with `host_tests/health_model_test.c` (threshold table, all-good fixture), plus the `fw health` command (a Keep row, not DM).
  - Conflict files: `host_tests/CMakeLists.txt`, `lichen_os/CMakeLists.txt`.
- **B (Opus: C6 contract, DMA low-water, cached reads and no wait on the LVGL task).** The probe gatherers through `lichen_sys_*`.
  - Conflict file: `tools/layers.conf` if a new edge appears.
- **C (Sonnet, after A).** The Settings > Health page and the FAIL-only boot notice.
  - Bench: `fw ui overflow` = 0.
- **Bench:**
  - Remote: `fw health` lists every row, and an SD unmount turns that row to FAIL.
  - The LoRa row never transmits. Check that `fw mesh` tx is unchanged.

### E381-W5: #382 host UI sim
- **Prerequisites:** SDL2 on the dev host (decision 4.5). It runs alongside W2-W4 on separate files.
- **A (Opus: the LVGL lock and UI service fakes).** The `host_tests/ui_sim` target, fakes for `lvgl_port_lock`, heap_caps, NVS, esp_timer and tasks, and headless PNG output.
- **B (Sonnet, after A).** The keyboard map, fake data (72 emoji contacts, long SSIDs) and goldens for home, Settings, the MeshCore grid and a chat. A moved label fails the test.
- **Conflict file:** `host_tests/CMakeLists.txt`. Use an `option()`, so the gate does not need SDL until decision 4.5 is made.
- **Reviewer focus:**
  - No firmware edits.
  - Fakes match the real service contracts.
  - Anti-aliasing tolerance.

### E381-W6: #385 app SDK template
- **Prerequisites:** #229 and #231 (#216).
- **Agent:** one Sonnet agent, since it touches the conflict files.
- **Files:** `badge.sh new-app`, the template, `docs/app-api.md`, and the `docs/architecture.md` line 316 fix.
- **Gate:** scaffolds a throwaway app. Conflict files: `host_tests/CMakeLists.txt`, `tools/layers.conf`, `tools/gate_manifest.txt`.

## 6. Risks, and what to leave out

**Risks:**
- #380 edits about 30 files that other epics own. Split it and merge it in a quiet window, or it collides with #392, #262 and #393 waves.
- Dev gating quietly breaks bench scripts and other epics' proofs that call DM commands. W2-d covers this, and every epic's bench brief should start with `fw dev status`.
- A factory reset clears `dev.mode`, so the next remote session needs USB.
- The release build in the gate costs time.
- Sim fakes drift from the real services, and the goldens can flake.
- The `dev_only` flag can clash with #229's flag layout.

**Leave out:**
- A network-only enable of developer mode, and auto-expiry (Lucas's decisions).
- The webcam and the USB power switch (parked).
- #384, which belongs to #262.
- On-air tests in the health page.
- Any remote path to DANGER dialogs or the #66 confirm, which is against #182's decision.
