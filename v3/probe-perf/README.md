# probe-perf: the #648 perf lab

A RAM-only image for `tools/v3-load-ram.sh` that measures one point of the matrix and prints the result in the bench-log protocol
(`CHECK`, `PERF`, `BEGIN`, `END`). Experiments live here and nowhere else: the kernel's dependency tree never contains the
`bench-experiments` feature (`v3/check.sh` guards it). The maths (statistics, patterns, LDO words, option validation) is in
`v3/perflab`, which has host tests; this crate is the register access.

## Build one point

    v3/probe-perf/build.sh [--cpu 360|400] [--psram 120|160|200] [--l2kb 128|256|512] [--l2line 64|128] [--dma 32|64|128]
                           [--ldo cal|1900] [--suite smoke|all|config|mem|cpu|irq|dma|integrity|park|dual] [--secs N]

The image is `v3/target/lichen/probe-perf/probe-perf-<tag>-<suite>.bin`, `<tag>` = `cpu360-ps120-l2k256l64-dma32-ldocal`. The tag is
also in the banner (`lichen-v3 <sha>-<tag> platform esp32p4 perflab ...`), so a stale file cannot pass for another point. Defaults are
the production point. Impossible points do not build: 512 KB L2 with 64 B lines, CPU above 400 MHz,
any LDO value but `cal` and `1900`, a DMA burst other than 32, 64 or 128 B. `v3/probe-perf/check.sh` is the gate (fmt, host tests,
clippy, every switch value, the refusals).

## Suites

| suite | what | length |
|---|---|---|
| `config` | bring-up and the register read-back only | seconds |
| `mem` | internal RAM and cached PSRAM: sequential read, write, copy, random reads, pointer chase from 8 KB to 8 MB | seconds |
| `cpu` | ALU kernel and straight-line code (24 KB, 512 KB) run from internal RAM and from PSRAM, CRC over data in both | seconds |
| `irq` | CLINT timer against SYSTIMER and mcycle (the counter's measured rate), 1 kHz interrupt latency and jitter, idle and with random PSRAM reads | 3 s |
| `dma` | AXI-GDMA memory to memory on four paths: MB/s, share of the CPU left, setup cycles | seconds |
| `integrity` | whole chip, dense inverted multi-pass, written by the CPU through the cache and by DMA, read back both ways | `--secs`, default 10 (smoke) or 60 |
| `park` | 5 s untouched, CPU-written and DMA-written 1 MB (the #569 style) | about 6 s |
| `dual` | core 1 streams PSRAM reads or writes (checks STOP every 256 KB) while core 0 reads 3 x 4 MB, measures a pointer chase, or reads internal RAM | seconds; always last |
| `smoke` | mem, cpu, irq, dma, integrity (10 s, at least one pass of each kind), park, dual | 24 s measured (`lab.run elapsed_ms`) |
| `all` | the same with 60 s of integrity | not yet run; about 75 s estimated (smoke minus its 10 s of integrity plus 60 s), read `lab.run` from the first run |

Plan a run at 120 s or less (`--secs` shortens it). It stops at the first flip, DMA fault, trap, panic or temperature at 85 C, records it as a failed point
(`END fail`) and puts the PSRAM rail back.

## Output

`PERF <metric> key=value ...` (the #643 grammar, `v3/perf/README.md`): decimal values, the unit in the key name, the variant in the metric
name. `lab.config` is read back from the registers (CPU measured, PSRAM clock, L2 size and line, DMA burst, LDO pair), never printed
from the build options; `648.config` fails when they differ. Metrics: `lab.mem.sram.*`, `lab.mem.psram.*`, `lab.mem.chase.{psram,sram}.ws<size>`
(ns per dependent load; stride 128 B so every hop is a new line), `lab.dma.<src>_to_<dst>`, `lab.integrity.{cached,dma_write,dma_read}.<pattern>`,
`lab.temp.<stage>`. Lab metrics are not v3 targets, so `perf_report.py` stores them in the result JSON and ratchets nothing on them.

    python3 -I v3/tools/perf_report.py --log <log> --build <id>
    python3 tools/v3_bench_report.py <log> --require 648.config 648.psram 648.integrity 648.park

## Safety

- The PSRAM rail: `--ldo 1900` writes dref 7 / mul 5 (nominal 1912.5 mV, below the calibrated 1950 mV) in ONE masked 32-bit write to
  0x5011_51D4, waits 200 us (SYSTIMER, not the ROM delay) and checks dref, mul, XPD and TIEH. It is refused unless the die temperature
  reads, and unless the rail is on 12/2 with TIEH off. The calibrated pair is written back by one raw masked write, before anything is
  printed, on the normal end, every failure, the panic handler and the trap handler, and read back (`648.ldo.restore`). No pair above 12/2
  can be expressed (`perflab::ldo`). After a hang the recovery is a power cycle or EN reset, not a soft reset (the PMU keeps the setting).
- The step happens after PSRAM init and tuning, which run at the calibrated pair: `psram::init` always programs 12/2 itself, so the
  tuning cannot be redone at the stepped voltage without a hook in `psram.rs` (#650).
- No eFuse write, no flash write; PSRAM is written only through the cached alias or by DMA, and read back through the cache after the lines
  were dropped (uncached reads only to diagnose). The whole chip is mapped with the hole 0x54_0000-0x5C_0000; flips there are counted in
  `watch` (0x400 mod 0x800 inside 0x54_0000-0x64_0000, the v2 fingerprint).
- Temperature is the uncalibrated LP_TSENS reading (no eFuse delta): good for trends and the stop at 85 C, not an absolute figure. Power is
  not measurable on this badge (#11 gauge not fitted).

## Open

Syscall and context-switch latency need a kernel and wait for #617; Phase 1 covers timer accuracy and CLINT interrupt latency and jitter only.
