#!/usr/bin/env bash
# Pack the stage ELF into an ESP image (esptool elf2image stays the packer, #615): pack.sh <elf> <out.bin>.
# `./pack.sh --trace <out.bin>` first builds the bench variant (feature `trace`: every ROM flash read is printed)
# and `--low <out.bin>` links at 0x4FF10000 instead of 0x4FF28ED0 (for a load that dies in the stage window).
set -euo pipefail
cd "$(dirname "$0")"
TARGET=riscv32imac-unknown-none-elf
case ${1:-} in
  --trace|--low)
    mode=$1; shift
    [ $# -eq 1 ] || { echo "usage: pack.sh $mode <out.bin>" >&2; exit 2; }
    feat=()
    if [ "$mode" = --trace ]; then feat=(--features trace); else export LICHEN_BOOT_BASE=0x4FF10000; fi
    cargo build --locked -q --release -p boot-esp32p4 "${feat[@]}" --target $TARGET --target-dir "target/variant${mode}"
    set -- "target/variant${mode}/$TARGET/release/boot-esp32p4" "$1"
    ;;
esac
[ $# -eq 2 ] || { echo "usage: pack.sh <elf> <out.bin>" >&2; exit 2; }
ESPTOOL=${ESPTOOL:-$(ls ~/.espressif/python_env/idf6.1_py3.*_env/bin/esptool 2>/dev/null | head -1 || true)}
[ -x "${ESPTOOL:-}" ] || ESPTOOL=$(command -v esptool || true)
[ -x "${ESPTOOL:-}" ] || { echo "esptool not found: set ESPTOOL" >&2; exit 2; }
"$ESPTOOL" --chip esp32p4 elf2image --min-rev-full 0 --max-rev-full 199 -o "$2" "$1" >/dev/null
echo "packed $2: $(stat -c %s "$2") bytes"
