# Orchestration record: seven-epic program

Recovery entry point. The repository, GitHub and verified check output are the truth; this file is a pointer and a dashboard. Never trust a RUNNING state recorded here without checking `agent-wt list` and `ListAgents`.

## Objective and scope
Plan, implement, test, review, bench and deliver seven epics through dependency-aware waves:
#216 platform and architecture, #262 memory and performance, #381 bench tooling, #392 MeshCore, #393 UI and apps, #394 hardware and power, #395 security.

## Phase
- **Current (2026-11-13, late evening):** wave A CLOSED. Wave B merged, pushed, flashed (fw-f537a44a), its bench RUNNING. Wave C code RUNNING.
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
| A | #389 (sent label), #368, #397, #211, #208, #11 (wave 1), #212 (5 functions), #298 | fw-0746e922 | CLOSED. Bench passed (29 steps, 10 reboots). #208, #397, #376, #378 closed. Open ends: #368 (the SD format step is destructive, not run), #11 (hands, breakout), #398 (hands), an unexplained transmit after a MeshCore start (existing behaviour, tool to identify it: #408) |
| B | #404, #389 (heard time), #400 | fw-f537a44a | Flashed (first compressed update: 1.74 MB on the wire for a 2.75 MB image, 24 s). Bench RUNNING (agent bench-wave-B, script `bench-wave-B.md`) |
| C | #406 (wt-1, Sonnet), #408 (wt-2, Opus), #410 (wt-4, Opus); then #409 `fw soak`, #405 per-row remote keys, #403, the mesh hygiene part of #209/#212/#213 | - | Code RUNNING |

Every merge so far had an independent review and a re-review of its fix commit. Reviews caught, before merge: a journal wipe on a card read error (#368), a fail-open race and a regression in the hand-only start (#400), an uncapped decompression size (#298), surviving mutants in the config limits test (#212).

## Slots
- Free: wt-5. wt-3: #322 `feat/322-refresh-50hz`, held until the #254 verdict. wt-6: #398 `fix/398-latitude-error-visible`, parked until the owner's hands check says whether the bug still exists.

## Bench facts
- The badge ran fw-664dea27 before this program. Developer mode is on (switch it back on over the USB console after any `fw dev off`). The home order is arranged: never confirm Settings > Display > Reset order.
- A remote Enter on the Advert & contacts page is refused by the firmware by design (#405 proposes a per-row rule). The page shows the owner's saved position: never copy it anywhere.
- The helper `fw.sh` in the scratchpad sends console lines and redacts addresses; badge.sh needs the address from the scratchpad file, never printed.
- DMA memory on the current images: free about 156 KB, minimum about 146-148 KB (gate: 89981 / 80650).

## Soak (after the last wave)
#407: 36 h with the verdict at 24 h, PSRAM at 120 MHz, a supervisor script on the bench host owns the run and wakes a headless Claude run only for a blocker, updates through one guarded wrapper, a diff guard on loop fixes, restorable destructive steps allowed with a verified backup and a passed restore drill. Prerequisites to build: #409 `fw soak`, #410 `fw mem window`, #408 transmit log, a Wi-Fi disconnect counter, the Wi-Fi rollback rule (owner decision), and the tools listed on #407.

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
The full list of 23 decisions with recommendations is `orchestration/plans/consolidated.md` section 7. The ones that block the most:
1. Go-ahead for OTA-1 (unblocks all remote proofing).
2. A soak slot of 36 h or more after H1, with an hourly DMA gate.
3. A date for the hands sitting H1 (about 40 hands-only items).
4. #288 form: key prefix plus a one-time `pw*` migration.
5. #289 switch: a setting, default off, USB-only to change.

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
