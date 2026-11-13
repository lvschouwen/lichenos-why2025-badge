# Orchestration record: seven-epic program

Recovery entry point. The repository, GitHub and verified check output are the truth; this file is a pointer and a dashboard. Never trust a RUNNING state recorded here without checking `agent-wt list` and `ListAgents`.

## Objective and scope
Plan, implement, test, review, bench and deliver seven epics through dependency-aware waves:
#216 platform and architecture, #262 memory and performance, #381 bench tooling, #392 MeshCore, #393 UI and apps, #394 hardware and power, #395 security.

## Phase
- **Current (2026-11-13, night):** waves A and B CLOSED. Wave C flashed (fw-f742e4ce), its bench RUNNING. Wave D: #288 reviewed and READY (not merged), #411, #409, #412 in progress.
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
| C | #406, #410 `fw mem window`, #408 transmit air log, #405 per-row remote keys; plus the #368 SD-format proof and #398 | fw-f742e4ce | Flashed. Bench RUNNING (agent `bench-wave-C`, script `bench-wave-C.md` in the scratchpad). #406 closed |
| D | #288 app secrets + one-time password move; #411 Wi-Fi rollback rule; #409 `fw soak`; #412 status poll timing | - | see "Wave D state" |

### Wave D state (resume here)
| Issue | Slot / branch | State |
|---|---|---|
| #288 | wt-5 `feat/288-app-secrets` (3 commits, tip f5952023) | Reviewed twice (Opus), verdict merge as is. NOT merged on purpose: it migrates stored passwords, so it rides in wave D with no other settings migration, and never with the #116 encryption flash. Merge it first in wave D |
| #411 | wt-4 `feat/411-ota-valid-needs-wifi` (tip bb676556 + a fix commit in progress) | Review found 2 mediums (Wi-Fi-off must read the saved switch; apply the rule to every trial image unless `fw slot other` left a mark). Fix commit was requested from `impl-411`; then a delta re-review, then merge |
| #409 | wt-2 `feat/409-fw-soak-line` | `impl-409` implementing; needs an Opus review (boot counter must increase across every kind of reset) |
| #412 | wt-1 `fix/412-status-poll-after-start` | `impl-412` implementing; needs an Opus review (it may only move a transmit later, never sooner or more often) |

A slot with a claimed branch and no live agent holds finished or interrupted work: read its `git log` and its check log in the scratchpad (`check-wt-N-<issue>*.log`) before doing anything; never reset it blind. Agents resume by name with SendMessage if the session is the same; in a new session start a fresh `lichen-implementer` on the same slot and branch with the issue and the review findings as the brief.

After wave D, in dependency order: #289 on-air allow list (decided: a setting, USB-only), #229 capabilities, #232, #231 + #349, #233, #385; in parallel as slots allow: #403 OTA parser follow-ups (ota_server.c is at 1471 of 1500 nloc: split first), mesh hygiene (#209/#212/#213 remainder), #384 + #268 (need bench numbers), #380, the soak tooling on #407.

Every merge had an independent review and a re-review of its fix commit. Reviews caught before merge: a journal wipe on a card read error (#368), a fail-open race and a regression in the hand-only start (#400), an uncapped decompression size (#298), two holes in the loosened advert-page rule (#405), a false "nothing transmitted" in the transmit log (#408), a migration record that stranded plaintext after a rollback (#288), two ways to keep a bad image (#411).

## Slots
- Free: wt-6 holds the parked #398 branch (drop it if the wave C bench shows the note visible). wt-3: #322 `feat/322-refresh-50hz`, held until the #254 verdict.
- Claimed: wt-1 (#412), wt-2 (#409), wt-4 (#411), wt-5 (#288).

## Bench facts
- The badge ran fw-664dea27 before this program. Developer mode is on (switch it back on over the USB console after any `fw dev off`). The home order is arranged: never confirm Settings > Display > Reset order.
- The MeshCore Advert & contacts page shows the owner's saved position: never copy it anywhere. Remote keys there: open the two position fields, type, cancel, submit an invalid value; nothing else (#405).
- The helper `fw.sh` in the scratchpad sends console lines and redacts addresses; badge.sh needs the address from a scratchpad file, never printed. In a new session the scratchpad is gone: get the address with `fw wifi` over the USB console (see the bench recipes in project memory), write it to a 0600 file, never echo it.
- DMA memory on these images: free about 156 KB, minimum about 146-149 KB (gate: 89981 / 80650).
- Status-history polling (#168) is ON, every 10 min, to the agreed repeater: the badge transmits a request after every MeshCore start and every 10 min. The owner wants it OFF; the on-air guard refuses `mesh history <id> off` from an agent because that repeater is not in the allow file. The owner switches it off himself.
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
1. Switch the status-history polling off (see Bench facts).
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
