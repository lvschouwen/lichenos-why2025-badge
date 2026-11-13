# Epic #216 plan: platform and architecture (Lichen OS v2)
Planner, 2026-11-13, read-only, main at 47dc849e. Status checked in code, not from titles.

## 1. Issue inventory (36 open)
| # | Short title | Labels of note | Real status |
|---|---|---|---|
| 234 | P4<->C6 contract + two-chip gate | needs-proofing, s8 | code on main; gate ran remotely (#335); stays open until it has also run after the next wave |
| 281 | thermal/notices without display | needs-lucas, now, s8 | code on main; remote part passed; power-off steps need hands (sitting G) |
| 237 | W9 close-out, MVP candidate | needs-lucas, needs-proofing, now | code done; hands-sitting checklist posted, not yet run |
| 274 | FATFS_FS_LOCK | needs-lucas | `CONFIG_FATFS_FS_LOCK=20` on main; left: Notes delete (hands), Files copy + ota-rm 409 (remote) |
| 273 | dialog strings under want_lock | needs-lucas | code on main; left: physical Enter confirm, cross-task dialog |
| 286 | boundary-audit spec fixes | needs-lucas | code on main (0 raw lv_async_call, 0 plain malloc in shell); left: Esc-hold 3 s, Clear-all dialog |
| 180, 210, 220-228, 235, 244, 245 | L-0..L-C waves (2A apps, boot steps, gate split, ui ops, lora cb, c6link hooks, backlight NVS, LVGL lock, sys_display, sys_update, books, NVS owner, sys surface, lower layers) | needs-proofing | code on main, layer baseline 0; await the #237 sitting / remote sections |
| 235 (tail) | narrow `owner-only nvs_open*` | (in #235 audit comment 11-10) | not started: owners still lichen_os, lichen_nvs, lichen_boot, crash, psram_guard, c6link (layers.conf:182) |
| 335 | MeshCore applied radio | needs-lucas | code on main; left: slider on panel, cold boot, physical key after c6 reset |
| 313 | registry wave 3 | - | done in code (#331-#334 closed, 3e = #335); close with #335 |
| 307 | registry sub-epic | epic | waves 1-3 merged; close after #313/#335 |
| 209 | stack frames >1 KB | - | 209a done; 209b partly: dialog apply fixed by #273 (now 32 B); left `mesh_notify_drain` 1872, `tl_view_text` 1728, `lora_task` 1360, `lora_cfg_parse` 1056, mesh_task lambda 1760 |
| 208 | split mesh_task.cpp / cmd_mesh | - | not started: mesh_task.cpp 3014 NLOC (3435 lines), cmd_mesh CCN 34 |
| 212 | CCN group | - | not started; baseline list differs from issue: now also `ota_proto.c:parse_header` 29 (+nest 11), `ota_server.c:handle_piece` 31, nest `c6_boot_plan` 7, `mn_decide` 7; `lichen_notify_to`/`lora_task` no longer listed |
| 213 | 9-11 params | - | not started (6 entries in baseline) |
| 229 | L-D1 capabilities | - | not started (no CAP_* anywhere; manifest cap column is a seed) |
| 231 | L-E2 lifecycle | - | not started (no app_life) |
| 232 | L-E3 crash attribution, safe mode | - | not started |
| 233 | L-E4 Settings › Apps | - | not started |
| 349 | L-E1f callbacks run as their app | - | not started (123 `lv_obj_add_event_cb` sites; `lichen_ctx_enter/leave` exist from #336-#340) |
| 298 | delta OTA spike | - | spike done; decision 11-13: deflate pieces + drop 20 s auto-restart, re-measure. Not started (pieces still refuse deflate, ota_server.c ~1012; delay in update_screen.c:331-341) |
| 328 | log-IDs sub-epic | epic | measured: ~25 KB movable, not worth it now. Parked |
| 78 | WAMR Scripts app | - | not started; probe waits for L-D/L-E |
| 371 | esp_hosted 3.1.0 rebase | - | not started; Lucas 11-13: "not now" |
| 385 | app SDK template (tooling theme, ordered here) | - | not started; waits for #229 + #231 |

## 2. Dependencies inside the epic
- #208 before #229: D1 folds the per-app console prefix into a command table that must exist first.
- #229 before #231: PSRAM=/INTERNAL_RAM= caps feed the lifecycle quota; both edit `lichen_app.h` and `layers.conf`.
- #232 before #231's FAULTED state wiring (or merged in the same wave, #232 first).
- #229, #231, #232 before #233 (the page shows caps, background on/off, crash count).
- #229 + #231 before #385; #229 + #231/#232 before #78 probe.
- #335 before closing #313, then #307.
- #237 sitting closes #281, #273, #286, #274 (with remote leftovers) and lets the needs-proofing wave issues close; #234 closes only after a later wave reruns its gate (W1 or W2 below).

## 3. Cross-epic dependencies
Needs from others:
- **#381 tooling:** #376/#377 (`fw ui answer`) so careful dialogs can be proofed remotely (#273, #286, #274 delete step); #391 (hand-only = TCA8418) before the #237 sitting's factory-combo step is meaningful. #232's `fw app crash <id>` must sit behind developer mode (#373, done).
- **#262 memory:** #256 (DMA low-water 68423 < 80650 after soak) decides whether the #234 gate holds; any internal-RAM move in #209b (static buffers) is checked against #262's internal budget; #231's PSRAM quota extends `mem_book`.
- **#395 security:** #116 (NVS encryption) shares the `nvs_boot_policy` boot path with #232: never in the same flash. CAP_SECRETS list in #229 must match #116's protected set.
- **#394 hardware:** CAP_BT (#73), CAP_RADIO_SCAN (#172), CAP_AUDIO, CAP_BATTERY must be named in #229 even if unused.

Others need from this one:
- **#392 MeshCore:** #208 and the mesh hygiene task (W1-B) rewrite mesh_task.cpp and ~9 mesh files: MeshCore feature work must avoid those files during W1 or rebase after. #229 changes the MeshCore app descriptor and its console prefix.
- **#381:** #385 waits for #229 + #231.
- **#393 UI:** #233 lands on `app_settings.c`, which UI work also edits: one owner per wave.

## 4. Decisions for Lucas, missing prerequisites
1. **#235 tail:** narrow the remaining NVS owners in a new small issue (Recommended: lets #235 close at the sitting), or close #235 and drop it.
2. **#212 `lora_decode`** (shared/lora, built into the C6): do it now with an `ota-c6`-first OTA and a C6 version bump (#279 check), or defer it to the next C6 change (Recommended: defer, saves an OTA pair).
3. **#328** close as "not now, measured" (Recommended) or keep parked.
4. **#371** stays out of this program (Lucas already said not now).
5. **#78** probe after W3, build-only, not merged (Recommended), or drop from the program.
6. The hands sitting (#237) itself: date and fw build. Order: sitting on the current image before W1 merges (Recommended: W1 then re-proofs only what it touches).
No solder work or new hardware is needed by this epic.

## 5. Waves
Every wave: full `firmware/build.sh build` green, `tools/standard-baseline.txt` shrinks (regenerate with `check_standard.py --shrink` after each rebase, merge one branch at a time), one reviewer per task, then the #234 two-chip gate remotely after the OTA.

### E216-W0 (main + Lucas only, no code). Prereq: none (#391/#377 help).
Remote sections of docs/mvp-bench.md on the current image, then the hands sitting on #237. Closes the needs-proofing/needs-lucas set (#180 #210 #220-#228 #235 #244 #245 #222 #236 #281 #292 #273 #274 #286 #335), then #313 and #307. Acceptance: checklist ticked; failures become linked priority issues.

### E216-W1 refactors + OTA cheap wins. Prereq: none (W0 recommended first).
| Task | Issues | Model | Files |
|---|---|---|---|
| A | #208 split mesh_task.cpp (commands into table-dispatched files, lambda 1760 B frame, `heap_caps_calloc`:296 → lichen_alloc) | Opus | mesh_task.cpp, new mesh_cmd_*.cpp, mesh/CMakeLists.txt, layers.conf (if the calloc line moves) |
| B | mesh hygiene: #209b `mesh_notify_drain`, `tl_view_text`; #212 `scan_task_fn`, `rs_check`, `mesh_run_event`, `lpp_type`, `one_value`, nest `mn_decide`; #213 `mesh_conv_row`, `mesh_hist_compact`, `radio_confirm_needed` | Sonnet | mesh_notify_run.c, mesh_telem.c, mesh_journal.c, mesh_rset.c, mesh_run.c, mesh_notify.c, mesh_chat.c, mesh_hist.c, radio_confirm.c |
| C | protocol + driver: #212 `mf_decode`, `rr_request_prepared`, nest `c6_boot_plan`; #209b `lora_task`, `lora_cfg_parse` (`lora_decode` only if decision 2 = now) | Opus | mesh_frame.c, mesh_remote.c, c6link/c6_boot_policy.c, lora/lora.c, lora/lora_link.c |
| D | OTA path: #298 deflate through pieces + drop the 20 s auto-restart; #212 `handle_piece`, `parse_header` (CCN + nest 11), `update_screen.c:tick` | Opus (protocol, boot/update path) | ota_server.c, ota_proto.c, ota_pieces.c, update_screen.c, tools/badge.sh |
| E | misc: #212 `utf8_len`, `files_text_page`, `sensors_task`; #213 `files_text_page`, `crash_record_format`, `shot_blend_argb8888` | Sonnet | wifi_list.c, files_model.c, sensors.c, crash_log.c, shot_blend.c |
All five in parallel (disjoint files). Conflict files: standard-baseline.txt (all; regenerate per merge), host_tests/CMakeLists.txt (D adds a piece-inflate test, A/C if new tests; union-merge), mesh/CMakeLists.txt (A only), layers.conf (A only).
Acceptance: behaviour identical except D; baseline entries removed (mesh_task nloc < 1500, the 15 CCN / 6 params / 5 stack entries gone or lowered); `fw mem` stack_free per task before/after for lora, mesh tasks.
Host tests: existing mesh_frame/lora_proto/remote/telem/files_model/crash_log tests must stay green and get cases for any branch split (pure decoders: round-trip tables). D: a deflate-piece test that fails today (pieces refuse Content-Encoding) and passes after, plus an inflate-bomb bound (output > piece cap refused); the restart-delay rule as a pure test.
Bench (remote): D: `ota-firmware` with deflate, time split re-measured and posted on #298 (decides delta); A/B/C: `fw mesh` commands reply unchanged (diff against a captured transcript), MeshCore RX + one TX to #badgetest, `fw lora`, C6 reset recovery; #234 gate.
Security/perf: D: inflate output bounded, per-piece SHA and `X-Content-SHA256` still checked after inflate, token auth unchanged; C: decoders keep every length check (reviewer diff-checks bounds). Static buffers in B/C counted against internal RAM (prefer PSRAM via lichen_alloc where not DMA).
Reviewer focus: A/C/D Opus-adjacent risks (mesh task concurrency, decoder bounds, OTA install path); B/E faithful refactor.

### E216-W2 capabilities + crash attribution. Prereq: W1-A (#208); #116 not in the same flash.
| Task | Issue | Model | Files |
|---|---|---|---|
| F | #229 caps: CAP_* incl. RADIO_SCAN, BT, AUDIO, BATTERY; `lichen_app_t.caps` mirrored in layers.conf; rule D in check_layers.py; run-time refusal (rate-limited log) at lichen_os entry points; per-app console prefix; `lichen_radio_set` SYSTEM-only; forbid L6 abort/adjtime/sntp_*; `console_line_max()`; mesh_notify_run DND double-check removed; ~190 manifest thread rules fixed | Opus | lichen_api/lichen_app.h, all app descriptors (lichen_apps/*, mesh, shell apps), lichen_os entry points, fw_console, check_layers.py, layers.conf, gate_manifest.txt, docs/app-api.md, architecture.md |
| G | #232 crash attribution: owner app id to RTC no-init (64 B), next-boot notice, pure safe-mode policy (2 in 3 boots → background off), "degraded boot" state replacing mesh_rhist_run's own throttle, task-WDT heartbeats for app loops, `fw app crash <id>` behind developer mode | Opus | crash/*, lichen_os app registry/start_background path, main boot step, new safe_mode_policy.c, mesh_rhist_run.c |
F and G in parallel; merge G first (smaller), F rebases. Conflict files: host_tests/CMakeLists.txt (caps_test.c, checker fixture; safe_mode_policy_test.c), layers.conf + gate_manifest.txt (F only; G adds no gate symbols or asks F), lichen_os/CMakeLists.txt (union), boot_order[] (G only, if a step is added).
Host tests: caps_test.c (an undeclared cap refuses); check_layers fixture (undeclared symbol fails rule D); safe_mode_policy_test.c (2 in 3 → off, re-enable resets), crash-record encode/decode round trip.
Bench: remote: MeshCore send/receive, LoRa spectrum borrow, `fw app caps`, 0 refusals in normal use; `fw app crash timer` → reboot → "Timer crashed", second → background off, re-enable; MeshCore unaffected; #234 gate. Keyboard test raw keys needs Lucas's hands (or #377-era tooling).
Security: caps documented as accident guard; CAP_SECRETS = exactly Wi-Fi, repeater passwords, OTA token (#116). Perf: one compare per gate call; `fw ui perf` p95 ≤ 33 ms; flash delta within the +8 KB archive rule.
Reviewer focus: F refusal paths never fire for the OS itself, manifest/header agreement; G panic-path writes (no allocation, no locks), RTC layout versioned.

### E216-W3 lifecycle + callback attribution. Prereq: W2.
| Task | Issue | Model | Files |
|---|---|---|---|
| H | #231 pure app_life.c state machine; stop(reason) generalising stop_for_update; PSRAM quota in mem_book; booked lichen_timer; move main.c radio-switch/update-interlock/power-off sequences to service begin/end hooks; update_mode.c to L3; EXT_RAM_BSS statics in mesh_store.c, meshcore_notify.c, weather_screen.c, timer_service.c into the book | Opus (task and memory books) | lichen_os (app_life, mem_book, timer), main.c, lichen_shell/update_mode.c, those four app files, lichen_app.h |
| I | #349 LVGL event/timer/async registration wrapper storing the ctx, enter/leave around the call, 123 sites migrated; ISR/esp_timer stay OS | Opus (UI service) | lichen_api/lichen_ui*, lichen_os UI service, call sites in shell, apps, mesh |
| J | #235 tail if decision 1 = yes: bind crash counters, factory_reset flags, c6link one-shot flags via settings binders; narrow `owner-only nvs_open*` | Opus (boot path) | crash.c, factory_reset.c, c6link.c/c6_sup.c, psram_guard console, layers.conf |
Merge order I → H → J (I touches many files mechanically; H and J rebase). Conflict files: layers.conf (H lichen_timer owner-only, J nvs line: sequence), host_tests/CMakeLists.txt, boot_order[] (H only), lichen_app.h (H only).
Host tests: app_life_test.c (every transition, deadline expiry → FAULTED, never deletes foreign tasks), mem_book_test quota refuse, ui callback ctx test (fails today: allocation from a timer lands on the OS), check_layers fixture for an unsanctioned nvs_open (J).
Bench (remote): OTA goes through stop(UPDATE); `fw mem` shows a mesh screen timer's allocations under mesh; `fw app ctx` 0 refused; p95 ≤ 33 ms; `ota-reboot` loop ≥ 10 (boot path changed); C6 reset; #234 gate; 1 h soak with `fw ui lockcheck` 0.
Reviewer focus: H deadlines and cooperative stop under the LVGL lock; I no lock taken inside the wrapper, ctx restored on every path; J boot ordering before the settings service.

### E216-W4 Settings › Apps. Prereq: W2 + W3; no #393 agent on app_settings.c in the same wave.
K: #233 (Sonnet): one page per app (caps, data size, Clear data via careful dialog, Background on/off, crash count), docs/ui-standard.md rules cited. Files: lichen_shell/app_settings.c or a new app_settings_apps.c, ui text. Host test: ui_text_test strings, page model pure test. Bench: remote `fw ui overflow` 0 on each page, Clear data needs a physical Enter (or #377). Then #385 (tooling epic) can start.

### E216-W5 optional: #78 WAMR probe. Prereq: W2, W3; decision 5.
Build-only branch, not merged: interpreter, one native (lichen_notify), linear memory in PSRAM; report flash, internal RAM, frame time. Opus (memory). No bench.

## 6. Risks and leave-outs
- W1 touches code whose bench steps are in the pending sitting: run W0 first or re-proof mesh/lora/OTA steps.
- W1-D changes the update path: a broken inflate bricks OTA only, USB rescue stays; keep the full-image fallback and test ota-reboot ×10.
- #208 and W1-B collide with MeshCore feature work (#392): freeze mesh files for that wave.
- #229 touches every app descriptor: sequence after all other app-file work in flight.
- #232 and #116 never in one flash.
- #256 may move the DMA gate; until settled, gate on baseline − 10 %.
- Left out: #371 (Lucas: not now; +12 KB DMA), #328 (measured, < 1 % of image), delta OTA (#298 decision; revisit after W1-D numbers), #212 `lora_decode` unless decision 2.
