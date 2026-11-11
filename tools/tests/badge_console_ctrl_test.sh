#!/usr/bin/env bash
# Host test (#324): badge.sh refuses a console line with a control character before its on-air gate reads it, on the
# USB console and on Wi-Fi ("se^And" is not "send" to the gate, and the logger used to strip the ^A). No badge host is
# host and no update token are configured here, and ssh/scp/curl are stubs, so nothing ever leaves this machine.
set -uo pipefail
here=$(cd "$(dirname "$0")" && pwd)
cfg=$(mktemp -d) && trap 'rm -rf "$cfg"' EXIT
mkdir "$cfg/bin"
for tool in ssh scp curl; do printf '#!/bin/sh\necho "REACHED %s"\nexit 1\n' "$tool" > "$cfg/bin/$tool"; done
chmod +x "$cfg"/bin/*
fail=0
run() {
    env -u BADGE_HOST -u BADGE_OTA_TOKEN PATH="$cfg/bin:$PATH" XDG_CONFIG_HOME="$cfg" bash "$here/../badge.sh" "$@" 2>&1
}
expect_refused() { # $1 = what, the rest: badge.sh arguments
    local what=$1 out rc
    shift
    out=$(run "$@"); rc=$?
    if [ "$rc" != 1 ] || grep -q REACHED <<<"$out" || ! grep -q '^refusing: control characters in a console line' <<<"$out"; then
        echo "FAIL: $what: exit $rc: $out" >&2
        fail=1
    fi
}
port=/dev/ttyUSB9
expect_refused "console ^A" console "$port" $'fw mesh se\x01nd #other hi'
expect_refused "console ESC" console "$port" $'fw \x1b[2J mem'
expect_refused "console DEL" console "$port" $'fw mem\x7f'
expect_refused "fw ^A" fw 192.0.2.1 mesh $'se\x01nd' '#other' hi
expect_refused "fw trailing newline" fw 192.0.2.1 $'mem\n'
# A tab passes the refusal: the line goes on to the host lookup, which fails here
out=$(run console "$port" $'fw get\tbl.')
grep -q 'no badge host' <<<"$out" || { echo "FAIL: a tab was refused: $out" >&2; fail=1; }
[ "$fail" = 0 ] && echo "badge_console_ctrl_test: ok"
exit "$fail"
