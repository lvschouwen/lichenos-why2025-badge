#!/usr/bin/env bash
# Host test for tools/check_release_image.sh (#401): its selftest on fakes, then the real CLI path with exit codes.
set -euo pipefail
tool=$(dirname "$0")/../check_release_image.sh
tmp=$(mktemp -d); trap 'rm -r "$tmp"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }
rc() { local r=0; bash "$tool" "$@" >/dev/null 2>&1 || r=$?; echo "$r"; }
expect() { local want=$1 got; shift; got=$(rc "$@"); case $want in
    0) [ "$got" = 0 ] ;; nz) [ "$got" != 0 ] ;; *) [ "$got" = "$want" ] ;; esac || fail "$* exited $got, wanted $want"; }

bash "$tool" --selftest >/dev/null
printf 'a\0Switch developer mode off?\0' > "$tmp/clean.bin"
printf 'a\0LICHEN-DEBUG-BUILD-MARKER\0' > "$tmp/marked.bin"
printf 'a\0fw dev on\0' > "$tmp/canary.bin"
printf '# CONFIG_LICHEN_DEV_COMMANDS is not set\n' > "$tmp/off.cfg"
printf 'CONFIG_LICHEN_NVS_ENCRYPT_BURN_CMD=y\n' > "$tmp/on.cfg"
expect 0 release "$tmp/clean.bin"
expect 1 release "$tmp/marked.bin"
expect 1 release "$tmp/canary.bin"
expect 0 dev "$tmp/marked.bin"
expect 1 dev "$tmp/clean.bin"
expect 1 release "$tmp/missing.bin"
expect 0 config "$tmp/off.cfg"
expect 1 config "$tmp/on.cfg"
expect 2 bogus "$tmp/clean.bin"
expect 2
expect 2 release
echo "check_release_image_test ok"
