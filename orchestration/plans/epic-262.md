# Plan: epic #262 Memory and performance (planner, 2026-11-13, read-only)

## 1. Issue inventory (16 open; #266 #270 #275 #277 closed)
| # | Short title | Labels of note | Real status (code checked) |
|---|---|---|---|
| 263 | FatFs buffers to PSRAM | needs-proofing | Done on main (`FATFS_ALLOC_PREFER_EXTRAM=y`). Bench almost complete (format, no 32 KB dip, gate clean). Left: old-image MB/s A/B, label check |
| 205 | fw mem totals, first frame, key-to-flush | needs-proofing | Code on main (df9ae6bf, `ui_perf_stats.c`). Bench never run. key-to-flush needs physical keys (`fw keys send` bypasses the indev) |
| 254 | v2 flicker | bug, no needs-* | Open investigation. Leads: 44.6 Hz real refresh + 80 MHz fallback (now fixed by #353). Last rows calm at 120 MHz. Missing rows: cold boot on battery, C6 reset; judged by eye |
| 264 | UI statics to EXT_RAM_BSS | needs-lucas | Done on main (18.9 KB). Remote walk passed. Left: crash-log delete (careful confirm, physical Enter or #377) |
| 256 | DMA low-water below gate | needs-proofing | Code on main (dma_watch hook, 8 KB gate chunk). Lucas decision: close after one ≥36 h soak, min ≥ 80650 |
| 261 | ALWAYSINTERNAL 16 KB→4 KB | needs-proofing | Done on main (=4096). Left: `fw ui perf bench 10` + Bench app vs old, soak |
| 265 | ringbuf/I2C ISR to flash | needs-lucas, needs-proofing | Done on main. Left remote: console during OTA/NVS, sensors during OTA. Lucas: physical keys during OTA |
| 268 | Shrink task stacks | — | Not started: `SERVER_STACK 16384` (ota_server.c:73), `MESH_STACK 12288` (mesh_task.cpp:64), sensors/sys_notices/thermal 4096, main 8192 unchanged. Data: busy `fw stress 1` table + OTA task min free 10488; missing an OTA-upload figure (after a C6 bundle push) and a UI-walk taskLVGL figure |
| 269 | LP RAM for internal non-DMA | needs-lucas, needs-proofing | Code on main (`TASK_STACK_LP_RAM` → `MALLOC_CAP_RTCRAM`, lichen_task.c:70; sys_notices + thermal on lpram). Left: remote exercise + soak; Lucas: deep-sleep wake |
| 271 | portMUX touches PSRAM | needs-lucas, needs-proofing | Done on main (5 spinlocks → mutexes). Left: Heltec-side steps via meshcore session; Lucas: DM to a switched-off Heltec |
| 278 | Stress app | — | Code on main (console-only `fw stress`, worker stacks via `stack_low`). Left: `badge.sh stress-host` wrapper (was parked on #280, now closed), stress hint (#286, epic #216), a `fw stress 10` + host load run |
| 322 | Refresh 50 Hz | — | Built, held: wt-3 `feat/322-refresh-50hz` 50fc3ff0 (40 MHz div 6, VFP 51), 77 commits behind; main's `st7703_bono.h` comments changed since (#317/#354), expect a rebase conflict. Held by Lucas until #254's table points somewhere |
| 384 | Boot-time budget | area:tooling | Not started: `fw boot` exists (boot_progress.c:85), no `tools/boot_budget.txt`, no `badge.sh boot-check` |
| 121 | PSRAM 120 MHz DMA flips | needs-proofing | Mitigated (E4 bounce, #353 120 default). Lucas decision: close after a 24 h+ soak with an hourly gate, no canary/ui_check hits |
| 260 | esp_hosted frame pools | needs-proofing | Done on main (TX 10 / RX 24, PSRAM). Left: screenshot and `/log` timing vs the old path, soak |
| 267 | Heap functions to flash | needs-lucas, needs-proofing | Done on main (`HEAP_PLACE_FUNCTION_INTO_FLASH=y`, sdkconfig.defaults:211). Left remote: speed switch, OTA, NVS, sd test, soak; Lucas: power-off/deep-sleep wake (with #281/#292) |

Net: 2 issues need code (#268, #384), 1 needs a rebase + merge decision (#322), 1 a small tooling add (#278); the other 12 are code-on-main awaiting bench.

## 2. Dependencies inside the epic
- W0 bench data → #268 (sizes need the OTA-upload and UI-walk high-water marks; `fw stress 10` gives the busy figure).
- W0 three-boot median → #384 (budgets only after measuring).
- #254 rows → #322 merge (Lucas hold, 2026-11-11 protocol: nothing display-related merges until the table points somewhere).
- #322 before-numbers (`fw ui perf bench 10`, dma gates on today's image) → #322 bench.
- One ≥36 h soak closes #256, #260, #261, #269 and (with an hourly gate) #121. #268 must merge after that soak, or it restarts it (stack changes alter the memory picture).
- #278 `fw stress 10` is the repeatable load for #268's measurement.
- #384 should land before #322 so a refresh change cannot hide a boot regression (weak; optional).

## 3. Cross-epic dependencies
Needed from others:
- #381: #377 `fw ui answer` (by policy) lets the #264 crash-log delete be proofed remotely, if the policy allows it for that careful dialog; #376 policy table first.
- #216: #228 (shell tasks via `lichen_task_start`) and #208 (split mesh_task.cpp) touch the same task-create sites as #268: run #268 after #228, or coordinate one-line `MESH_STACK` edit with #208. #209 (stack frames > 1 KB) lowers peaks: fine before or after, sizes stay conservative. #281 moves sys_notices/thermal start out of the shell: re-check #269 LP RAM stacks after it. #210 boot-step refactor defines the step names #384 budgets: #384 keys on `boot_step_label()`, so a rename needs the budget file updated. #286 carries the stress hint for #278.
- #394: #281/#292 power-off sitting hosts the #267/#269 deep-sleep checks. #388 (light sleep) must re-verify LP RAM stacks (#269) and heap-in-flash (#267) and keep the DMA gate; #388 is measured first and is out of this epic. #146 (Wi-Fi RTT degrades, screenshots truncate) skews #260's timing A/B: measure on a fresh boot, repeat 3x.
- #392: #368 (journal after SD format, found by #263) does not block #263.
- #395: #391 (injected keys reach hand-only paths) limits which steps may be driven remotely; #116 NVS encryption would change NVS-write cache-off cost (#267), not now.
Others need from this epic:
- #216 #237 (MVP close-out) and #234 (two-chip gate, DMA min ≥ 80650) need the #256 soak verdict and #254 closed.
- #394 #39 (faster boot) needs #384's ratchet to show the gain; #388 needs the DMA headroom numbers.
- Every epic shares the one badge: the 36 h soak freezes all bench work.

## 4. Decisions for Lucas, missing prerequisites
1. Soak slot: when the badge goes undisturbed for ≥36 h (blocks every epic's bench). Recommended: right after W0, with builds running meanwhile.
2. May the soak run an hourly `fw psram dma` gate so it also closes #121 (8 KB dips, far above the gate)? Recommended: yes.
3. #254: run the two missing rows (cold boot on battery, C6 reset) by eye; then close #254 as "80 MHz fallback + 44.6 Hz", and release #322 or drop it.
4. #322 fallback choice if the panel dislikes VFP 51: split porch (vfp 36 / vbp 35) or APLL 38.4 MHz (check lichen_audio does not own APLL).
5. #268: a stack size is not pure logic; accept bench high-water as the proof instead of a failing host test (or a tiny pure "size ≥ peak/0.75" table test).
6. #261 1024 trial: skip (DMA min ~150 KB, no pressure). Recommended: skip.
7. One hands-on sitting (W3): physical keys during OTA (#265), key-to-flush (#205), Heltec off (#271), crash-log delete (#264 unless #377), deep-sleep wake (#267/#269 with #281/#292), #254 rows.
No solder work needed.

## 5. Waves
### E262-W0 — remote bench proofing + baselines (main only, no code)
- Issues: #263 #205 #261 #265 #267 #269 #260 #268(data) #384(data) #322(before) #278(run).
- Prereqs: none. Can start now.
- Tasks (each issue's checklist has the strings): `fw mem` totals/min_pct + `first frame at` (#205); `fw ui perf bench 10` + Bench app (#261, and #322 before-value); speed switch 80↔120, OTA, NVS write, `fw sd test` (#267); console over UART/USB during OTA + NVS write, Sensors during OTA (#265); `fw tasks` shows lpram for sys_notices/thermal, exercise thermal/notices (#269); screenshot + `/log` timing x3 on a fresh boot, A/B via `fw slot other` or a `ESP_HOSTED_HOST_SDIO_FRAME_POOLS=n` build (#260); `fw stress 10` + a UI walk + a C6 bundle push then `fw tasks` (#268 OTA and taskLVGL figures); 3 reboots, 75 s wait, `fw boot` each (#384 median); `fw psram dma sweep 16384` x3 + `dma 15` x7 + `fw mesh hist scan 100` (#322 before).
- Then start the ≥36 h soak (#256 #260 #261 #269 #121): badge undisturbed; read `fw mem` drop lines, canaries, `ui_check`, `CORRUPT HEAP`.
- Acceptance: numbers posted per issue; close the ones whose checklists are complete. Remote: all of it (keys locked, #190).

### E262-W1 — build while the soak runs (2 agents, disjoint)
- Prereqs: W0 measurements (#268 data, #384 medians). Merge only after the soak ends.
- Task A, #384 boot budget, Sonnet, slot wt-N `feat/384-boot-budget`: stable one-line-per-step `fw boot` format + total + first frame (boot_progress.c, boot_steps.c/.h, lichen_boot); pure parser + ratchet in an ESP-free .c; `tools/boot_budget.txt` (W0 median per step, Wi-Fi/SNTP excluded); `tools/badge.sh boot-check`. Then #278 `badge.sh stress-host <host> <min>` in the same branch (badge.sh owner; OTA token over ssh stdin, never argv). Host test: parser + ratchet fail on a slow fixture, fails before/passes after. Bench (main, remote): `badge.sh boot-check` green after a reboot; `badge.sh stress-host` run with `fw stress 10`.
- Task B, #268 stack shrink, Opus (task and memory books), slot wt-N `perf/268-stack-shrink`: size each task at ≥ peak/0.75 (code standard ≥25 % free) from the W0 figures only; candidates ota (min free 10488 of 16384 → ~8-10 KB once the upload figure is in), mesh (min free 8988 of 12288 → ~6-8 KB), sensors, sys_notices, keyboard, LoRa, psram_guard; then `ESP_HOSTED_HOST_DEFLT_TASK_STACK_BYTES` and main (`CONFIG_ESP_MAIN_TASK_STACK_SIZE` 8192) only if measured. Files: ota_server.c, mesh_task.cpp (MESH_STACK line only), sensors.c, sys_notices.c, thermal.c, keyboard/lora/psram_guard start sites, firmware/sdkconfig.defaults. Bench: `fw stress 10`, OTA upload, UI walk; every task free ≥ 25 %; `fw mem` dma free gain recorded. Never shrink a task without a number.
- Conflict files: host_tests/CMakeLists.txt (A only), tools/standard-baseline.txt (both: sequence merges A then B), sdkconfig.defaults (B only), tools/gate_manifest.txt (A if boot-check joins the gate list).
- Reviewer focus: A: parser robustness on truncated/garbled console output, budget only-shrinks logic, token handling in badge.sh. B: every new size traced to a measured peak, ISR/cache-off stacks, tasks that touch NVS/flash stay internal, esp_hosted fork order if the stack Kconfig lives there.
- Performance: no RAM threshold beyond "free does not drop, DMA min ≥ 80650"; record the gain.

### E262-W2 — #254 close-out and #322 (main + 1 agent)
- Prereqs: decision 3 (the #254 rows); W1 merged (so the soak is not disturbed by a display change).
- Task: rebase wt-3 `feat/322-refresh-50hz` on main, resolve the `st7703_bono.h` comment conflict, keep the host test that pins the applied clock (display_timing_test). Opus (hardware-adjacent, PSRAM scan-out bandwidth). Files: firmware/components/board/st7703_bono.h, host_tests/display_timing_test.c.
- Acceptance: `fw get disp.` pclk 40000000, refresh 500 (±5 dHz); dma gates x3/x7 0 bad bytes, `fw mesh hist scan 100` clean; `fw ui perf bench 10` within the spread of W0's before-value (threshold set from W0's three runs). Lucas: test card calm at 10 % backlight, no tearing (needs hands/eyes).
- Reviewer focus: divider maths (240/n), vtotal, refresh read-back.

### E262-W3 — hands-on sitting (main + Lucas, no code)
- #265 physical keys during OTA; #205 key-to-flush after physical keys; #271 DM to a switched-off Heltec; #264 crash-log delete (or remote via #377); #267/#269 power-off and deep-sleep wake (joined with #281/#292); #254 rows if W2 still needs them. Then close what passes; close epic #262 when its list is empty.

## 6. Risks, and what to leave out
- One badge: the soak and every other epic's bench compete; a reflash resets the soak clock.
- #268 overflow risk: the OTA-upload peak was lost once to the auto-restart; take it after a C6 bundle push. Stack canary/overflow hook must stay on.
- #322 is +17 % scan-out PSRAM reads; at 120 MHz the SDMMC→PSRAM direct path is unused, but rerun the full gate.
- #384 boot-to-boot variance (SD, C6 sync): use medians and a margin, a hardware gate cannot run in build.sh.
- #269 LP RAM stacks become a #388 light-sleep risk later.
- Leave out: 200 MHz PSRAM; PSRAM frame pools near SDMMC DMA (direct D=0 in one session is not proof); ALWAYSINTERNAL 1024 trial; RX/TX pool depth tuning beyond 24/10 unless #260 timing is short; esp_hosted `s_tx_buf`/mdns packet (closed with #270); #388 light sleep (hardware epic, measure first).
