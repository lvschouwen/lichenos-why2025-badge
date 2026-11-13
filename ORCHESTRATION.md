# Orchestration record: seven-epic program

Recovery entry point. The repository, GitHub and verified check output are the truth; this file is a pointer and a dashboard. Never trust a RUNNING state recorded here without checking `agent-wt list` and `ListAgents`.

## Objective and scope
Plan, implement, test, review, bench and deliver seven epics through dependency-aware waves:
#216 platform and architecture, #262 memory and performance, #381 bench tooling, #392 MeshCore, #393 UI and apps, #394 hardware and power, #395 security.

## Phase
- **Current:** A+B (per-epic dependency analysis and wave planning, then architecture consolidation).
- Started 2026-11-13.

## Epic order
- Original: #216, #262, #381, #392, #393, #394, #395.
- Approved: pending Decision Gate A.

## Conventions this program follows
- Check: `agent-check` (full `firmware/build.sh build`), green before every merge and push. There is no CI; the check and the pre-push gate are the gate.
- Worktrees: warm pool `.claude/worktrees/wt-1..6` via `agent-wt`; branches `<type>/<issue>-<slug>`.
- Agents are build-only: green check plus a local commit. Main merges (ff-only), pushes, flashes, benches and closes issues.
- Delivery of a wave = merged on main, `agent-check` green, pushed, bench evidence on the issue (or `needs-lucas` with a bench checklist), issue closed or labelled.
- Bench: `lichen-bench` skill, main session only. OTA batches per wave; ask Lucas before OTA or key operations; on-air only #badgetest.
- Epics list their issues in the epic body (no GitHub sub-issues).

## Deviations from the program brief (recorded on purpose)
- Phases A and B run as one pass per epic (one planner per epic does dependency analysis and wave decomposition), followed by one architecture consolidation. Fifteen agents became eight.
- Planners write to the session scratchpad; main copies approved plans to `orchestration/plans/` and posts each wave table on its epic.

## Agents and models
| Role | Model | Notes |
|---|---|---|
| Lead orchestrator | Fable 5.2, low effort | main session |
| Epic planners (7) | Opus | read-only, one per epic |
| Architecture consolidation | Opus | read-only |
| Implementers (`lichen-implementer`) | Sonnet; Opus on the CLAUDE.md Opus triggers | one slot each |
| Reviewer (`lichen-reviewer`) | Sonnet; Opus for security, memory, concurrency, protocol | never the implementer |
| Lookups | Haiku | search fan-out only |

Environment: `CLAUDE_CODE_SUBAGENT_MODEL=sonnet` is the default, at most 6 concurrent subagents, spawn depth 1 (agents cannot spawn agents, so main launches every worker).

## Pre-existing work found at start (2026-11-13)
- `main` was 3 commits ahead of `origin/main` (#374 developer mode UI). Needs check, push, bench.
- `wt-1` claimed for #374 (`feat/374-dev-mode-ui`, merged). `wt-3` claimed for #322 (`feat/322-refresh-50hz`, 1 commit, 77 behind main, not merged).
- `wt-2`, `wt-4`, `wt-5`, `wt-6` free.

## Epic dashboard
| Epic | Waves | Done | Active | Blocked | Open defects | Bench | Delivery |
|---|---|---|---|---|---|---|---|
| #216 | tbd | 0 | - | - | - | - | - |
| #262 | tbd | 0 | - | - | - | - | - |
| #381 | tbd | 0 | - | - | - | - | - |
| #392 | tbd | 0 | - | - | - | - | - |
| #393 | tbd | 0 | - | - | - | - | - |
| #394 | tbd | 0 | - | - | - | - | - |
| #395 | tbd | 0 | - | - | - | - | - |

## Wave queue
Empty until planning lands. States: PLANNED, READY, RUNNING, INTEGRATING, TESTING, REVIEWING, FIXING, VALIDATING, DELIVERING, BLOCKED, NEEDS_LUCAS, COMPLETED.

## Waiting on Lucas
Nothing yet.

## Recovery
1. Read this file, then `orchestration/plans/` if present.
2. `git status -sb`, `git log --oneline origin/main..main`, `agent-wt list`.
3. `gh issue list --label needs-lucas`, `gh issue list --label now`.
4. `ListAgents` for agents that are really still running.
5. Rebuild the READY queue from the wave table and resume the next safe wave.
