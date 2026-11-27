# v3 performance tracking (#643)

- `targets.toml`: one table per metric with the v2 baseline (value, fw commit, conditions, source issue), the target,
  an optional stretch, the direction and `ratchet`. A v2 value that is not in the repo or an issue is `todo`.
- `best.toml`: best measured value per ratcheted metric. Only improves, written by `--update-best`.
- `results/<build-id>.json`: what one run measured.

## PERF line

The kernel console prints `PERF <metric.dotted> key=value ...` next to the CHECK lines. Values are plain numbers
(no units, no suffix); the unit goes in the key name: `PERF kernel.syscall.local median_us=3.1 p95_us=4.8`.
The metric name must exist in `targets.toml` and its `key` names the value that is compared. A later line for the
same metric replaces an earlier one.

## Run

    python3 -I v3/tools/perf_report.py --log <load-ram log> --build <id> [--update-best]

Prints metric | v2 | v3 | target | % vs v2 | status | commit and writes `results/<id>.json`. Status is `ok`,
`stretch`, `miss`, `REGRESS` or `-` (not measured: skipped, never a failure).

## Ratchet

A metric with `ratchet = true` fails the run (exit 1) when it is worse than its `best.toml` value by more than
`tolerance_pct` (default 5). Set `ratchet = true` only once the metric is measurable on a v3 build. `--update-best`
never moves best on a failing run and only replaces a value with a strictly better one. Baselines only improve:
loosening a best value by hand needs the owner's ok.
