#!/usr/bin/env bash
# Build one configuration of the #648 perf lab: ELF, then a RAM image (the ROM download format) for tools/v3-load-ram.sh.
#   ./probe-perf/build.sh [--cpu 360|400] [--psram 120|160] [--l2kb 128|256|512] [--l2line 64|128] [--dma 32|64|128]
#                         [--ldo cal|1900] [--suite smoke|all|config|mem|cpu|irq|dual|dma|integrity|park] [--secs N]
# -> target/lichen/probe-perf/probe-perf-<config tag>-<suite>.bin (+ .elf): the tag is in the file name and in the image's banner, so a
# stale variant cannot be loaded for another. Defaults are the production point (CPU 360, PSRAM 120, L2 256 KB / 64 B, burst 32 B,
# calibrated rail). Impossible combinations (PSRAM 200 before #650, 512 KB with 64 B lines, CPU above 400, an LDO value other than
# `cal` or `1900`) do not build. --secs is the length of the integrity passes (default 10 for smoke, 60 otherwise).
set -euo pipefail
cd "$(dirname "$0")/.."
TARGET=riscv32imac-unknown-none-elf
OUT=target/lichen/probe-perf
cpu=360 psram=120 l2kb=256 l2line=64 dma=32 ldo=cal suite=smoke secs=0
while [ $# -gt 0 ]; do
    [ $# -ge 2 ] || { echo "missing value for $1" >&2; exit 2; }
    case $1 in
        --cpu) cpu=$2 ;; --psram) psram=$2 ;; --l2kb) l2kb=$2 ;; --l2line) l2line=$2 ;;
        --dma) dma=$2 ;; --ldo) ldo=$2 ;; --suite) suite=$2 ;; --secs) secs=$2 ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
    shift 2
done
for v in "$cpu" "$psram" "$l2kb" "$l2line" "$dma" "$secs"; do
    case $v in ''|*[!0-9]*) echo "not a number: $v" >&2; exit 2 ;; esac
done
case $suite in smoke|all|config|mem|cpu|irq|dual|dma|integrity|park) ;; *) echo "unknown suite $suite" >&2; exit 2 ;; esac
tag="cpu$cpu-ps$psram-l2k${l2kb}l$l2line-dma$dma-ldo$ldo"
id=$(git rev-parse --short HEAD 2>/dev/null || echo dev)
git diff --quiet HEAD -- 2>/dev/null || id="$id+"
IMG_TOOL=${IMG_TOOL:-$(ls ~/.espressif/python_env/idf6.1_py3.*_env/bin/esptool 2>/dev/null | head -1)}
[ -x "$IMG_TOOL" ] || { echo "image converter not found: set IMG_TOOL to the IDF esptool" >&2; exit 2; }
mkdir -p "$OUT"
export LICHEN_PERF_CPU=$cpu LICHEN_PERF_PSRAM=$psram LICHEN_PERF_L2KB=$l2kb LICHEN_PERF_L2LINE=$l2line LICHEN_PERF_DMA=$dma
export LICHEN_PERF_LDO=$ldo LICHEN_PERF_SUITE=$suite LICHEN_PERF_SECS=$secs LICHEN_PERF_ID="$id-$tag"
cargo build --locked -q --release -p lichen-probe-perf --target $TARGET --target-dir target/probe-perf
name="probe-perf-$tag-$suite"
cp "target/probe-perf/$TARGET/release/lichen-probe-perf" "$OUT/$name.elf"
"$IMG_TOOL" --chip esp32p4 elf2image --min-rev-full 0 --max-rev-full 199 -o "$OUT/$name.bin" "$OUT/$name.elf" >/dev/null
echo "probe-perf ($tag, $suite): $OUT/$name.bin ($(stat -c %s "$OUT/$name.bin") bytes)"
