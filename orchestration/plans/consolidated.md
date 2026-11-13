# Consolidated program plan: epics #216 #262 #381 #392 #393 #394 #395
Senior review, 2026-11-13, read-only, main green and pushed (incl. #374). I checked the disputed claims in code and on the issues: `boot_keys_play.c:22,53` injects; `factory_combo.c:9-10,48` refuses injected arming. `tools/standard-baseline.txt:21,28,29,34,39` puts `scan_task_fn` in mesh_journal.c, `parse_header`/`handle_piece` in ota_*, and `mesh_conv_row` in mesh_chat.c. Lucas decided #298 on 11-13 (deflate + drop the 20 s restart). The DMA gate is defined in `.claude/skills/lichen-bench/SKILL.md:41` and `docs/mvp-bench.md:94`.

**Bottom line:** of about 115 open issues, roughly 85 are bench debt. The program is limited by the badge, not by agents. Code only needs to stay ahead of the bench. One OTA now is the highest-leverage step, because it unlocks every remote proof.

## 1. Recommended epic order
**#381 → #395 → #216 → #392 → #262 → #393 → #394** (priority when waves compete for the badge or for slots; waves still interleave).

| Change | Reason |
|---|---|
| #381 first (was 3rd) | Its W0 (OTA of the current main plus the #376/#377/#378/#379/#391 proofs) is a prerequisite for the remote sweeps of #216, #262, #392 and #393. Without `fw ui answer`/`fw sd cat`/`fw boot keys`, about 30 steps stay needs-lucas. |
| #395 second (was last) | #391's bench proves the hand-only rule that every other remote proof relies on. #211 is the most exposed parser and collides with #216 W1-D (it must go first, see §2). #288 must land before #229 (CAP_SECRETS), #233 (Clear data), #387 and #169, which all consume the secrets class. #289 must be in place before E392-W3's on-air dialog proofs. #116 (the burn) stays last. |
| #216 third (was 1st) | Its W0 is bench debt that runs with everyone else's. #208 goes into Round 1 because it gates #229 and collides with #289, #380 and #268 on `mesh_task.cpp`. |
| #262 fifth (was 2nd) | All its code (#268, #384) needs W0 numbers. The 36 h soak goes after the second OTA, not right after W0 (§5). |
| #394 last | It hangs on #226, #229, #234 and on the soak's DMA headroom. Light sleep (#388) is the riskiest item and is main-session only. |

## 2. Dependency graph
Edges (`A -> B`: A before B):
- #377,#378,#379 proven -> remote proofs in #66 #128 #155 #162 #163 #167 #169 #189 #264 #273 #274 #286 #291: tooling first.
- #391 bench -> #289 bench; #391 -> #379 retarget: the hand-only rule decides what boot keys may do.
- #211 -> #298 (E216-W1-D): same functions and files (`ota_proto.c`, `ota_server.c`, `ota_pieces.c`).
- #389, #368 -> E216-W1-B: W1-B refactors `mesh_conv_row` (mesh_chat.c) and `scan_task_fn` (mesh_journal.c).
- #208 -> #229 (command table for the per-app prefix); #208 -> #289, #380c, #268 (all edit `mesh_task.cpp`).
- #288 -> #229 (both add fields to `lichen_app_t`; CAP_SECRETS maps onto #288's declaration); #288 -> #233, #387, #116.
- #288 benched (own flash) -> #116 burn; #232 never in the same flash as #116 (`nvs_boot_policy`).
- #229 -> #231 -> #233, #385; #232 -> #231 FAULTED; #229 -> E381-W3-B (`dev_only` flag), #394 W4c/W5/W6 (CAP_*).
- E393-P1 (split app_settings.c) -> #387 -> #386; P1 -> #233, #383 page.
- #164 benched, #126 closed, UI lockcheck at 0 -> #386.
- S1 figures -> #268, #384; soak -> #268 merge (a stack change restarts the soak).
- #254 rows (H1) -> #322 merge; S1 `ui perf` before-value -> #322 bench.
- Soak DMA headroom -> #67 I2S, #73 NimBLE budgets.
- #234 contract section -> #172 C6 service, #73 C6 BT-off, #388 wake path; #223 -> #172 P4, #73.
- #226 -> #11 UI, #67 settings, #388.
- W1-C (`lora.c`, `c6link`) -> #380c (`lora.c`, `c6link.c`); #268 -> #380a/b (`thermal.c`, `psram_guard.c`, `lichen_task.c`).
- #298 -> #384 (both edit `tools/badge.sh`).
- #380 -> E381-W3; the release build in the check (follow-up) -> E381-W3.
- #163 default proven -> #291; #52 position -> #171; #117 login -> #131 #132 #135 #137 (one session).
- #11 driver -> #162 battery value, #383 battery row, #388 inputs.

Wave level: `S1(OTA-1) -> R1 merges -> OTA-2 + H1 -> SOAK -> R3 (#268, #288/#289, #380) -> R4 (#229/#232, #322, #67) -> R5 (#231/#349, E381-W3, #387) -> R6 (#233, #385, #386, #116 burn) -> R7 (#388, #73)`.

**Contradictions between plans:**
1. **#379 vs #391** (381, 393, 395): #379's step "boot keys esc+bksp shows #66" cannot pass, as the code confirms. #391 is right. Retarget #379 to a negative proof (refusal logged). This needs **no code**, so E381-W1-C is dropped.
2. **#211 (E395-W1a) vs E216-W1-D (#212 `handle_piece`/`parse_header`)**: both plans refactor the same two functions. #211 owns them (security, mutation harness). W1-D shrinks to #298 deflate + the restart delay + `update_screen.c:tick`, after #211.
3. **E216-W1-B vs E392-W1**: W1-B claims mesh_chat.c and mesh_journal.c, which #389 and #368 edit. Bug fixes first; W1-B rebases.
4. **#289 vs #208 order**: 395 wants #289 first, 216 wants #208 in W1. #208 first, because #289 is blocked on decisions 1-3 and a hook in a dispatch table is the cleaner rebase.
5. **DMA gate**: 394 cites "#234 DMA free ≥150 KB". The live gate is free ≥89981 / min ≥80650 (baseline −10 %); 150 KB is the architecture target (`architecture.md:306`). #67/#73 budgets must use headroom over 80650. The docs disagree (issue to file, §8).
6. **Badge image**: 381 says fw-664dea27, 393 says fw-5119efd5. Read `fw` before OTA-1. Either way, nothing newer than wave 1 of #381 is proofed.
7. **When to run the hands sitting**: 216 wants it on the current image before W1 merges, 262 wants the soak right after W0. I put H1 on OTA-2 (Round 1 included) and the soak after H1. Lucas's hands are scarcer than OTAs, so proving the newer image saves a second hands pass. If Lucas is free before OTA-2, run H1 on OTA-1 and remote re-proofs cover Round 1.
8. **#225 lockcheck 1 h** (393) vs a 1 h soak in E216-W3: one 1 h UI-driven `fw ui lockcheck` run in the OTA-2 session covers both. The idle 36 h soak does not exercise the UI.

## 3. Critical path
OTA-1 → S1 (tooling proofs) → #208 + #211 merged → OTA-2 + H1 → 36 h soak → #288 (own flash) → #229 → #232 → #231 + #349 → #233 → #385. The #116 burn and #388 light sleep trail it. About 7 rounds; the soak and Lucas's sittings are the long poles, not agent time.

## 4. Global wave queue
Kind: BR = bench-remote, BH = bench-hands, C = code. Free slots: wt-1 (after `agent-wt done wt-1`, #374 is done), wt-2, wt-4, wt-5, wt-6; wt-3 is held by #322.

| Wave | Issues | Kind | Prereqs | State | Agents/model | Slots |
|---|---|---|---|---|---|---|
| **S1** = E381-W0 + E395-W0 + E262-W0 (measure) + E392-W0 + E393-W0 + E216-W0 remote + E394-W0 remote | tooling #376 #377 #378 #379 #391 #374; #116 readout; #205 #261 #263 #265 #267 #269 #260 #268/#384 data; #126 #50 #111 #127 #53 #166 #117 #137 #132 #168; #133 #135 #136 #138 #131 #189 #236 #169 #155 #161 #128 #123 #351; mvp-bench remote, #274 #335; #370 #146 | BR | OTA-1 ok | NEEDS_LUCAS (OTA ok only) | main | 0 |
| R1-code (E395-W1a, E216-W1-A, E392-W1 T1/T2, E393-P1) | #211 #208 #389 #368 P1 | C | none | READY | 2 Opus, 3 Sonnet | 5 |
| E216-W1-B' | #209b/#212/#213 mesh hygiene | C | #389, #368 merged | PLANNED R2 | Sonnet | 1 |
| E216-W1-C | `mf_decode`, `rr_request_prepared`, `c6_boot_plan`, `lora_task`, `lora_cfg_parse` | C | none (before #380c) | PLANNED R2 | Opus | 1 |
| E216-W1-D' (renumbered) | #298 deflate + restart delay, `update_screen:tick` | C | #211 merged | PLANNED R2 | Opus | 1 |
| E216-W1-E | misc #212/#213 | C | none | PLANNED R2b | Sonnet | 1 |
| E262-W1-A | #384 (+#278 stress-host) | C | S1 boot medians; after #298 (badge.sh) | BLOCKED S1 | Sonnet | 1 |
| E262-W1-B | #268 | C | S1 figures; merge after soak | BLOCKED S1, soak | Opus | 1 |
| E394-W1 | #11 driver, absent path | C | none | PLANNED R2 | Opus | 1 |
| E392-W1 T3 | #52 note (to file) | C | issue filed | PLANNED R2b | Sonnet | 1 |
| E381-W1-B | ✕ key starts MeshCore (to file) | C | issue filed | PLANNED R2b | Opus | 1 |
| E381-W1-A | release build in the check | C | decision 21 | NEEDS_LUCAS | Sonnet | 1 |
| E381-W1-C | #379 parser | - | dropped (contradiction 1) | - | - | - |
| **OTA-2 + H1** = E216-W0 hands, E393-W1, E262-W3, E392-W4, E381/E395 hands, E394-W0 hands | §5 | BR+BH | R1 merged | NEEDS_LUCAS | main | 0 |
| **SOAK** (E262-W0 tail) | #256 #260 #261 #269 #121 | BR passive | after H1 | NEEDS_LUCAS | main | 0 |
| E395-W1c | #288 (own OTA) | C | decisions 4-5 | NEEDS_LUCAS | Opus | 1 |
| E395-W1b | #289 | C | decisions 1-3, #208 | NEEDS_LUCAS | Opus | 1 |
| E381-W2 a/b/c then d | #380 + bench docs | C | S1, #208, W1-C, #268 merged | BLOCKED | 3+1 Sonnet | 3 |
| E392-W2 | #163 reset clears scope | C | decision 11 | NEEDS_LUCAS | Sonnet | 1 |
| E393-W2 | bugs from S1 + "99+" | C | S1 | BLOCKED S1 | Sonnet/Opus | 1-2 |
| E216-W2 | #229 F, #232 G | C | #208, #288 merged | BLOCKED | Opus×2 | 2 |
| E262-W2 | #322 rebase | C+BH | #254 verdict (H1), soak | BLOCKED | Opus | wt-3 |
| E394-W2 | #67 I2S | C | soak headroom | BLOCKED soak | Opus | 1 |
| E381-W4 | #383 | C | #234 closed, P1 | PLANNED R4 | Sonnet+Opus | 2 |
| E216-W3 | #231 H, #349 I, #235-tail J | C | W2; J: decision 13 | BLOCKED | Opus×3 | 3 |
| E381-W3 | route gating, `dev_only`, dialog-policy checker, badge.sh helpers | C | #380, decision 5, #229 | NEEDS_LUCAS | Opus+3 Sonnet | 4 |
| E393-W3 | #387 (T1 pure, T2) | C | P1, decision 8, #288 | NEEDS_LUCAS | Sonnet | 1-2 |
| E392-W3 | #167 #162 #163 #291 #53 dialogs | BR | #377, #289, decisions 11-12 | BLOCKED | main | 0 |
| E394-W3 | #172 C6 service | C | #234 section | BLOCKED | Opus | 1 |
| E216-W4 | #233 | C | W2, W3, P1 | BLOCKED | Sonnet | 1 |
| E381-W6 | #385 | C | #229, #231 | BLOCKED | Sonnet | 1 |
| E393-W4 | #386 | C | decision 9, #387, #164, lockcheck 0 | NEEDS_LUCAS | Sonnet+Opus | 2 |
| E394-W4/W5 | #172 P4+app, #67 settings, #11 UI | C | W3, #223, #226, #229 | BLOCKED | mixed | 2-3 |
| E395-W2 | #116 burn + defaults flip | BH+C | #288 benched, no migration pending | NEEDS_LUCAS | main + Sonnet | 1 |
| E381-W5 | #382 sim | C | decision 22 (SDL2) | NEEDS_LUCAS | Opus+Sonnet | 1 |
| E394-W6/6b | #73 BLE | C+BH | #234, #223, budget, phone | NEEDS_LUCAS | Opus | 2-3 |
| E394-W7/W8 | #388 table, light sleep | C | meter numbers, decision 17; W8 main only | NEEDS_LUCAS | Sonnet / main | 1/0 |
| E216-W5 | #78 probe | C | W3, decision 16 | NEEDS_LUCAS | Opus | 1 |

**Execution rounds.** Shared files: `mesh_task.cpp`, `mesh/CMakeLists.txt`, `app_settings.c`, `lichen_app.h` + all app descriptors, `tools/badge.sh`, `ota_server.c`, `lora.c`, `c6link.c`, `thermal.c`, `psram_guard.c`, `lichen_task.c` and the task-create sites, `main.c`/`boot_order[]`, `host_tests/CMakeLists.txt`, `tools/standard-baseline.txt`, `layers.conf`, `layers-baseline.txt`, `cppcheck-baseline.txt`, `gate_manifest.txt`, `sdkconfig.defaults` (P4 and C6), `dialog_remote_policy.c`, `shell.c`/`shortcut.c`.

- **R1:** R1-code ×5 in parallel with OTA-1 + S1. Merge order: #389 → #368 → P1 → #211 → #208.
- **R2:** OTA-2 → re-proof → H1 → soak start. Code (merged to main, no OTA during the soak): W1-B, W1-C, #11 driver, #298, #384. Merge order: W1-B → W1-C → #11 → #298 → #384 (badge.sh last). R2b, as slots free up: W1-E, #52 note, ✕ key, and #268 built but held.
- **R3 (after the soak):** merge #268 first → OTA-3 (re-measure #298 OTA time, `boot-check`). Code: #288, #289 (sequence #289 → #380c on the mesh dispatch files), #380a/b/c, then d; #163; E393-W2. Merge order: #288 → #289 → #380a → #380b → #380c → #380d. #288 rides alone on OTA-4 (one migration per flash).
- **R4:** #232 → #229 (G first, smaller; F rebases), #322, #67, #383.
- **R5:** #349 → #231 → #235-tail (216's order), E381-W3, #387, #172 C6 (`ota-c6` first).
- **R6:** #233, #385, #386, #394 W4/W5; the #116 burn in its own sitting with no other migration pending.
- **R7:** #388 W7 → W8 (main only), #73, #78.

## 5. Single-badge bench schedule
- **OTA-1 (ask Lucas):** `ota-c6` (same bundle) first, which also captures #268's OTA-task figure after a C6 push, then the P4 image. Wait 75 s, run the two-chip gate, `fw dev on` over USB. Before it, run a release build once (the #396 class).
- **S1 (1-2 remote sessions, in order):** (1) 3 reboots with `fw boot` (#384 medians), and #260 timing ×3 on a fresh boot. (2) Tooling proofs: #376/#377/#378/#379 (negative)/#391/#374. (3) `fw mem`/`ui perf bench 10`/dma sweeps (#205, #261, #322 before-value), `fw stress 10` + UI walk (#268). (4) UI sweep + E393 remote steps. (5) Heltec chat block, then the advert/discover block, then ONE read-only repeater login for #117 #131 #132 #135 #137; #168 read, then off. (6) mvp-bench remote sections, #274 Files copy and ota-rm. (7) #370 1 h CRC and #146 can run passively in parallel with (4)-(6). Failures become linked priority issues for R2.
- **OTA-2 session:** #265 console during the OTA; re-proof the Round 1 changes (`fw mesh` transcript diff for #208, a file push + `/info` + `/log` + `/console` for #211, #389 states on #badgetest, and #368's `fw sd format ERASE`, only after S1's #127 50-message proof); a 1 h UI-driven lockcheck (#225).
- **Soak:** starts right after H1 on the OTA-2 image. ≥36 h, passive reads only, hourly `fw psram dma` (decision 2). Agents build and merge R2 meanwhile, so the soak costs no code time, only bench time. A reflash restarts the clock.

**H1, the combined hands sitting for Lucas (badge on USB), grouped by setup:**
1. **Keys at the desk:** physical Esc+Bksp opens #66 and Esc cancels it (#391); 7 About taps (#374); #355 3b; #351 secret field; #75 saver + holds 9-11; #273 Enter confirm + cross-task dialog; #286 Esc-hold 3 s + Clear-all; #274 Notes delete; #264 crash-log delete (unless `fw ui answer` allows it); #335 slider + key after a C6 reset; #205 key-to-flush; #117 the reversible write; #265 keys during the OTA (do this group while OTA-2 runs).
2. **Eyes on the panel:** #254 rows (cold boot on battery, C6 reset); #39 steps 2-3; #236 cold boot logo; #164 three steps; #155 panel; #171 dot colours; #128 18-19 HOT; #161 7.5 dB verdict.
3. **Phone:** #153 scans (dummy Wi-Fi only); #111 mention; #159 analyser comparison; #126 step 5 optional.
4. **Wi-Fi-cutting (USB console):** #108 7-14; #160 second network, Forget, open twin.
5. **Power and the Heltec off:** #292 part 1+2; #281 power-off; #267/#269 deep-sleep wake; #135 13; #136 13; #271 DM to a switched-off Heltec.
6. **Last:** #66 Settings reset path and boot-combo confirm. Then restore Wi-Fi and run `fw dev on` over USB (a factory reset clears `dev.mode`), then the soak starts.

**H2 (later, own sitting):** solder and probe: #68 J3 + R49, then #372; the #11 breakout wiring; #388 meter baseline. **H3:** the #116 burn. **H4:** #322 test card, #387 QR, #67 speaker, #73 phone.

## 6. Round 1 in detail (no decisions, no bench results needed)
| # | Issue | Branch | Model | Files owned | Conflict files |
|---|---|---|---|---|---|
| 1 | #389 | `fix/389-delivery-state-labels` | Sonnet | mesh_chat.c, meshcore_chat.c, host_tests/mesh_chat_test.c | standard-baseline. Stop if the journal format needs a field. |
| 2 | #368 | `fix/368-journal-after-sd-format` | Sonnet (Opus reviewer: journal integrity) | mesh_journal.c, new pure mesh_journal_gen.c/.h + test | mesh/CMakeLists.txt, host_tests/CMakeLists.txt |
| 3 | P1 (file issue first, epic #393) | `refactor/<N>-settings-display-split` | Sonnet | app_settings.c, new settings_display.c, shell_parts.h | lichen_shell/CMakeLists.txt |
| 4 | #211 | `refactor/211-ota-parsers` | Opus | ota_proto.c/.h, ota_server.c, ota_pieces.c/.h, ota_proto_test.c, ota_pieces_test.c, new host_tests/mutate.h | host_tests/CMakeLists.txt, standard-baseline (lines 28, 29, 34 go) |
| 5 | #208 | `refactor/208-split-mesh-task` | Opus | mesh_task.cpp, new mesh_cmd_*.cpp | mesh/CMakeLists.txt, layers.conf, cppcheck-baseline, standard-baseline |

Merge order: #389 → #368 → P1 → #211 → #208. Regenerate the baseline per merge, union-merge the CMake lists, one reviewer each (Opus-adjacent focus for #208 concurrency and #211 bounds). None of the five touches the badge, the soak or an open decision. #211 keeps the status codes for valid clients byte-identical, and refuses duplicate headers as specified. Left out on purpose: #298 (same files as #211), W1-B (same files as #389/#368), #288/#289 (decisions), #380 (after S1), #268/#384 (need S1 numbers).

## 7. Decisions for Lucas (ordered by work blocked)
1. **OTA-1 now, plus key/test ok for the new image.** Rec: yes. Unblocks S1, so all remote proofing.
2. **Soak slot ≥36 h after H1, with an hourly DMA gate.** Rec: yes. Closes #256 #260 #261 #269 #121; gates #268, #234, #237, #67 and #73 budgets.
3. **H1 date (on OTA-2).** Closes about 40 needs-lucas items and #237, and gives the #254 verdict.
4. **#288 form and migration.** Rec: a key prefix in `lichen_app_t` and the one-time `pw*` move (the 11-10 decision beats the 11-07 one). Unblocks #288 → #229 → the whole #216 chain, #233, #387, #116.
5. **#289 switch.** Rec: a setting `fw onair`, default off; USB-only to switch off or edit; applies to every console origin, LOCAL included. Unblocks #289 and E392-W3.
6. **#380 `?` rows, HTTP routes in release, the update-server default, the two physical-only dialogs.** Unblocks #380's `?` part and E381-W3.
7. **#379 retarget to a negative proof.** Rec: yes, no code. Closes #379.
8. **#387 look, limits and storage.** Rec: secrets class, never in the backup; the tag sits on top of the lock. Unblocks E393-W3, then #386.
9. **#386 gesture and toggles.** Unblocks E393-W4.
10. **#254 close + #322 release, and the #322 fallback.** Rec: decide after the H1 rows.
11. **#163:** factory reset clears the scope (rec: yes), plus the test region off-issue; **12.** #162 Heltec telemetry share; #168 polling off (rec: off).
13. **#235 tail:** file a small issue (rec: yes).
14. **#212 `lora_decode`:** defer to the next C6 change (rec).
15. **#328:** close as measured (rec). **16.** #78 probe build-only after W3 (rec), or drop.
17. **#388 profile design** after the meter numbers; **18.** #67 speaker fitted?; #73 phone + passkey fallback.
19. **#39:** split the P4-bootloader WARN into its own issue (rec); **#370:** accept the CRC rate if within noise (rec).
20. **#116 burn date** (irreversible, H3), and keep the burn command out of post-burn images (rec).
21. **Release build in the check:** measure, then put it in pre-push only (rec).
22. **SDL2 on the dev host for #382.** Rec: later.
23. Minor: #268 proven by bench high-water (rec); #261 1024 trial skipped (rec); #155 offset; `notify clear` and SD unmount allowed remotely; solder dates (#68, #12 stays parked).

## 8. Issues to file
- #52 latitude parse error shows off screen (#392).
- Status-bar unread "99+" overlaps the clock (#393).
- Split Settings > Display out of app_settings.c, the P1 prep (#393).
- An injected or remote ✕ key starts a stopped MeshCore (#381, reviewed by #395).
- Release build not in the check; #396 slipped through (#381).
- Narrow the remaining owner-only `nvs_open*` (#235 tail, #216), if decision 13 is yes.
- Docs: the DMA gate (bench skill: baseline −10 %) vs `architecture.md`/`shell-design.md` "150 KB free" (#216/#262).
- P4 bootloader WARN, USB session (#394), if decision 19 is yes.
- Conditional: a journal heard-time field, if #389 needs a format change (Opus, #392).
- Housekeeping, not issues: comment the drifted list on #212; post the #379 retarget; remove the stale needs-lucas on #182; add needs-proofing to #391.

## 9. Risks and what to cut or park
- **One badge.** The soak and H1 are single points; a dead Heltec stalls about 15 #392 items. Batch by peer state and keep S1's order.
- **#208 refactor of the mesh task** (3435 lines, concurrency): Opus plus a transcript-diff proof. If review drags, merge the other four and hold #208. Only #229 waits on it.
- **#211/#298 touch the update path.** A broken parser blocks OTA (USB rescue stays). The full-image fallback stays; `ota-reboot` ×10 after #298.
- **#380 edits about 30 files** owned by others. Run it after #268 and W1-C, in a quiet window, and add `fw dev status` to every bench brief.
- **NVS migrations:** #288, #235-tail and #116 each get their own flash; #232 never shares a flash with #116. After the burn, every slot rebases before flashing.
- **#229 touches every app descriptor:** schedule it when no app-file branch is open.
- **#388 light sleep vs DSI/SDIO/PSRAM:** last, main only.
- **Cut or park:** #371 (Lucas: not now), #328, delta OTA (until #298's numbers), #12, #73 W6b and #172 4c (until their spikes pass), #382 (until SDL2), #78 (probe only), the #261 1024 trial, 200 MHz PSRAM, #212 `lora_decode`, and the #11 mods 2-5.
