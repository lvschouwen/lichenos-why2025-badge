#!/usr/bin/env bash
# Build the #608 load-ram probe: ELF, then an esptool image (RAM segments only) with its id stamped in.
#   ./probe-p4/build.sh   -> target/lichen/probe-p4/probe-p4.bin (+ .elf), prints the probe id
set -euo pipefail
cd "$(dirname "$0")/.."
TARGET=riscv32imac-unknown-none-elf
OUT=target/lichen/probe-p4
ESPTOOL=${ESPTOOL:-$(ls ~/.espressif/python_env/idf6.1_py3.*_env/bin/esptool 2>/dev/null | head -1)}
[ -x "$ESPTOOL" ] || { echo "esptool not found: set ESPTOOL" >&2; exit 2; }
ID=$(git rev-parse --short=8 HEAD)$(git diff --quiet -- probe-p4 || echo -dirty)
mkdir -p "$OUT"
LICHEN_PROBE_ID=$ID cargo build -q --release -p lichen-probe-p4 --target $TARGET --target-dir target/probe-p4
ELF=target/probe-p4/$TARGET/release/lichen-probe-p4
cp "$ELF" "$OUT/probe-p4.elf"
"$ESPTOOL" --chip esp32p4 elf2image --min-rev-full 0 --max-rev-full 199 -o "$OUT/probe-p4.bin" "$OUT/probe-p4.elf" >/dev/null
echo "probe $ID: $OUT/probe-p4.bin ($(stat -c %s "$OUT/probe-p4.bin") bytes)"
