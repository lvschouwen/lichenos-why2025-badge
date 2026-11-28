# Orchestration: standing notes

A pointer, not a log. Open work, decisions and evidence of finished work live on the GitHub issues
(`gh issue list`, `--label now`, `--label needs-lucas`). Never record done work here (#483). Write a line
here only for live state that exists nowhere else (a running soak or agent and its run dir), and remove it
when the run ends. The old wave logs are in git history (before #483).

## Process (owner, 2026-11-13)
- A wave = a batch of reviewed merges. Merge one branch at a time (rebase, ff-only, full check), push, relabel
  build, ONE flash, then a brief remote bench run from a script main writes. A wave closes only after its bench.
- Flash over Wi-Fi without asking (P4 and C6, C6 bundle first). Ask the owner only when the permission check refuses.
- Bench scripts: remote only, every allowed command listed, on-air sends only to `#badgetest`. Bench debt is
  folded into wave benches; a read-only prep agent compiles the steps from the issues first.
- When the owner is at the badge, announce each visible step and wait. Destructive steps and injected Enter
  presses need his word. Steps that would put a secret through main's context go into an owner-run `!` script
  (`~/.config/why2025-badge/owner-onair.sh`, `owner-token.sh`).
- Security checklist for briefs and reviews: `orchestration/plans/epic-395.md` section 6.

## Merging
- `orchestration/tools/merge-one.sh <slot> <branch> <tag>` merges one reviewed branch: records the branch's
  non-size baseline removals, rebases (main's size lines win; source and test lists union-merged by `union.py`),
  re-applies the removals, fast-forwards, runs the full check, commits the size lines. It stops on any other
  conflict: resolve it in the slot, `git rebase --continue`, run it again. `SOAK_SCRATCH` = a dir for its logs.
- `tools/standard-baseline.txt` conflicts: keep the branch's non-size changes, take main's file, let the check
  rewrite the size lines. `host_tests/CMakeLists.txt` and component source lists: union-merge.
- `tools/gate_manifest.txt` is generated: on a conflict take the branch's side and run
  `tools/check_layers.sh --gen-manifest`. A new gate or console function needs a `cap-func` line in `tools/layers.conf`.
- A branch whose own commits add then remove a source-list entry gets it back from the union merge (commit-by-commit rebase): the check fails with "Cannot find source file"; drop the line on main and re-run (#558).
- After a submodule pointer moves on main, a rebased slot shows `meshcore` dirty:
  `git -C <slot> submodule update meshcore`, then `agent-wt done`.
- After `cd` into a slot, prefix main's commands with `cd /home/lucas/coding/why2025-badge &&`.

## Guards
- `.claude/hooks/guard-badge.sh` judges a command whole and cut before every console verb. Text that names a
  gated console command next to a line that reaches the badge is refused, even inside an issue body or a commit
  message: write such text with Write and pass `--body-file`.
- A mesh send line to any channel but `#badgetest` in a Bash command is refused, also as issue text.
- With the badge's allow list ON, Wi-Fi updates, C6 pushes and `fw slot other` are refused (#420).

## Bench
- Helper: a scratchpad `fw.sh` (ten lines around `tools/badge.sh fw <ip>`, redacts addresses) and a 0600
  `badge-ip` file; a copy of the IP file is kept at `~/.config/why2025-badge/badge-ip`. Never print the address.
- Developer mode stays on. Never confirm Settings > Display > Reset order. Never copy the owner's saved position.
- DMA gate: free >= 89981, min >= 80650 (current images read about 145 KB free, 139 KB min).
- Status-history polling is off on the badge, so it does not transmit by itself after a MeshCore start.
- `fw storage` listings during a soak skew the timing figures: note them when you run one.
- Soak tooling: `tools/soak_driver.py`, `tools/soak_report.py` (trend, expected, census, air); config in
  `~/.config/why2025-badge/soak-driver.conf` (never print it). The census needs the serial log cut to the soak's boot.

## Recovery after a /clear
1. `git status -sb`, `git log --oneline origin/main..main`, `agent-wt list`, `ListAgents`.
2. `gh issue list --label now` and `--label needs-lucas`; the latest comment on an issue is its state.
3. A claimed slot with no live agent holds interrupted work: read its `git log` and check log before reassigning.

## Live (2026-11-28 06:10, overnight, owner asleep)
- 2026-11-28 morning: fmt-649 (Sonnet, wt-7 `chore/649-v3-fmt-check`) and psram-651 (Sonnet, wt-5 `fix/651-v3-probe-psram-hole`) running beside proc-617. v3 slots were set up with `git checkout -b <br> origin/v3` + `agent-wt claim` (a `reset --force` was refused).
- v3 tip 18b59011: #654/#653, #656 and #613 merged and closed overnight (#607 closed too). Agent proc-617 (Opus, wt-6 `feat/617-v3-process-model`) is running. Unattended bench = kernel load-ram at the ROM clock only; no #648 matrix runs, no flash writes.

## Live (2026-11-27 21:30, v3 sprint)
- v3 tip 53b00b43 (pushed): #648 Phase 1 merged (perf lab, probe-perf gate inside v3/check.sh). #650 open until the first 200 MHz run. v3 merges: rebase, ff-only, ./v3/check.sh, push from the slot (`git -C <slot> push origin HEAD:refs/heads/v3`), then `git fetch . origin/v3:refs/heads/v3`.
- v3 slots: `agent-wt done` refuses branches merged into v3 (not main). Recipe: `git -C <slot> checkout --detach origin/v3 && agent-wt release <slot>`; the branch stays (wt-5/6/8 branches kept).
- #648 perf lab: decisions on the issue (M0 skipped by the owner; 200 MHz and the 1900 step approved, 1950 = today). Main runs every 160/200 and LDO run.
- Bench lock: one load-ram at a time, runs of 120 s or less; roadmap first. wt-2 #587 and wt-3 #585 hold uncommitted v2 work.
- #615 next steps are owner flash writes.
- #648: Phase 1 merged; bench C1-C4 done and posted on #648 (200 MHz fails at the calibrated rail, 1900 step skipped by the owner, 128 B L2 lines = +62 % read). Open on #648: CPU 400, 512 KB L2, DMA bursts, `all` suite. #650 still needs a forced-failure retry run.
- #652 GUI overhaul: research committed (3e8899ca, docs/v3-gui-research/); mockups on a private design canvas; owner leans to Poster wall.
