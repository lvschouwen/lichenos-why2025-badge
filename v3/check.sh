#!/usr/bin/env bash
# The v3 gate (#611): fmt, clippy -D warnings, host tests (proptest), release builds for qemu-virt and esp32p4, the
# image check, the QEMU harness, a size report and the licence check. Run by `agent-check` (CHECK=./v3/check.sh).
# Never `--workspace`: feature unification would switch on lichen-abi/rt for the kernel (see build.sh).
set -euo pipefail
cd "$(dirname "$0")"
HOST=x86_64-unknown-linux-gnu
TARGET=riscv32imac-unknown-none-elf
SYSROOT_BIN=$(rustc --print sysroot)/lib/rustlib/$HOST/bin
step() { printf '\n== %s\n' "$*"; }
export CARGO_TERM_COLOR=never

# --all covers every workspace member; v3/boot is its own workspace and runs fmt in boot/check.sh.
step "rustfmt"
cargo fmt --all -- --check

step "host tests (kcore, abi; proptest; platform register maths)"
cargo test --locked -q -p lichen-kcore -p lichen-abi -p lichen-platform-esp32p4 --target $HOST

step "python tests (bench report, image check, licence check)"
python3 -I -m unittest discover -s tools -p 'test_*.py'

step "release build: qemu-virt (apps first, then the kernel)"
./build.sh qemu-virt
step "release build: esp32p4 (RAM-only bring-up image)"
./build.sh esp32p4
step "release build: probe-p4 (unchanged PoC, must keep building)"
LICHEN_PROBE_ID=check cargo build --locked -q --release -p lichen-probe-p4 --target $TARGET --target-dir target/probe-p4

step "release build: probe-psram (#620, stage all)"
LICHEN_PSRAM_STAGE=all cargo build --locked -q --release -p lichen-probe-psram --target $TARGET --target-dir target/probe-psram

step "perf lab (#648): perflab + probe-perf, own gate"
./probe-perf/check.sh

step "clippy -D warnings"
CL=(cargo clippy --locked -q --target-dir target/clippy)
"${CL[@]}" -p lichen-kcore -p lichen-abi --all-targets --target $HOST -- -D warnings
"${CL[@]}" -p lichen-platform-esp32p4 --target $TARGET -- -D warnings
"${CL[@]}" -p lichen-platform-esp32p4 --all-targets --target $HOST -- -D warnings
"${CL[@]}" -p lichen-platform-esp32p4 --features bench-experiments --target $TARGET -- -D warnings
"${CL[@]}" -p lichen-platform-esp32p4 --features bench-experiments --all-targets --target $HOST -- -D warnings
"${CL[@]}" -p lichen-probe-psram --target $TARGET -- -D warnings
for app in heartbeat attacker; do
  LICHEN_APP_LD=$PWD/target/lichen/qemu-virt/$app.ld "${CL[@]}" -p $app --target $TARGET -- -D warnings
done
for app in worker rogue; do
  LICHEN_APP_LD=$PWD/target/lichen/esp32p4/$app.ld "${CL[@]}" -p $app --target $TARGET -- -D warnings
done
LICHEN_APP_HEARTBEAT=$PWD/target/lichen/qemu-virt/heartbeat.bin LICHEN_APP_ATTACKER=$PWD/target/lichen/qemu-virt/attacker.bin \
  "${CL[@]}" -p lichen-kernel --target $TARGET --no-default-features --features qemu-virt -- -D warnings
"${CL[@]}" -p lichen-kernel --target $TARGET --no-default-features --features esp32p4 -- -D warnings

step "bench-experiments stays out of the kernel"
tree=$(cargo tree --locked -p lichen-kernel --no-default-features --features esp32p4 -e features)
probe_tree=$(cargo tree --locked -p lichen-probe-psram -e features)
if ! grep -q 'feature "bench-experiments"' <<<"$probe_tree"; then
  echo "guard self-test failed: the probe's tree does not show bench-experiments" >&2
  exit 1
fi
if grep -q 'feature "bench-experiments"' <<<"$tree"; then
  echo "bench-experiments is enabled in the kernel's dependency tree" >&2
  exit 1
fi

step "image check (RAM-only, inside the limits)"
python3 -I tools/check_image.py target/lichen/esp32p4/kernel.elf target/lichen/esp32p4/kernel.bin
# The kernel uses psram::map since #621: its cache sync must stay in internal RAM too.
python3 -I tools/check_image.py --ram-text target/lichen/esp32p4/kernel.elf
python3 -I tools/check_image.py --ram-text target/probe-psram/$TARGET/release/lichen-probe-psram

step "QEMU harness (21 isolation cases)"
QLOG=target/qemu-run.log
set +e
timeout 60 qemu-system-riscv32 -machine virt -bios none -nographic -m 128M \
  -kernel target/kernel-qemu-virt/$TARGET/release/lichen-kernel > $QLOG 2>&1
QRC=$?
set -e
grep -E '^(TEST|SUMMARY)' $QLOG | tail -3
if [ $QRC -ne 0 ] || ! grep -q '^SUMMARY: 21 pass, 0 fail' $QLOG; then
  echo "QEMU harness failed (exit $QRC); full log in v3/$QLOG" >&2
  exit 1
fi

step "size report"
for img in target/lichen/esp32p4/kernel.elf target/kernel-qemu-virt/$TARGET/release/lichen-kernel; do
  echo "$img"
  "$SYSROOT_BIN/llvm-size" "$img"
done
echo "esp32p4 loaded image: $(stat -c %s target/lichen/esp32p4/kernel.bin) bytes (limit 0x2ABD0 = 175056, #658)"

step "licences (MIT or Apache-2.0 only)"
python3 -I tools/check_licences.py

step "boot stage (#615): its own workspace and gate"
./boot/check.sh

printf '\nv3/check.sh: OK\n'
