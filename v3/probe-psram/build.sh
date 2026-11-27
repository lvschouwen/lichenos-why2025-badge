#!/usr/bin/env bash
# Build the #620 PSRAM probe: ELF, then an esptool RAM image for tools/v3-load-ram.sh.
#   ./probe-psram/build.sh [stage]   -> target/lichen/probe-psram/probe-psram.bin (+ .elf)
# stage (default all) is baked in at build time: dump | ldo | mpll | chip | tune | all (see src/main.rs).
# LICHEN_PSRAM_PHASE (0-3), LICHEN_PSRAM_DATA_DL and LICHEN_PSRAM_DQS_DL (0-15) override IDF's tuning pick (default: auto).
set -euo pipefail
cd "$(dirname "$0")/.."
TARGET=riscv32imac-unknown-none-elf
OUT=target/lichen/probe-psram
ESPTOOL=${ESPTOOL:-$(ls ~/.espressif/python_env/idf6.1_py3.*_env/bin/esptool 2>/dev/null | head -1)}
[ -x "$ESPTOOL" ] || { echo "esptool not found: set ESPTOOL" >&2; exit 2; }
mkdir -p "$OUT"
export LICHEN_PSRAM_STAGE=${1:-all}
cargo build --locked -q --release -p lichen-probe-psram --target $TARGET --target-dir target/probe-psram
cp "target/probe-psram/$TARGET/release/lichen-probe-psram" "$OUT/probe-psram.elf"
"$ESPTOOL" --chip esp32p4 elf2image --min-rev-full 0 --max-rev-full 199 -o "$OUT/probe-psram.bin" "$OUT/probe-psram.elf" >/dev/null
echo "probe-psram ($LICHEN_PSRAM_STAGE): $OUT/probe-psram.bin ($(stat -c %s "$OUT/probe-psram.bin") bytes)"
