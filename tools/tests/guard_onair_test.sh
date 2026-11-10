#!/usr/bin/env bash
# Host test (#287): the guard hook refuses on-air console commands to anything but #badgetest and the allow-listed
# test node, and leaves source greps and read-only mesh commands alone. #290: the console scope (the allow list's
# "scope" line, a made-up region here) gates "mesh scope console" and, in badge.sh, every on-air line with a target.
set -uo pipefail
here=$(cd "$(dirname "$0")" && pwd)
hook="$here/../../.claude/hooks/guard-badge.sh"
tmp=$(mktemp -d) && trap 'rm -rf "$tmp"' EXIT
printf '# test node\nTESTNODE\nabcdef123456\nscope #ZZ-Bench\n' > "$tmp/allow"
printf 'scope\nTESTNODE\n' > "$tmp/allow-bare"
# #303: several exact scope lines; a wildcard and "*" (unscoped on the badge) never count
printf 'TESTNODE\nscope zz-one\nscope zz-*\nscope *\nscope #ZZ-Two\n' > "$tmp/allow-multi"
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
# The console scope (#290): read freely, set only to the allow list's scope line
expect pass "$b mesh scope"
expect pass "$b mesh scope console zz-bench YES"
expect pass "$b mesh scope console '#ZZ-Bench' YES"
expect deny "$b mesh scope console zz-other YES"
expect deny "$b mesh scope console"
expect deny "$b mesh scope default zz-bench"
expect deny "$b mesh scope console zz-bench YES" "$tmp/missing"
expect deny "$b mesh scope console zz-bench YES" "$tmp/allow-bare"
expect pass "$b mesh scope console zz-one YES" "$tmp/allow-multi"
expect pass "$b mesh scope console zz-two YES" "$tmp/allow-multi"
expect deny "$b mesh scope console zz-three YES" "$tmp/allow-multi"
expect deny "$b mesh scope console 'zz-*' YES" "$tmp/allow-multi"
expect deny "$b mesh scope console '*' YES" "$tmp/allow-multi"
# A scope line is never a contact target
expect deny "$b mesh dm scope hi" "$tmp/allow-bare"
expect deny "$b mesh remote scope status"
expect deny "$b mesh remote settings login"
expect pass "$b mesh remote TESTNODE settings"
expect pass "$b mesh remote TESTNODE status" "$tmp/allow-bare"
# The status history (#290): its polls are console floods, so its target must be an allowed node
expect pass "$b mesh history"
expect pass "$b mesh history TESTNODE on 60 YES"
expect deny "$b mesh history OTHER-RPT on 60 YES"
expect deny "$b mesh history 0123456789ab interval 10"
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
# badge.sh reads only the allow file of its own tree (no override), so it runs from a copy with the test's list
mkdir -p "$tmp/bin" "$tmp/repo/tools" "$tmp/repo/.claude"
# The fake answers the scope query with FAKE_SCOPE (#290), prints SENT for anything else, and logs every line
cat > "$tmp/bin/curl" <<'SH'
#!/bin/sh
body=
for a in "$@"; do case $a in @*) body=$(cat "${a#@}") ;; esac; done
printf '%s\n' "$body" >> "$FAKE_LOG"
case $body in
  "mesh scope" | "fw mesh scope") [ -n "${FAKE_SCOPE-}" ] && printf '%s\r\n' "$FAKE_SCOPE" ;;
  *) echo SENT ;;
