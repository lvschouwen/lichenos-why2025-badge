# Orchestration record: seven-epic program

Recovery entry point. The repository, GitHub and verified check output are the truth; this file is a pointer and a dashboard. Never trust a RUNNING state recorded here without checking `agent-wt list` and `ListAgents`.

## Objective and scope
Plan, implement, test, review, bench and deliver seven epics through dependency-aware waves:
#216 platform and architecture, #262 memory and performance, #381 bench tooling, #392 MeshCore, #393 UI and apps, #394 hardware and power, #395 security.

## Phase
- **Course correction (owner, 2026-11-14, after a review against the original plan; recorded on #407 and #420):** (1) a plain 36 h passive BASELINE soak runs right after wave H with the tooling that exists (stop and notify on a blocker, no self-fix); the final soak stays for the end, and its autonomy tooling is built only if the baseline shows a need. (2) The allow list stays OFF on the bench badge until the program ends (the owner switches it off once at USB; the firmware rule is unchanged). (3) One hands sitting after the wave H flash, before #233 and #385 start: main prepares one grouped checklist of the 15-20 checks later work builds on. (4) Soak tooling stops after #430 (census, trend test); the rest is decided from the baseline's numbers. Also decided: #421 gates ALL listed test commands (in wt-4); the owner allows the one `fw sd format ERASE` for #368 in the wave H bench; wt-6's unmerged branch may go, but the permission check refused the deletion: the owner runs it. Filed: #430 (wt-2), #431-#436 (the six refactors left out of #231). Open #231 review notes to add to #431/#432: a revive before a queued timer drop; a restart behind an update waits 8 s per busy app.
- **Earlier (2026-11-14, late evening): PAUSED at the owner's request; resumed the same night.** Waves A-E CLOSED; F and G benched remotely; wave H half merged. The badge runs fw-03656b31 (wave G plus the #425 measurement commit). The allow list is ON with the test channel as its only entry: Wi-Fi updates, C6 pushes and `fw slot other` are refused until the owner switches it off at USB; a USB flash always works. Resume at "Wave H state".
- Planning finished 2026-11-13. Plans: `orchestration/plans/` (`consolidated.md` is the program plan).

## Process (owner, 2026-11-13)
- A wave = a batch of reviewed merges. Per wave: merge one branch at a time (rebase, ff-only, full check), push, relabel build, ONE flash, then a brief on-badge bench that an agent runs from a script main writes. A wave cannot close until that bench is done.
- No soak per wave. One soak when all waves are done: plan, review and decisions in #407.
- The flash itself is done by main. Ask the owner for a go when the permission check refuses it.
- Bench scripts live in the session scratchpad (`bench-wave-<X>.md`); they are remote only, list every allowed command, and allow on-air sends only to the test channel (an advert is also allowed by the owner).
- Rebase conflicts in `tools/standard-baseline.txt`: record the branch's non-size changes first, take main's file, re-apply those changes, let the check rewrite the size lines. `host_tests/CMakeLists.txt` and component source lists: union-merge.

## Epic order
- Original: #216, #262, #381, #392, #393, #394, #395.
- Approved (Decision Gate A, 2026-11-13): **#381, #395, #216, #392, #262, #393, #394**. It is a priority order when waves compete for the badge or for slots; waves interleave.
- Why: #381's tooling makes the other epics' bench checks remote; #395's #391 proof, #211 and #288 are built on by later work; #262's code needs bench numbers first; #394 hangs on #226, #229, #234 and the soak.

## Critical path
OTA-1 -> remote sweep S1 -> #208 + #211 merged -> OTA-2 + hands sitting H1 -> soak (36 h or more) -> #288 (own flash) -> #229 -> #232 -> #231 + #349 -> #233 -> #385. The #116 burn and #388 light sleep trail it. The badge and Lucas's sittings are the long poles, not agent time: about 85 of about 115 open issues are bench debt.

## Conventions this program follows
- Check: `agent-check` (full `firmware/build.sh build`), green before every merge and push. There is no CI; the check and the pre-push gate are the gate.
- Worktrees: warm pool `.claude/worktrees/wt-1..6` via `agent-wt`; branches `<type>/<issue>-<slug>`.
- Agents are build-only: green check plus a local commit. Main merges (rebase, ff-only, check, push), flashes, benches and closes issues.
- Delivery of a wave = merged on main, check green, pushed, bench evidence on the issue (or `needs-lucas` with a bench checklist), issue closed or labelled.
- Bench: `lichen-bench` skill, main session only. OTAs batched; ask Lucas before any OTA or key operation; on-air only #badgetest with the Heltec.
- Security: the 13-rule checklist in `orchestration/plans/epic-395.md` section 6 goes into every brief and review from the first wave.

## Deviations from the program brief (recorded on purpose)
- Phases A and B ran as one pass per epic (seven Opus planners) plus one Opus consolidation: eight agents, not fifteen.
- No wave-coordinator agents: spawn depth is 1, so main launches and tracks every worker.
- Reviewers: `lichen-reviewer` on Sonnet by default, Opus where the change touches security, memory, concurrency or protocols.

## Agents and models
| Role | Model |
|---|---|
| Lead orchestrator (main) | Fable 5.2, low effort |
| Planners, consolidation | Opus (finished, stopped) |
| `lichen-implementer` | Sonnet; Opus on the CLAUDE.md Opus triggers |
| `lichen-reviewer` | Sonnet; Opus for security, memory, concurrency, protocol |

Environment: default subagent model Sonnet, at most 6 concurrent, spawn depth 1.

## Waves
| Wave | Issues | Image | State |
|---|---|---|---|
| A | #389 (sent label), #368, #397, #211, #208, #11 (wave 1), #212 (5 functions), #298 | fw-0746e922 | CLOSED. #208, #397, #376, #378, #379 closed. Open ends: #11 (hands, breakout), #298 (rest of its bench list) |
| B | #404, #389 (heard time), #400 | fw-f537a44a | CLOSED. #389, #404 closed; #400 has one hands check left. Found #412 |
| C | #406, #410, #408, #405, #398 | fw-f742e4ce | CLOSED. All five closed. The #368 SD-format proof was NOT run: the permission check refused `fw sd format ERASE` from the bench agent; the owner runs it or allows it (#368, `needs-lucas`) |
| H | #426, #423, #231, #421 (+ #430 tooling) | fw-e93ddf36 | Flashed over Wi-Fi (list off) and benched remotely 2026-11-14: all steps passed. #368 and #430 closed. Open ends: #231 (the update stop, seen at the next flash), #423 (`fw help` is cut over Wi-Fi after 37 lines: read it over USB), #421 (developer-mode-off sweep: main over USB), #420 (pending-update refusal: owner), #425 (`mslow=0` over hours: baseline soak), #426 (owner-only reset). Scripts: `orchestration/bench/bench-wave-H.md`, `bench-wave-H2.md` (the remote part of the hands sitting), `hands-sitting-H.md` |
| G | #421, #424, #349, #425 | fw-1570ef58 | Benched remotely. #424, #349 closed. #421 open (`needs-lucas`: the developer-mode-off check; owner decision on the sweep). #425 open: file time is off the lock, but the loop still takes 507 ms under it and the write after the unlock takes 1.4 s. Filed: #426 (companion factory reset keeps contacts), #427 (callback crash loop, mark tied to context) |
| F | #417, #419, #229, #420 | fw-575798b9 | Benched remotely. #417, #419 closed. #229 open for hand checks (`needs-lucas`). #420 open for its list-off steps. Follow-ups filed: #421 (test commands outside developer mode), #349 (callbacks run as their app; #422 was a duplicate), #423 (per-app console prefix) |
| E | #416, #403, #289, #232 | fw-73532714 | CLOSED. #416, #403, #232 closed. #289 open (`needs-lucas`): four hand checks listed on the issue. Script: `orchestration/bench/bench-wave-E.md` |
| D | #288, #411, #412, #409 | fw-869e608d | CLOSED. #409, #411, #412 closed. #288 open for one check: the repeater login with the saved password (on air, `needs-lucas`). Found #416. Not seen: the #411 rollback banner (same version in both slots). Script: `orchestration/bench/bench-wave-D.md` |

### After wave H (resume here)
1. **Running when this was written:** a bench agent on `bench-wave-H2.md` (the owner allowed the remote part of the hands sitting: #273, #377 allowed half, #274, #229 Notes and Timer, #264, #286 Clear all; the Reset order dialog is only ever cancelled). An implementer in wt-1 on #438 (`fw mesh chat open <#channel>`, developer mode; needs a review, a merge and the next flash, then #349's chat leak check runs remotely).
2. **Main, after H2:** the #421 developer-mode-off sweep over the USB console (`fw dev off`, the ten refusals, the status forms, a reboot, `fw dev on`); read `fw help` over USB for #423; #271 with the Heltec peer session (a post on the test channel, screenshots of the notice, tile and chat; the owner allowed it).
3. **Then the owner's sitting** (`hands-sitting-H.md`, shortened by what ran remotely: strike those steps and post the rest as one checklist). #233 and #385 start only after it.
4. **Then the baseline soak** (#407, third round of decisions): 36 h passive on the image of that moment, existing tooling, stop and notify. The sampler is still to be written as a plain 10-minute read of `fw soak`, `fw mem`, `fw tasks` into a CSV (scratch tool, no new firmware).
5. **Facts from the wave H bench:** apps boot step 853 / 828 ms (1.5 s on record: confirm over several boots); DMA free about 151.5 KB, min 143-146 KB; `fw help` over Wi-Fi is cut after 37 lines; after an SD format the journal's "started a new one" line comes with its second flush (about 100 s); `slot other` reboots by itself; a release build is red on main for #437.
6. **Open issues filed this session:** #431-#436 (left out of #231), #437 (release check), #438. The allow list is OFF on the bench badge until the program ends.
7. Queue after the sitting, in dependency order: #233, #385. In parallel: #427, mesh hygiene (#209/#212/#213 remainder), #384 + #268 (after the baseline's numbers), #380, #437. After the final soak: #418.

### Wave G state (resume here)
1. #425 round two: an Opus implementer in wt-1 (`fix/425-save-build-under-lock`) is finding the 507 ms spent under the mesh lock while a contacts save is built (no file call). Bench line to compare against: `contacts saved after the unlock: 553 contacts, 84056 B, 4096 B rewritten, 1376 ms` and `MeshCore loop took 507 ms: files 0 ms (0 calls)`. Whether the 1.4 s write should become cheap (another file layout) is a design question, not yet an issue.
2. Soak tooling: `tools/soak_report.py expected <csv>` and `tools/soak_budgets.txt` exist (#424); `python3 -I -c "import sys; sys.path.insert(0,'tools'); import soak_parse; ..."` turns a console transcript into the CSV. On fw-1570ef58 the report flags `mesh_loop_slow_ms` (#425) and `notice_slow_ms` (the 3 s storage count, unplanned). Still to build (#407): the sampler, per-app RAM in `fw soak`, the probes (first frame, update time, SD speed), the log census, the trend test.
3. #349 facts: apps register LVGL callbacks only through `lichen_ui_event_add`, `lichen_ui_timer_create`, `lichen_ui_group_focus_cb` (raw calls are forbidden for L6 in `tools/layers.conf`); `CONFIG_LV_USE_EXT_DATA=y` is in `firmware/sdkconfig.defaults` (measured cost 0.4-2.7 KB of PSRAM per screen). A crash inside a screen callback is named but counts no strike. Before/after figures: `wg-before-349.txt` / `wg-after-349.txt` values are on the #349 bench comment.
4. Remote-bench limit met twice: opening a MeshCore chat needs Enter next to real conversations, so chat-screen measurements (leak check, busy-screen PSRAM) wait for a hands sitting or a test-only chat.
5. Next, in dependency order: #231 (app lifecycle; #349 is in), #233, #385. In parallel: #427, #426, #423, the next soak tooling pieces, mesh hygiene (#209/#212/#213 remainder), #384 + #268, #380. After the soak: #418.

### Wave F state (resume here)
1. To finish #420: the owner runs the USB helper with `off`; then main proves over Wi-Fi: an update passes (this also settles #417's "normal update" step), `fw slot other` there and back, `ota-c6`; an update with no reboot followed by the owner's `fw onair on` attempt is refused as pending; after the reboot and a valid slot the owner's helper `sync` switches it on again.
2. Wave F facts: rule D (#229) fails the build when a component calls a new gate or console function that `tools/layers.conf` does not classify: after merging branches that add such functions, expect to add a `cap-func` line and run `tools/check_layers.sh --gen-manifest` (it happened with #420 on top of #229). `tools/gate_manifest.txt` is generated: on a rebase conflict take the branch's side and regenerate at each commit. With the list on, `badge.sh ota-c6` prints a misleading "mixed bundle" warning for the refusal.
3. Expected against measured (owner rule, #407): every issue states expected resources; #420 measured 2.3 KB of flash against an expected 1 KB (scope added by the reviews; recorded on the issue). Per-app budgets from #229 cover statics and stacks only; the heap part is still to be measured.
4. Next, in dependency order: #231 + #349, #233 (revisits the MeshCore exemption and what "background" means), #385. In parallel as slots allow: #421, the soak tooling on #407 (#424 first), #423, mesh hygiene (#209/#212/#213 remainder), #384 + #268, #380. After the soak: #418.

### After wave E (resume here)
Bench facts from wave E: a test crash at 120 MHz costs two boots and about 2 min; `degraded boot yes` stays for the 3-boot window after a crash (by the rule); the badge's own allow list now refuses console sends to anything but the test channel, and only the USB console can edit it. Agents and main cannot send `onair` edit lines or read the host allow file (hooks): the owner runs a USB helper himself (a script in the session scratchpad, modelled as `owner-onair.sh sync|show|off|line`; recreate it when needed). The host allow file lists contact NAMES; the badge's list takes key digits, so a test-node entry needs its key from the owner.

Next, in dependency order: #420 (refuse Wi-Fi updates and `fw slot other` while the list is on; owner decision), #229 capabilities (touches every app descriptor: no app-file branch open at the same time; `lichen_app_t` gained `safe_mode_exempt` in #232), then #231 + #349, #233 (also revisits the MeshCore exemption), #385. In parallel as slots allow: #419 (`fw soak` fourth line) and the soak tooling on #407 (scope widened: expected against measured), #417, mesh hygiene (#209/#212/#213 remainder), #384 + #268 (need bench numbers), #380. After the soak: #418.

### After wave D (resume here)
Wave D bench facts: the #288 move line appeared once (1 moved, 0 not moved); the factory-reset preview hides 2 app secrets (1 more than moved: look once at what the second is). A crash at 120 MHz is followed within 20 s by the `psram 120 re-arm after crash` restart, so a sampler sees `boot` +2, `rst=sw rstcrash=0`, `crash` +1 (#416: decide the rule before the soak tooling is built). Old issues with fresh evidence comments: #205, #261, #263, #256, #210, #236 (About version row differs from its checklist), #52, #50, #133. Not run: #127 (needs Enter on the Contacts tile), most of #136/#138.

Earlier queue (kept for the record; superseded by "After wave E"): #229 capabilities, #231 + #349, #233, #385; in parallel as slots allow: #403 OTA parser follow-ups (ota_server.c is at 1471 of 1500 nloc: split first), mesh hygiene (#209/#212/#213 remainder), #384 + #268 (need bench numbers), #380, the soak tooling on #407.

Every merge had an independent review and a re-review of its fix commit. Reviews caught before merge: a journal wipe on a card read error (#368), a fail-open race and a regression in the hand-only start (#400), an uncapped decompression size (#298), two holes in the loosened advert-page rule (#405), a false "nothing transmitted" in the transmit log (#408), a migration record that stranded plaintext after a rollback (#288), two ways to keep a bad image (#411).

## Slots
- Free: wt-2, wt-4, wt-5, wt-6. wt-1: #438 `feat/438-mesh-chat-open`.
- wt-3: #322 `feat/322-refresh-50hz`, held until the #254 verdict.

## Tools kept in the repo
- `orchestration/tools/merge-one.sh <slot> <branch> <tag>`: merges ONE reviewed branch from the main checkout: records the branch's non-size baseline removals, rebases (main's size lines win, source and test lists are union-merged by `union.py`), re-applies the removals, fast-forwards, runs the full check and commits the new size lines. It stops on any other conflict (resolve that one by hand in the slot, finish the rebase, then run the script again), on a branch that adds baseline entries, and on a check that is not green. Set `SOAK_SCRATCH` to a scratch directory for its logs. After the last merge run `agent-check` once more for the stamp, then push in its own command.
- `orchestration/bench/bench-wave-{A,B,C}.md`: the bench scripts that were run, as models. Their "Tools" section refers to a session scratchpad helper (`fw.sh`) that sends one console line over Wi-Fi and redacts addresses; recreate it in the new session's scratchpad (it is a ten-line wrapper around `tools/badge.sh fw`).

## Bench facts
- The badge ran fw-664dea27 before this program. Developer mode is on (switch it back on over the USB console after any `fw dev off`). The home order is arranged: never confirm Settings > Display > Reset order.
- The MeshCore Advert & contacts page shows the owner's saved position: never copy it anywhere. Remote keys there: open the two position fields, type, cancel, submit an invalid value; nothing else (#405).
- The helper `fw.sh` in the scratchpad sends console lines and redacts addresses; badge.sh needs the address from a scratchpad file, never printed. In a new session the scratchpad is gone: get the address with `fw wifi` over the USB console (see the bench recipes in project memory), write it to a 0600 file, never echo it.
- DMA memory on these images: free about 156 KB, minimum about 146-149 KB (gate: 89981 / 80650).
- Status-history polling (#168) is OFF since 2026-11-13 (the owner switched it off on the badge: `fw mesh history` reads `off`, 0 of 3 slots on). The badge no longer transmits by itself after a MeshCore start, so a bench may assert an empty transmit log after a start. The on-air guard refuses any `mesh history <target>` line from badge.sh or an agent unless the target is in the allow file, also for `off`. The #412 bench step needs the polling on: leave that step for a time the owner switches it on, or prove #412 by its host tests only.
- Updates are compressed since wave B: about 1.75 MB on the wire for a 2.76 MB image, 24-28 s.
- The on-air guard allows sends to the test channel and the two test nodes; the owner also allows a one-off advert.

## Soak (after the last wave)
#407: 36 h, verdict at 24 h, a blocker in hours 24-36 still fails it; PSRAM at 120 MHz; a supervisor script on the bench host owns the run and wakes a headless Claude run only for a blocker; updates through one guarded wrapper; a diff guard on loop fixes; wipe, settings reset and SD format allowed with a verified backup and a passed restore drill (losing the SD message history is acceptable). Firmware prerequisites: #410 done, #408 done, #409 and #411 in wave D, a Wi-Fi disconnect counter (inside #409 if the Wi-Fi code already counts). Tooling still to build: the list on #407.

## Epic dashboard
| Epic | Open issues | Waves | Done | Active | Blocked on |
|---|---|---|---|---|---|
| #381 | 11 (+#400, #401) | 7 | 0 | W0 (bench) | OTA-1 |
| #395 | 5 | 3 | 0 | W1a #211 | OTA-1, decisions 4-5 |
| #216 | 36 | 6 | 0 | W1-A #208 | OTA-1, H1 |
| #392 | 19 (+#398) | 5 | 0 | W1 #389, #368 | OTA-1 |
| #262 | 16 (+#402) | 4 | 0 | - | OTA-1, soak |
| #393 | 21 (+#397, #399) | 5 + P1 | 0 | P1 queued | OTA-1, decisions 8-9 |
| #394 | 13 | 9 | 0 | - | #226, #229, #234, soak, solder |

## Issues filed by this program
#397 Display split (P1), #398 latitude error off screen, #399 "99+" overlaps the clock, #400 injected shape key starts MeshCore, #401 release build not in the check, #402 DMA gate docs.

## Waiting on Lucas
Decisions are asked at once as multiple choice with a recommendation (project memory `ask-decisions`); none is open right now. Open actions on his side:
1. The SD format for the #368 proof: run `fw sd format ERASE` himself or allow that one command.
2. One hands sitting, any time: #374 (7 Enters on About; the off dialog), #391 (physical Esc+Backspace at boot), #377 (confirm attempts on the reset dialog, with him watching), #400 (physical Messages key starts a stopped MeshCore), #11 (wire the MAX17048 breakout on J7, then the readings check).
3. For the soak: one permission rule for the update wrapper once it exists, and a notification channel.
Decided on 2026-11-13 and recorded on the issues: #288 (prefix + one-time move), #289 (setting, USB-only), #405 (saving a position stays hands-only), #411 (roll back after 5 min without Wi-Fi), #379 (closed on the refusal), bench debt folded into wave benches (10-15 old issues per wave, steps compiled by a read-only prep agent first), soak length and rules (#407).

## Open housekeeping
- Post each epic's wave table as a comment on the epic once the plans are pushed.
- Retarget #379 to a negative proof (decision 7); remove the stale `needs-lucas` on #182; add `needs-proofing` to #391; comment the drifted function list on #212.
- Read `fw` before OTA-1: the plans disagree on which image the badge runs.

## Recovery
1. Read this file, then `orchestration/plans/consolidated.md`.
2. `git status -sb`, `git log --oneline origin/main..main`, `agent-wt list`.
3. `gh issue list --label needs-lucas`, `gh issue list --label now`.
4. `ListAgents` for agents that are really still running; a slot with a claimed branch and no live agent holds interrupted work: read its `git log` and check log before reassigning, never reset it blind.
5. Rebuild the READY queue from the wave table in `consolidated.md` section 4 and resume the next safe wave.
