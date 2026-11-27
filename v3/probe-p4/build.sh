#!/usr/bin/env bash
# Build the #608 load-ram probe: ELF, then an esptool image (RAM segments only) with its id stamped in.
#   [LICHEN_PROBE_RUN=b5|b6|core1|core0|dgpio] ./probe-p4/build.sh -> target/lichen/probe-p4/probe-p4.bin (+ .elf), prints the id
#   LICHEN_PROBE_RUN=app LICHEN_PROBE_SUITE=<c1pmp|pmp1|c1timer|psram|dgpio> ./probe-p4/build.sh
#                         -> target/lichen/probe-p4/probe-p4-<suite>.bin, one #609 check per app image (src/s609.rs)
set -euo pipefail
cd "$(dirname "$0")/.."
TARGET=riscv32imac-unknown-none-elf
OUT=target/lichen/probe-p4
ESPTOOL=${ESPTOOL:-$(ls ~/.espressif/python_env/idf6.1_py3.*_env/bin/esptool 2>/dev/null | head -1)}
[ -x "$ESPTOOL" ] || { echo "esptool not found: set ESPTOOL" >&2; exit 2; }
ID=$(git rev-parse --short=8 HEAD)$(git diff --quiet -- probe-p4 || echo -dirty)
SUITE=${LICHEN_PROBE_SUITE:-}
NAME=probe-p4
if [ -n "$SUITE" ]; then
    case $SUITE in c1pmp|pmp1|c1timer|psram|dgpio) ;; *) echo "unknown LICHEN_PROBE_SUITE $SUITE" >&2; exit 2 ;; esac
    [ "${LICHEN_PROBE_RUN:-}" = app ] || { echo "LICHEN_PROBE_SUITE needs LICHEN_PROBE_RUN=app" >&2; exit 2; }
    NAME=probe-p4-$SUITE
fi
mkdir -p "$OUT"
LICHEN_PROBE_ID=$ID cargo build -q --release -p lichen-probe-p4 --target $TARGET --target-dir target/probe-p4
ELF=target/probe-p4/$TARGET/release/lichen-probe-p4
cp "$ELF" "$OUT/$NAME.elf"
if [ "${LICHEN_PROBE_RUN:-}" = app ]; then
    # #608 step 3b: an app image for ota_1. Header flash settings as Lichen's; the ELF SHA-256 into esp_app_desc_t
    "$ESPTOOL" --chip esp32p4 elf2image --min-rev-full 0 --max-rev-full 199 --flash-mode dio --flash-freq 80m \
        --flash-size 16MB --elf-sha256-offset 0xb0 -o "$OUT/$NAME.bin" "$OUT/$NAME.elf" >/dev/null
else
    "$ESPTOOL" --chip esp32p4 elf2image --min-rev-full 0 --max-rev-full 199 -o "$OUT/probe-p4.bin" "$OUT/probe-p4.elf" >/dev/null
fi
echo "probe $ID${SUITE:+ suite $SUITE}: $OUT/$NAME.bin ($(stat -c %s "$OUT/$NAME.bin") bytes)"
