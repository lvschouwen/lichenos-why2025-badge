#!/usr/bin/env bash
# Host test (#287): the guard hook refuses on-air console commands to anything but #badgetest and the allow-listed
# test node, and leaves source greps and read-only mesh commands alone.
set -uo pipefail
here=$(cd "$(dirname "$0")" && pwd)
hook="$here/../../.claude/hooks/guard-badge.sh"
tmp=$(mktemp -d) && trap 'rm -rf "$tmp"' EXIT
printf '# test node\nTESTNODE\nabcdef123456\n' > "$tmp/allow"
fail=0
decision() { # $1 = command, $2 = allow file; prints deny, ask or pass
  local out
  out=$(jq -cn --arg c "$1" '{tool_input:{command:$c},cwd:"/"}' | ONAIR_ALLOW_FILE="${2:-$tmp/allow}" bash "$hook")
  [ -n "$out" ] || { echo pass; return; }
  jq -r '.hookSpecificOutput.permissionDecision' <<<"$out"
}
expect() { # $1 = expected, $2 = command, $3 = allow file (optional)
  local got; got=$(decision "$2" "${3:-}")
  [ "$got" = "$1" ] || { echo "FAIL: '$2': $got, expected $1" >&2; fail=1; }
}
b="tools/badge.sh fw 192.0.2.1"
expect pass "$b mesh send '#badgetest' hello there"
expect pass "$b mesh send \"#badgetest\" hi"
expect deny "$b mesh send Public hello"
expect deny "$b mesh send badgetest hello"
expect deny "ssh pi 'tools/badge.sh console /dev/ttyACM0 \"fw mesh send #general hi\"'"
expect deny "$b mesh send '#badgetest' ok; $b mesh send '#other' no"
expect pass "$b mesh dm abcdef123456 ping"
expect pass "$b mesh dm ABCDEF123456 ping"
expect deny "$b mesh dm 0123456789ab ping"
expect pass "$b mesh remote TESTNODE status"
expect deny "$b mesh remote OTHER-RPT cli reboot YES"
expect pass "$b mesh remote"
expect pass "$b mesh remote settings"
expect deny "$b mesh telemetry 0123456789ab YES"
expect pass "$b mesh telemetry"
expect deny "$b mesh trace 0123456789ab"
expect deny "$b mesh discover 0123456789ab"
expect pass "$b mesh trace"
expect pass "$b mesh advert"
expect deny "$b mesh advert interval 6h"
expect pass "$b mesh out"
expect pass "grep -n 'mesh send Public' firmware/components/mesh/mesh_task.cpp"
expect pass "git commit -m 'guard-badge.sh denies fw mesh send to Public
lines that reach the badge (badge.sh, ssh) are checked'"
expect deny "$b mesh dm \\
  0123456789ab ping"
expect deny "curl -s -X POST --data-binary 'mesh send Public hi' http://192.0.2.1:8080/console"
expect deny "echo x >> .claude/onair-allow.local"
got=$(jq -cn '{tool_name:"Edit",tool_input:{file_path:"/r/.claude/onair-allow.local"}}' | bash "$hook" |
  jq -r '.hookSpecificOutput.permissionDecision')
[ "$got" = deny ] || { echo "FAIL: Edit of the allow list: $got, expected deny" >&2; fail=1; }
# Without an allow file only #badgetest passes
expect deny "$b mesh dm abcdef123456 hi" "$tmp/missing"
expect pass "$b mesh send '#badgetest' hi" "$tmp/missing"
# badge.sh applies the same rules to every line it sends, whatever the command looked like: a fake curl records
# what would have gone out
mkdir -p "$tmp/bin"
printf '#!/bin/sh\necho SENT\n' > "$tmp/bin/curl" && chmod +x "$tmp/bin/curl"
badge() { # $@ = badge.sh arguments; prints its output, returns its exit code
  PATH="$tmp/bin:$PATH" ONAIR_ALLOW_FILE="$tmp/allow" BADGE_OTA_TOKEN=0123456789abcdef0123456789abcdef \
    BADGE_HOST=none bash "$here/../badge.sh" "$@" 2>&1
}
sent() { # $1 = expected (sent|refused), rest = badge.sh arguments
  local want=$1 out rc; shift
  out=$(badge "$@"); rc=$?
  if [ "$want" = sent ]; then
    if [ "$rc" != 0 ] || ! grep -q SENT <<<"$out"; then echo "FAIL: badge.sh $*: not sent: $out" >&2; fail=1; fi
  elif [ "$rc" = 0 ] || grep -q SENT <<<"$out" || ! grep -q '^refusing: ' <<<"$out"; then
    echo "FAIL: badge.sh $*: not refused ($rc): $out" >&2; fail=1
  fi
}
sent sent fw 192.0.2.1 mesh send '#badgetest' hello
sent sent fw 192.0.2.1 mesh dm abcdef123456 hi
sent sent fw 192.0.2.1 mesh out
sent refused fw 192.0.2.1 mesh send Public hello
sent refused fw 192.0.2.1 "mesh send #other hi"
sent refused fw 192.0.2.1 mesh dm 0123456789ab hi
sent refused fw 192.0.2.1 mesh remote OTHER-RPT cli reboot YES
sent refused fw 192.0.2.1 mesh advert interval 6h
sent refused console /dev/null "fw mesh send Public hi"
sent sent fw 192.0.2.1 mesh advert
[ "$fail" = 0 ] && echo "guard_onair_test: ok"
exit "$fail"
