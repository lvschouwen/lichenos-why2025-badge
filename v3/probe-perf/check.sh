#!/usr/bin/env bash
# The gate of the #648 perf lab, standalone: v3/check.sh names its packages, so it does not format, test, lint or build
# lichen-perflab and lichen-probe-perf. Run it from anywhere:   v3/probe-perf/check.sh
# (one line in v3/check.sh runs it: `./probe-perf/check.sh`, after the "release build: probe-psram" step).
#   fmt, host tests of the maths, clippy on both targets, a release image of every switch value, the refused combinations,
#   and the --ram-text image check on each. Needs the IDF esptool for the images (see build.sh).
set -euo pipefail
cd "$(dirname "$0")/.."
HOST=x86_64-unknown-linux-gnu
TARGET=riscv32imac-unknown-none-elf
step() { printf '\n== probe-perf: %s\n' "$*"; }
export CARGO_TERM_COLOR=never

step "rustfmt"
cargo fmt -p lichen-perflab -p lichen-probe-perf -- --check

step "host tests (perflab maths)"
cargo test --locked -q -p lichen-perflab --target $HOST

step "clippy -D warnings"
CL=(cargo clippy --locked -q --target-dir target/clippy)
"${CL[@]}" -p lichen-perflab --all-targets --target $HOST -- -D warnings
"${CL[@]}" -p lichen-probe-perf --target $TARGET -- -D warnings

step "images: every switch value builds and keeps its cache code in RAM"
# one switch at a time from the default, plus the ends of the matrix
variants=(
  ""
  "--cpu 400"
  "--psram 160"
  "--cpu 400 --psram 160"
  "--l2kb 128 --l2line 64"
  "--l2kb 128 --l2line 128"
  "--l2kb 256 --l2line 128"
  "--l2kb 512 --l2line 128"
  "--dma 64"
  "--dma 128"
  "--ldo 1900"
)
for v in "${variants[@]}"; do
  # shellcheck disable=SC2086
  out=$(./probe-perf/build.sh $v --suite config)
  echo "$out"
  elf=${out#*: }
  elf=${elf%.bin *}.elf
  python3 -I tools/check_image.py --ram-text "$elf"
done

step "images: impossible combinations are refused at build time"
refused=(
  "--psram 200:200 MHz waits for #650"
  "--psram 80:impossible"
  "--cpu 440:impossible"
  "--l2kb 512 --l2line 64:impossible"
  "--l2kb 64:impossible"
  "--ldo 1950:must be cal or 1900"
  "--dma 16:must be 32, 64 or 128"
)
for r in "${refused[@]}"; do
  args=${r%%:*}
  want=${r#*:}
  # shellcheck disable=SC2086
  if log=$(./probe-perf/build.sh $args --suite config 2>&1); then
    echo "built, but must be refused: $args" >&2
    exit 1
  fi
  grep -q "$want" <<<"$log" || { echo "refused, but not with '$want': $args" >&2; echo "$log" >&2; exit 1; }
  echo "refused: $args"
done

step "the default image, restored"
./probe-perf/build.sh --suite config >/dev/null
printf '\nv3/probe-perf/check.sh: OK\n'
