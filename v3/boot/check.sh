#!/usr/bin/env bash
# The v3 boot stage gate (#615): fmt, clippy -D warnings, host tests (proptest), the stage build, the real packer's
# output verified by the core parser, the 24 KB size gate and the licence check. Standalone: main wires it into
# v3/check.sh at merge. Never `--workspace` for the target build (boot-core is host code).
set -euo pipefail
cd "$(dirname "$0")"
HOST=x86_64-unknown-linux-gnu
TARGET=riscv32imac-unknown-none-elf
STAGE_LIMIT=24576 # the ROM reads the stage from flash 0x2000..0x7FFF
step() { printf '\n== %s\n' "$*"; }
export CARGO_TERM_COLOR=never

step "rustfmt"
cargo fmt --all -- --check

step "host tests (boot-core; proptest)"
cargo test --locked -q -p boot-core --target $HOST

step "clippy -D warnings"
CL=(cargo clippy --locked -q --target-dir target/clippy)
"${CL[@]}" -p boot-core --all-targets --target $HOST -- -D warnings
"${CL[@]}" -p boot-esp32p4 --target $TARGET -- -D warnings

step "stage build (esp32p4, release)"
cargo build --locked -q --release -p boot-esp32p4 --target $TARGET --target-dir target/stage
ELF=target/stage/$TARGET/release/boot-esp32p4
OUT=target/stage/stage.bin
./pack.sh "$ELF" "$OUT"

step "the real packer's image passes the core parser"
cargo run --locked -q -p boot-core --example verify --target $HOST -- "$OUT"

step "size gate (<= $STAGE_LIMIT bytes, the 0x2000..0x7FFF slot)"
SIZE=$(stat -c %s "$OUT")
echo "stage image: $SIZE bytes (limit $STAGE_LIMIT)"
[ "$SIZE" -le "$STAGE_LIMIT" ] || { echo "stage image too large" >&2; exit 1; }
SYSROOT_BIN=$(rustc --print sysroot)/lib/rustlib/$HOST/bin
"$SYSROOT_BIN/llvm-size" "$ELF"

step "licences (MIT or Apache-2.0 only)"
cargo metadata --locked --format-version 1 | python3 -I ../tools/check_licences.py --stdin

printf '\nv3/boot/check.sh: OK\n'
