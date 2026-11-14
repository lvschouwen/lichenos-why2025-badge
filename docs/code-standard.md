# Lichen code and resource standard

Issue #202. Limits for size, complexity and resource use, how each is measured, and how the build holds new code to them.
Labels on the basis: **V** verified from a primary source, **S** secondary source only, **J** judgment where no source
gives a number. Sources: Linux kernel coding style, clang-tidy `readability-function-size`, the lizard defaults,
SonarSource (cognitive complexity default 25 for C), ESP-IDF 6.1 docs for esp32p4 (RAM usage, heap debugging, `size`,
watchdogs), FreeRTOS `task.h`, NASA SWEHB (50 to 70 % CPU utilisation margin), Memfault Interrupt (stack usage, size
deltas per change against the merge base), LVGL 9.x docs (33 ms refresh, partial buffer at least 1/10 of the screen),
JPL "Power of 10" rule 4 (only via a secondary page). Not reachable: Barr Group, MISRA, Holzmann's paper.

"Past the line" always means a value strictly above the warn or fail number (below, for the runtime floors).

## The table

| Metric | Warn | Fail | Basis | Measured by |
|---|---|---|---|---|
| Lines per file | 800 | 1500 | J (house rule 800) | lizard NLOC per file |
| Lines per function | 60 | 150 | warn S (JPL rule 4), fail J | lizard `length` (first to last line) |
| Cyclomatic complexity | 15 | 25 | V lizard default 15; S NIST 10-15; fail V Sonar C 25 (cognitive, an analogue) | lizard CCN |
| Nesting depth | 4 | 6 | J (Linux says 3) | lizard `-ENS` (nested control structures) |
| Parameters | 6 | 8 | J | lizard PARAM |
| Stack frame | 512 B | 1 KB | J; `-fstack-usage` V (Memfault) | `.su` files of our components |
| Task stack high-water | <25 % free | <15 % or <256 B | J; IDF V "some safety margin" | `fw mem` (`stack_free`) vs `fw tasks` (size) |
| Min free internal heap | <15 % | <8 % | J; IDF V "some headroom" | `fw mem` (`internal ... min_pct`) |
| Largest internal block | <50 % of free | <25 % of free | V fragmentation view, J numbers | `fw mem` (`internal ... largest` vs `free`) |
| PSRAM use | >75 % | >90 % | J | `fw mem` (`psram ... used_pct`) |
| App image vs OTA slot | >75 % | >90 % | J; measured 62.6 % P4, 48.1 % C6 (#297) | `badge_fw.bin` vs `ota_0` in `firmware/partitions.csv` |
| Per-archive growth per change | +2 KB | +8 KB | J; delta against a baseline V (Memfault) | `esp_idf_size --archives` vs the sizes the parent commit recorded |
| CPU idle per core | <30 % | <15 % | V NASA 50-70 % utilisation; numbers J | `fw mem` (`task IDLE0/IDLE1 ... cpu`) |
| ISR / interrupts off | >50 us | >200 us | J; IWDT 300/800 ms V is the crash line | cycle counter or scope (no read-out; known gap, not yet tracked) |
| Task watchdog margin | longest run >25 % of 5 s | >50 % | V TWDT 5 s; J ratios | trace (no read-out; known gap, not yet tracked) |
| Frame time | >16.7 ms | >33 ms p95 | V LVGL 33 ms default | `fw ui perf` (developer mode) |
| Input-to-display | >100 ms | >200 ms | J | `fw ui perf` (developer mode; `key-to-flush ... p95`, physical keys only) |
| Boot to first frame | >3 s | >5 s | J | `fw ui perf` (developer mode; `first frame at <ms> ms`) |
| Draw buffer (partial mode) | | <1/10 screen | V LVGL | `lv_display_set_buffers` call |

The first six rows and the two size rows are gated by the build. The rest are read on the badge by the main session.

## Scope

Our code only: tracked files under `firmware/components/`, `firmware/main/`, `c6/main/` and `shared/` with the
extensions `.c .cpp .h .hpp`, minus `thirdparty/` and `managed_components/`. Vendored trees (`meshcore/`, `esp_hosted/`,
`third_party/`) and build directories are never measured. A new file or function is measured automatically: the gate asks `git ls-files` plus the untracked-not-ignored files, so it counts before it is `git add`ed; a new component needs nothing else
because `firmware/CMakeLists.txt` turns `-fstack-usage` on for every directory under `components/`.

## How to measure

- Lines, complexity, nesting, parameters: `lizard -ENS <files>` (lizard 1.25.1, `pipx install lizard`). Per file the
  table at the end gives NLOC; per function the columns are NLOC, CCN, tokens, PARAM, length, NS (nesting).
- Stack frames: `-fstack-usage` writes `<file>.su` next to each object (`firmware/build/esp-idf/<component>/CMakeFiles/`,
  `c6/build/...`). It does not change the generated code. A line is `file:line:col:function<TAB>bytes<TAB>static|dynamic|bounded`.
  Frames are per function; the real use of a task is the deepest call chain, which `fw mem` shows as `stack_free` on the badge.
  Only `.su` files whose object is an output in that build dir's `compile_commands.json` count: ninja leaves the `.obj`/`.su` of a moved or dropped source behind (#239).
- Image and archives: `idf.py -C firmware size` (totals), `idf.py -C firmware size-components` (per component),
  `python -m esp_idf_size --archives --format csv firmware/build/badge_fw.map` (what the gate reads). Slot size: the
  `ota_0` row of `firmware/partitions.csv`.
- Runtime: `fw mem` (heaps free/largest/min for internal, dma, psram; per task core, `stack_free`, cpu % over the interval since the
  last `fw mem`; the IDLE0/IDLE1 lines give idle per core; `mem dma_low` lines: when the DMA min fell by 2 KB or more,
  with the uptime, free then, and `big`/`task`: the largest internal allocation (1 KB or more) in the 250 ms windows
  where that record's min fell and the task that made it (an ISR's is blamed on the task it interrupted; `?` when the
  name could not be read consistently), #256), `fw tasks` (stack size, owner), `fw ui perf [reset|bench]`
  (render, flush and wait times per frame; `fw ui` and `fw keys` are developer-mode commands since #377: `fw dev on`
  from USB first), `fw boot` (start-up steps).
- Report without gating: `python3 -I tools/check_standard.py --report` lists everything past a warn line, worst first.

## The gate and the ratchet

The standard gate checks the developer image only. `build` then builds the release image too (`firmware/build-release`,
#401, about 10 s warm with nothing changed, up to about 1 min after a source change; measured full check 1m31 without, 1m42 with) and runs `tools/check_release_image.sh` on it: the marker `LICHEN-DEBUG-BUILD-MARKER`
(`fw_console/console.c`, only with `CONFIG_LICHEN_DEV_COMMANDS`) must be absent there and present in the developer image.
The release config must also have every debug-command symbol off (`check_release_image.sh config`). `BADGE_SKIP_RELEASE=1` (exactly 1) skips it and exits 3, so such a run never earns a green stamp.

`firmware/build.sh build` (also `all` and `app`; not `reconfigure` or `storage`) runs `tools/check_standard.sh` after the
P4 image is built, about 3 s. It runs `tools/check_standard.py`. Nothing passes silently:

- **lizard missing fails the build.** The only way past is the explicit `BADGE_SKIP_STANDARD=1`, which prints a loud
  "SKIPPED" line.
- **Incomplete measurement fails** (`INCOMPLETE`): lizard exiting non-zero on a chunk, a file without a lizard result, no `.su`
  files in `firmware/build` or `c6/build` (stack frames not on), a missing image or map, `esp_idf_size` failing or giving no
  archives. The gate says which. Files are `git ls-files` plus untracked-not-ignored ones, so a new file counts before it is added.
- Past the **fail** line and not in the baseline: fails (`NEW`). In the baseline but above its value: fails (`WORSE`).
- Past the **warn** line: printed, never fails; only for files changed since the merge base.
- **Stale baseline entries fail** (`STALE`): an offender that was fixed, lowered or removed must also be removed or lowered in
  the baseline (`--shrink`). The gate does not rewrite the static (complexity, length, stack) lines on its own, so that diff
  stays a deliberate, reviewable edit. Only the `size` lines are rewritten automatically, see below.
- **The baseline only shrinks, enforced:** every static entry that is new, or higher than in the merge-base version of
  `tools/standard-baseline.txt` (HEAD if there is no merge base), fails (`GREW`). The one change that introduces the file
  is exempt, since there is nothing to compare with.
- Images: P4 app above 90 % of `ota_0` fails, above 75 % warns (set in #297 from the measured 62.6 % P4 and 48.1 % C6); the same for the C6 `network_adapter.bin` against the
  `ota_0` of `c6/partitions.esp32c6.csv`.
- **Size growth is per linked archive, from the link map** (`esp_idf_size --archives`), not per component from
  `idf.py size-components`: the archive list is machine-readable and stable, and `size-components` only prints a table
  whose columns shift with the memory layout. Growth is **per branch / per push** (#297): an archive is compared with the
  size recorded in the `size` lines of `tools/standard-baseline.txt` at the merge base of `HEAD` and `origin/main`
  (`git merge-base HEAD refs/remotes/origin/main`, then `git show`). In a slot that gates the whole branch; in main it
  gates exactly what the next push adds. Rebuilds, amends, rebases and several commits cannot add up unchecked.
  More than +8 KB fails (a new archive counts from zero), more than +2 KB warns, shrinks are silent. A **green** build
  rewrites only the numbers of the `size` lines to the measured values (shrinks and growth up to the fail line alike; new
  archives are inserted sorted; every other line stays byte for byte; a red build never writes), so the commit carries its
  own sizes. After a rebase, rebuild and commit the rewritten size lines (two branches that changed one archive conflict
  on that line). The gate fails closed: no git, no `origin/main` or no baseline at the merge base is an error (fetch
  origin); `--no-ref` skips the reference on purpose (fresh repo) and says so. There is no per-archive baseline for the
  C6 image (gap).
- **Merged string pools are their own line, `size <merged-strings>`.** The linker folds every `.rodata.*.str1.*` and
  `.cst*` input section (SHF_MERGE) into one image-wide blob and books all of it on the first contributing object, so
  the pool (~240 KB) used to sit under `libesp_stdio.a` and any change of link order moved it to another archive as a
  false GREW and a fake shrink. The gate reads the pool input sections from `badge_fw.map`, subtracts them from their
  owning archive and sums them into one line, gated with the same per-change +2 KB warn / +8 KB fail rules. Growth of the pool
  means more or longer strings anywhere in the image; look at the diff of the strings, not at one archive.

### Editing the baseline

- Fixed or lowered an offender: `python3 -I tools/check_standard.py --shrink` (drops and lowers; never adds or raises; refuses
  when the measurement is incomplete). Run the baseline commands inside the IDF Python environment
  (`. ~/esp/esp-idf-v6.1/export.sh` first): the size part needs `esp_idf_size`, and without it the measurement counts
  as incomplete and the command refuses.
- Moved a file or renamed a function that is a baselined offender: `python3 -I tools/check_standard.py --rename OLD NEW`.
  OLD/NEW are a path (all its entries move, `file:function` keys follow) or `path:function`. The values are kept and
  `moved` records tell the shrink-only check the new key is the old entry, so it passes only if the value did not grow.
  `--shrink` later drops the records once the merge base has the new key.
- Intended growth of an archive beyond 8 KB: `--size-update` (IDF environment, fresh build). It prints every changed
  archive, so the diff is reviewable; give the reason in the commit message. The gate accepts growth past the fail line
  when the file's value is at least the measured one, and prints a warning on every run ("accepted by the baseline
  line: needs Lucas's ok"). That is an honour rule, not a provenance check: a hand-edited value passes the same way.
- `--init` creates the first baseline and refuses when the file exists. Never edit a value up to get a build through; split
  the function.

Function keys are `path:name`; a second function of the same name in a file (C++ overloads, helpers under `#ifdef`) is
`path:name#2` in source order, so keys stay stable across unrelated edits and need no line numbers. Stack keys are
`path:function` as gcc prints it (C++ with its signature); one source built into two objects keeps the larger frame.
The gate's own logic is tested by `tools/tests/check_standard_test.py` (ctest `check_standard_test`).

## Where we stand (2026-11-14; `check_standard.py --report` gives today's)

Past-warn and max columns are from `--report` at the commit of #444; the medians and the file and function counts are
from 2026-11-07 (`--report` does not print them).

| Metric | Median | Max | Past warn | Past fail |
|---|---|---|---|---|
| Lines per file (NLOC) | 43 | 1481 (`ota_server.c`) | 9 | 0 |
| Lines per function | 9 | 129 (`boot_screen_create`) | 27 | 0 |
| CCN | 3 | 25 (`handle_connection`) | 64 | 0 |
| Nesting | 1 | 6 (`cmd_sensors`) | 17 | 0 |
| Parameters | 1 | 8 (`draw_chart`) | 43 | 0 |
| Stack frame | 16 B | 1872 B (`mesh_notify_drain`) | 40 | 1 |

App image: 2,800,352 B of the 4 MiB OTA slot = 66.8 % (warn at 75 %). Internal RAM (DIRAM, static): not re-measured
(199,000 B on 2026-11-07). Largest archives: see the `size` lines of `tools/standard-baseline.txt`.
The offenders past the fail line are the baseline file itself; the ranked list is in #202.
