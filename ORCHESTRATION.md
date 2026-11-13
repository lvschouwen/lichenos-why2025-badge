# Orchestration record: seven-epic program

Recovery entry point. The repository, GitHub and verified check output are the truth; this file is a pointer and a dashboard. Never trust a RUNNING state recorded here without checking `agent-wt list` and `ListAgents`.

## Objective and scope
Plan, implement, test, review, bench and deliver seven epics through dependency-aware waves:
#216 platform and architecture, #262 memory and performance, #381 bench tooling, #392 MeshCore, #393 UI and apps, #394 hardware and power, #395 security.

## Phase
- **Current:** Round 1 code RUNNING; bench sweep S1 waits on Lucas's go for OTA-1.
- Planning (phases A and B) finished 2026-11-13. Plans: `orchestration/plans/` (`consolidated.md` is the program plan: order, dependency graph, wave queue, bench schedule, decisions).

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

## Round 1 (code, started 2026-11-13)
Merge order: #389 -> #368 -> #397 -> #211 -> #208. One reviewer each before merge.

| Issue | Epic | Slot | Branch | Agent / model | State |
|---|---|---|---|---|---|
| #389 delivery-state labels | #392 | wt-2 | `fix/389-delivery-state-labels` | impl-389 / Sonnet | RUNNING |
| #368 journal after SD format | #392 | wt-4 | `fix/368-journal-after-sd-format` | impl-368 / Sonnet | RUNNING |
| #211 OTA parsers + `mutate.h` | #395 | wt-5 | `refactor/211-ota-parsers` | impl-211 / Opus | RUNNING |
| #208 split mesh_task.cpp | #216 | wt-6 | `refactor/208-split-mesh-task` | impl-208 / Opus | RUNNING |
| #397 Settings Display split (P1) | #393 | next free slot | `refactor/397-settings-display-split` | Sonnet | READY, waits for a slot |

## Slots
- wt-1: still claimed by #374 (merged and pushed, not benched). Release with `agent-wt done wt-1` after the #374 bench.
- wt-3: #322 `feat/322-refresh-50hz`, held until the #254 verdict.

## Bench queue (main only)
| Step | What | State |
|---|---|---|
| OTA-1 | current main to the badge (`ota-c6` first), then `fw dev on` over USB | NEEDS_LUCAS (go-ahead) |
| S1 | combined remote sweep of all epics, order in `consolidated.md` section 5 | BLOCKED on OTA-1 |
| OTA-2 + H1 | Round 1 image, re-proof, one hands sitting | PLANNED |
| Soak | 36 h or more after H1, hourly DMA gate | NEEDS_LUCAS (slot) |

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
