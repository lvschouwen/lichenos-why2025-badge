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
| Min free internal heap | <15 % | <8 % | J; IDF V "some headroom" | `fw mem` (`internal ... min`) |
| Largest internal block | <50 % of free | <25 % of free | V fragmentation view, J numbers | `fw mem` (`internal ... largest` vs `free`) |
| PSRAM use | >75 % | >90 % | J | `fw mem` (`psram`), see gaps below |
| App image vs OTA slot | >85 % | >92 % | J | `badge_fw.bin` vs `ota_0` in `firmware/partitions.csv` |
| Per-archive growth per change | +2 KB | +8 KB | J; delta against a baseline V (Memfault) | `esp_idf_size --archives` vs the baseline |
| CPU idle per core | <30 % | <15 % | V NASA 50-70 % utilisation; numbers J | `fw mem` (`task IDLE0/IDLE1 ... cpu`) |
| ISR / interrupts off | >50 us | >200 us | J; IWDT 300/800 ms V is the crash line | cycle counter or scope (no read-out) |
| Task watchdog margin | longest run >25 % of 5 s | >50 % | V TWDT 5 s; J ratios | trace (no read-out) |
| Frame time | >16.7 ms | >33 ms p95 | V LVGL 33 ms default | `fw ui perf` |
| Input-to-display | >100 ms | >200 ms | J | key timestamp vs flush (no read-out) |
| Boot to first frame | >3 s | >5 s | J | `fw boot` timestamps, see gaps below |
| Draw buffer (partial mode) | | <1/10 screen | V LVGL | `lv_display_set_buffers` call |

The first six rows and the two size rows are gated by the build. The rest are read on the badge by the main session.

## Scope

Our code only: tracked files under `firmware/components/`, `firmware/main/`, `c6/main/` and `shared/` with the
extensions `.c .cpp .h .hpp`, minus `thirdparty/` and `managed_components/`. Vendored trees (`meshcore/`, `esp_hosted/`,
`third_party/`) and build directories are never measured. A new file or function is measured automatically: the gate asks
`git ls-files`, so a file must be `git add`ed (or be a tracked path) to count; a new component needs nothing else
because `firmware/CMakeLists.txt` turns `-fstack-usage` on for every directory under `components/`.

## How to measure

- Lines, complexity, nesting, parameters: `lizard -ENS <files>` (lizard 1.25.1, `pipx install lizard`). Per file the
  table at the end gives NLOC; per function the columns are NLOC, CCN, tokens, PARAM, length, NS (nesting).
- Stack frames: `-fstack-usage` writes `<file>.su` next to each object (`firmware/build/esp-idf/<component>/CMakeFiles/`,
  `c6/build/...`). It does not change the generated code. A line is `file:line:col:function<TAB>bytes<TAB>static|dynamic|bounded`.
  Frames are per function; the real use of a task is the deepest call chain, which `fw mem` shows as `stack_free` on the badge.
- Image and archives: `idf.py -C firmware size` (totals), `idf.py -C firmware size-components` (per component),
  `python -m esp_idf_size --archives --format csv firmware/build/badge_fw.map` (what the gate reads). Slot size: the
  `ota_0` row of `firmware/partitions.csv`.
- Runtime: `fw mem` (heaps free/largest/min for internal, dma, psram; per task core, `stack_free`, cpu % over the interval since the
  last `fw mem`; the IDLE0/IDLE1 lines give idle per core), `fw tasks` (stack size, owner), `fw ui perf [reset|bench]`
  (render, flush and wait times per frame), `fw boot` (start-up steps).
- Report without gating: `python3 -I tools/check_standard.py --report` lists everything past a warn line, worst first.

## The gate and the ratchet

`firmware/build.sh build` runs `tools/check_standard.sh` after the P4 image is built (about 4 s; it skips with a note if
lizard is missing). It runs `tools/check_standard.py`:

- Past the **fail** line and not in the baseline: the build fails (`NEW`).
- In the baseline but above its baselined value: the build fails (`WORSE`). An offender may not get worse.
- Past the **warn** line: printed, never fails; only for files changed since the merge base, so the output stays short.
- App image above 92 % of the OTA slot fails, above 85 % warns.
- An archive that grows by more than 8 KB over its baseline size fails (a new archive counts from zero), more than
  2 KB warns.
- A baseline entry that is fixed (or gone) is reported as `stale baseline`; it does not fail.

`tools/standard-baseline.txt` is the baseline. For the static metrics it holds the offenders past the fail line at their
current value and **may only shrink**: after fixing one run `python3 -I tools/check_standard.py --shrink`, which drops or
lowers entries and never adds or raises one. Never edit a value up to get a build through; split the function. The size
entries are the archive sizes at the last deliberate update: when a reviewed change legitimately grows an archive by more
than 8 KB, run `python3 -I tools/check_standard.py --size-update` with the IDF environment and a fresh build, and say why in the commit.
The gate's own logic is tested by `tools/tests/check_standard_test.py` (ctest `check_standard_test`).

## Where we stand (2026-11-07, fw built from this commit)

Measured over 548 files, 4198 functions, 63492 NLOC (lizard), 3052 functions in `.su` files.

| Metric | Median | Max | Past warn | Past fail |
|---|---|---|---|---|
| Lines per file (NLOC) | 43 | 3088 (`mesh_task.cpp`) | 6 | 1 |
| Lines per function | 9 | 172 (`app_main`) | 44 | 2 |
| CCN | 3 | 76 (`cmd_wifi`) | 82 | 18 |
| Nesting | 1 | 12 (`cmd_wifi`) | 23 | 4 |
| Parameters | 1 | 11 (`mesh_hist_compact`, `files_text_page`) | 40 | 6 |
| Stack frame | 16 B | 2448 B (`settings_backup_plan`) | 32 | 9 |

App image: 2,554,160 B of the 4 MiB OTA slot = 60.9 % (warn at 85 %). Internal RAM (DIRAM, static): 199,000 of 441,296 B
(45 %). Largest archives: `liblvgl` 542 KB, `libmesh` 374 KB, `libesp_stdio` 234 KB (almost all rodata, worth a look),
`liblichen_shell` 217 KB. The offenders past the fail line are the baseline file itself; the ranked list is in #202.