esac
SH
chmod +x "$tmp/bin/curl"
cp "$here/../badge.sh" "$here/../onair_guard.sh" "$here/../elf_archive.sh" "$tmp/repo/tools/"
cp "$tmp/allow" "$tmp/repo/.claude/onair-allow.local"
# A tree whose allow list has no scope line
mkdir -p "$tmp/bare/tools" "$tmp/bare/.claude"
cp "$here/../badge.sh" "$here/../onair_guard.sh" "$here/../elf_archive.sh" "$tmp/bare/tools/"
grep -v '^scope' "$tmp/allow" > "$tmp/bare/.claude/onair-allow.local"
export FAKE_LOG="$tmp/sent.log"
export FAKE_SCOPE="default None | console zz-bench"
printf 'EVIL-NODE\n' > "$tmp/evil"
badge() { # $@ = badge.sh arguments; prints its output, returns its exit code. BADGE_REPO picks the tree.
  PATH="$tmp/bin:$PATH" BADGE_OTA_TOKEN=0123456789abcdef0123456789abcdef \
    BADGE_HOST=none bash "${BADGE_REPO:-$tmp/repo}/tools/badge.sh" "$@" 2>&1
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
# The console scope (#290): an on-air line with a target goes only when the badge's console scope is the scope line
sent sent fw 192.0.2.1 mesh remote TESTNODE status
sent sent fw 192.0.2.1 fw mesh send '#badgetest' hi
FAKE_SCOPE="default zz-wide | console ZZ-Bench" sent sent fw 192.0.2.1 mesh trace abcdef123456
FAKE_SCOPE="default None | console not set" sent refused fw 192.0.2.1 mesh send '#badgetest' hello
FAKE_SCOPE="default zz-bench | console zz-other" sent refused fw 192.0.2.1 mesh dm abcdef123456 hi
FAKE_SCOPE="" sent refused fw 192.0.2.1 mesh telemetry abcdef123456 YES
BADGE_REPO="$tmp/bare" sent refused fw 192.0.2.1 mesh send '#badgetest' hello
FAKE_SCOPE="" sent sent fw 192.0.2.1 mesh out
FAKE_SCOPE="" sent sent fw 192.0.2.1 mesh remote settings
# A remote settings read logs in and asks on air: the scope check applies; "settings" with more words is a target
FAKE_SCOPE="" sent refused fw 192.0.2.1 mesh remote TESTNODE settings
sent sent fw 192.0.2.1 mesh remote TESTNODE settings
sent refused fw 192.0.2.1 mesh remote settings login
# Only one exact status line speaks for the badge: a message logged around the reply cannot spoof it
FAKE_SCOPE="chat: default None | console zz-bench" sent refused fw 192.0.2.1 mesh send '#badgetest' hi
FAKE_SCOPE="default None | console zz-bench x" sent refused fw 192.0.2.1 mesh send '#badgetest' hi
FAKE_SCOPE=$'default None | console zz-bench\ndefault None | console zz-bench' sent refused fw 192.0.2.1 mesh send '#badgetest' hi
FAKE_SCOPE=$'chat: hello\ndefault None | console zz-bench\nI (123) mesh: x' sent sent fw 192.0.2.1 mesh send '#badgetest' hi
FAKE_SCOPE=$'default None | console zz-bench | console zz-other' sent refused fw 192.0.2.1 mesh send '#badgetest' hi
# The USB console prefixes every reply line with "fw: " (#301): exactly one such prefix counts, nothing else does
FAKE_SCOPE="fw: default None | console zz-bench" sent sent fw 192.0.2.1 mesh send '#badgetest' hi
FAKE_SCOPE="fw: default None | console zz-other" sent refused fw 192.0.2.1 mesh send '#badgetest' hi
FAKE_SCOPE="fw: fw: default None | console zz-bench" sent refused fw 192.0.2.1 mesh send '#badgetest' hi
FAKE_SCOPE="fw: chat: default None | console zz-bench" sent refused fw 192.0.2.1 mesh send '#badgetest' hi
FAKE_SCOPE=$'fw: default None | console zz-bench\ndefault None | console zz-bench' sent refused fw 192.0.2.1 mesh send '#badgetest' hi
sent refused fw 192.0.2.1 mesh scope console zz-other YES
sent sent fw 192.0.2.1 mesh history TESTNODE on 60 YES
# #303: a tree whose allow file has several scope lines: any exact one passes, the wildcard and "*" lines don't
mkdir -p "$tmp/multi/tools" "$tmp/multi/.claude"
cp "$here/../badge.sh" "$here/../onair_guard.sh" "$here/../elf_archive.sh" "$tmp/multi/tools/"
cp "$tmp/allow-multi" "$tmp/multi/.claude/onair-allow.local"
BADGE_REPO=$tmp/multi FAKE_SCOPE="default None | console zz-two" sent sent fw 192.0.2.1 mesh send '#badgetest' hi
BADGE_REPO=$tmp/multi FAKE_SCOPE="default None | console zz-one" sent sent fw 192.0.2.1 mesh send '#badgetest' hi
BADGE_REPO=$tmp/multi FAKE_SCOPE="default None | console zz-x" sent refused fw 192.0.2.1 mesh send '#badgetest' hi
BADGE_REPO=$tmp/multi FAKE_SCOPE="default None | console *" sent refused fw 192.0.2.1 mesh send '#badgetest' hi
FAKE_SCOPE="default None | console not set" sent refused fw 192.0.2.1 mesh history TESTNODE on 60 YES
sent refused fw 192.0.2.1 mesh history OTHER-RPT on 60 YES
FAKE_SCOPE="" sent sent fw 192.0.2.1 mesh history
sent sent fw 192.0.2.1 advert flood
FAKE_SCOPE="default None | console not set" sent refused fw 192.0.2.1 advert flood
FAKE_SCOPE="default None | console not set" sent refused fw 192.0.2.1 fw advert
: > "$FAKE_LOG"
sent sent fw 192.0.2.1 mesh scope console zz-bench YES
sent sent fw 192.0.2.1 mesh dm abcdef123456 hi
printf 'mesh scope console zz-bench YES\nmesh scope\nmesh dm abcdef123456 hi\n' | cmp -s - "$FAKE_LOG" ||
  { echo "FAIL: badge.sh sent $(tr '\n' '|' < "$FAKE_LOG"), expected the set, then the query before the dm" >&2; fail=1; }
# Both ways of replacing the flash drive ask (#183, #300: Lucas's call); a plain flash-fw keeps the drive and passes
f="tools/badge.sh flash-fw /dev/ttyUSB1"
expect ask "$f --wipe-storage"
expect ask "$f --storage-image /tmp/storage.bin"
expect ask "BADGE_EXTRA_PORTS=/dev/ttyUSB1 $f --storage-image ./restore.bin /tmp/build"
expect pass "$f"
# A caller's environment cannot swap the list
for var in ONAIR_ALLOW_FILE onair_allow_override; do
  out=$(env "$var=$tmp/evil" PATH="$tmp/bin:$PATH" BADGE_OTA_TOKEN=0123456789abcdef0123456789abcdef BADGE_HOST=none \
    bash "$tmp/repo/tools/badge.sh" fw 192.0.2.1 mesh dm EVIL-NODE hi 2>&1)
  grep -q '^refusing: ' <<<"$out" || { echo "FAIL: $var swapped the allow list: $out" >&2; fail=1; }
done
[ "$fail" = 0 ] && echo "guard_onair_test: ok"
exit "$fail"
