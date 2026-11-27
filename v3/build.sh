#!/usr/bin/env bash
# Build the v3 PoC: apps first (each linked for its slot), then the kernel that embeds them.
#   ./build.sh [qemu-virt|esp32p4] [run]
set -euo pipefail
cd "$(dirname "$0")"
PLAT=${1:-qemu-virt}
TARGET=riscv32imac-unknown-none-elf
OUT=target/lichen/$PLAT
OBJCOPY=$(rustc --print sysroot)/lib/rustlib/x86_64-unknown-linux-gnu/bin/llvm-objcopy
mkdir -p "$OUT"
case $PLAT in
  qemu-virt) TEXT0=$((0x80400000)); DATA0=$((0x80800000)); LEN=$((0x10000)); FEAT=qemu-virt ;;
  esp32p4)   . ./kernel/esp32p4-slots.sh; FEAT=esp32p4 ;;
  *) echo "unknown platform $PLAT" >&2; exit 2 ;;
esac
app() { # name slot
  local ld=$PWD/$OUT/$1.ld
  sed -e "s/@TEXT@/$(printf 0x%x $((TEXT0 + $2 * LEN)))/" -e "s/@DATA@/$(printf 0x%x $((DATA0 + $2 * LEN)))/" \
      -e "s/@LEN@/$(printf 0x%x $LEN)/" apps/app.ld.in > "$ld"
  LICHEN_APP_LD=$ld cargo build -q --release -p "$1" --target $TARGET --target-dir "target/app-$1-$PLAT"
  "$OBJCOPY" -O binary "target/app-$1-$PLAT/$TARGET/release/$1" "$OUT/$1.bin"
  echo "app $1: $(stat -c %s "$OUT/$1.bin") bytes"
}
app heartbeat 0
app attacker 1
LICHEN_APP_HEARTBEAT=$PWD/$OUT/heartbeat.bin LICHEN_APP_ATTACKER=$PWD/$OUT/attacker.bin \
  cargo build -q --release -p lichen-kernel --target $TARGET --no-default-features --features $FEAT --target-dir target/kernel-$PLAT
KELF=target/kernel-$PLAT/$TARGET/release/lichen-kernel
echo "kernel: $KELF"
if [ "${2:-}" = run ] && [ "$PLAT" = qemu-virt ]; then
  timeout 60 qemu-system-riscv32 -machine virt -bios none -nographic -m 128M -kernel "$KELF"
fi
