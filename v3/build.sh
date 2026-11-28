#!/usr/bin/env bash
# Build the v3 kernel.
#   ./build.sh qemu-virt [run]   the #607 isolation harness: apps first (each linked for its slot), then the kernel
#   ./build.sh esp32p4           the RAM-only bring-up image: target/lichen/esp32p4/kernel.bin (+ .elf), for load-ram;
#                                it ends with the same isolation harness, apps linked for the P4 slots (#613)
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
  # The app slots of kcore/src/slots.rs (SLOT_TEXT, SLOT_DATA, SLOT_LEN): NOLOAD L2MEM above the kernel (#613).
  esp32p4) TEXT0=$((0x4FF60000)); DATA0=$((0x4FF70000)); LEN=$((0x8000)) ;;
  *) echo "unknown platform $PLAT" >&2; exit 2 ;;
esac
app() { # name slot [output name: a second link of one crate for another slot, #618]
  local out=${3:-$1} ld=$PWD/$OUT/${3:-$1}.ld
  sed -e "s/@TEXT@/$(printf 0x%x $((TEXT0 + $2 * LEN)))/" -e "s/@DATA@/$(printf 0x%x $((DATA0 + $2 * LEN)))/" \
      -e "s/@LEN@/$(printf 0x%x $LEN)/" apps/app.ld.in > "$ld"
  LICHEN_APP_LD=$ld cargo build --locked -q --release -p "$1" --target $TARGET --target-dir "target/app-$out-$PLAT"
  "$OBJCOPY" -O binary "target/app-$out-$PLAT/$TARGET/release/$1" "$OUT/$out.bin"
  # The loader refuses an image whose header end differs from its length (an app with no rodata or data once ended on
  # a 2-byte boundary, #618): fail here, not at load.
  local end want=$((TEXT0 + $2 * LEN + $(stat -c %s "$OUT/$out.bin")))
  end=$(od -An -t u4 -j16 -N4 "$OUT/$out.bin" | tr -d ' ')
  [ "$end" = "$want" ] || { echo "app $out: header image end $end != slot text + length $want" >&2; exit 1; }
  echo "app $out: $(stat -c %s "$OUT/$out.bin") bytes"
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
  app heartbeat 0
  app attacker 1
  # The #618 process-stage apps: separate crates, linked on their own, loaded into slots 0 and 1 by core 0.
  app worker 0
  app rogue 1
  app rogue 0 rogue0 # the same crate for slot 0: the ICache case loads it over what ran there
  LICHEN_APP_HEARTBEAT=$PWD/$OUT/heartbeat.bin LICHEN_APP_ATTACKER=$PWD/$OUT/attacker.bin \
  LICHEN_APP_WORKER=$PWD/$OUT/worker.bin LICHEN_APP_ROGUE=$PWD/$OUT/rogue.bin LICHEN_APP_ROGUE0=$PWD/$OUT/rogue0.bin \
  cargo build --locked -q --release -p lichen-kernel --target $TARGET --no-default-features --features esp32p4 \
    --target-dir target/kernel-esp32p4
  KELF=target/kernel-esp32p4/$TARGET/release/lichen-kernel
  cp "$KELF" "$OUT/kernel.elf"
  "$ESPTOOL" --chip esp32p4 elf2image --min-rev-full 0 --max-rev-full 199 -o "$OUT/kernel.bin" "$OUT/kernel.elf" >/dev/null
  echo "kernel $LICHEN_BUILD_ID: $OUT/kernel.bin ($(stat -c %s "$OUT/kernel.bin") bytes)"
fi
