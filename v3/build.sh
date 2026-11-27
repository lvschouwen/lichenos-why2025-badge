#!/usr/bin/env bash
# Build the v3 kernel.
#   ./build.sh qemu-virt [run]   the #607 isolation harness: apps first (each linked for its slot), then the kernel
#   ./build.sh esp32p4           the RAM-only bring-up image: target/lichen/esp32p4/kernel.bin (+ .elf), for load-ram
# The banner's build id is the git sha, "-dirty" when v3/ has uncommitted changes: rebuild after every commit or rebase.
set -euo pipefail
cd "$(dirname "$0")"
PLAT=${1:-qemu-virt}
TARGET=riscv32imac-unknown-none-elf
OUT=target/lichen/$PLAT
OBJCOPY=$(rustc --print sysroot)/lib/rustlib/x86_64-unknown-linux-gnu/bin/llvm-objcopy
mkdir -p "$OUT"
export LICHEN_BUILD_ID=$(git rev-parse --short=8 HEAD)$(git diff --quiet HEAD -- . || echo -dirty)
case $PLAT in
  qemu-virt) TEXT0=$((0x80400000)); DATA0=$((0x80800000)); LEN=$((0x10000)) ;;
  esp32p4) ;;
  *) echo "unknown platform $PLAT" >&2; exit 2 ;;
esac
app() { # name slot
  local ld=$PWD/$OUT/$1.ld
  sed -e "s/@TEXT@/$(printf 0x%x $((TEXT0 + $2 * LEN)))/" -e "s/@DATA@/$(printf 0x%x $((DATA0 + $2 * LEN)))/" \
      -e "s/@LEN@/$(printf 0x%x $LEN)/" apps/app.ld.in > "$ld"
  LICHEN_APP_LD=$ld cargo build --locked -q --release -p "$1" --target $TARGET --target-dir "target/app-$1-$PLAT"
  "$OBJCOPY" -O binary "target/app-$1-$PLAT/$TARGET/release/$1" "$OUT/$1.bin"
  echo "app $1: $(stat -c %s "$OUT/$1.bin") bytes"
}
if [ "$PLAT" = qemu-virt ]; then
  app heartbeat 0
  app attacker 1
  LICHEN_APP_HEARTBEAT=$PWD/$OUT/heartbeat.bin LICHEN_APP_ATTACKER=$PWD/$OUT/attacker.bin \
    cargo build --locked -q --release -p lichen-kernel --target $TARGET --no-default-features --features qemu-virt \
      --target-dir target/kernel-qemu-virt
  KELF=target/kernel-qemu-virt/$TARGET/release/lichen-kernel
  echo "kernel: $KELF"
  if [ "${2:-}" = run ]; then
    timeout 60 qemu-system-riscv32 -machine virt -bios none -nographic -m 128M -kernel "$KELF"
  fi
else
  ESPTOOL=${ESPTOOL:-$(ls ~/.espressif/python_env/idf6.1_py3.*_env/bin/esptool 2>/dev/null | head -1)}
  [ -x "$ESPTOOL" ] || { echo "esptool not found: set ESPTOOL" >&2; exit 2; }
  cargo build --locked -q --release -p lichen-kernel --target $TARGET --no-default-features --features esp32p4 \
    --target-dir target/kernel-esp32p4
  KELF=target/kernel-esp32p4/$TARGET/release/lichen-kernel
  cp "$KELF" "$OUT/kernel.elf"
  "$ESPTOOL" --chip esp32p4 elf2image --min-rev-full 0 --max-rev-full 199 -o "$OUT/kernel.bin" "$OUT/kernel.elf" >/dev/null
  echo "kernel $LICHEN_BUILD_ID: $OUT/kernel.bin ($(stat -c %s "$OUT/kernel.bin") bytes)"
fi
