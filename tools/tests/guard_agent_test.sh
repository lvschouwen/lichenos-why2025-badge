#!/usr/bin/env bash
# Host test (#614): the guard hook lets a subagent or teammate (hook input with an agent_id) run tools/v3-load-ram.sh
# toward the badge and nothing else (every other badge.sh verb, ssh, scp, esptool are denied), while the main session
# (no agent_id) is judged exactly as before: badge.sh verbs pass there.
set -uo pipefail
here=$(cd "$(dirname "$0")" && pwd)
hook="$here/../../.claude/hooks/guard-badge.sh"
tmp=$(mktemp -d) && trap 'rm -rf "$tmp"' EXIT
printf '# test node\nTESTNODE\nscope #ZZ-Bench\n' > "$tmp/allow"
fail=0
decision() { # $1 = command, $2 = agent id (empty for main); prints deny, ask or pass
  local out
  out=$(jq -cn --arg c "$1" --arg a "$2" '{tool_input:{command:$c},cwd:"/"} + (if $a == "" then {} else {agent_id:$a} end)' |
    ONAIR_ALLOW_FILE="$tmp/allow" bash "$hook")
  [ -n "$out" ] || { echo pass; return; }
  jq -r '.hookSpecificOutput.permissionDecision' <<<"$out"
}
expect() { # $1 = expected, $2 = command, $3 = agent id (default: an agent)
  local got
  got=$(decision "$2" "${3-agent-1}")
  [ "$got" = "$1" ] || { echo "FAIL: '$2' (agent '${3-agent-1}'): $got, expected $1" >&2; fail=1; }
}
# Agents: the wrapper passes, in each way of writing it
expect pass "tools/v3-load-ram.sh firmware/v3/build/kernel.elf"
expect pass "./tools/v3-load-ram.sh kernel.elf --timeout 30 --follow"
expect pass "bash tools/v3-load-ram.sh kernel.bin"
expect pass "BADGE_PORT=/dev/ttyACM0 tools/v3-load-ram.sh kernel.bin"
expect pass "cd /x && /home/u/repo/tools/v3-load-ram.sh kernel.bin 2>&1 | tail -n 40"
# Agents: everything else toward the badge is denied
for verb in "load-ram /dev/ttyACM0 k.bin" "flash-fw /dev/ttyACM0" "ota-firmware 192.0.2.1" "console /dev/ttyACM0 x" \
  "restore /dev/ttyACM0 f.bin --yes" "backup /dev/ttyACM0" "ports" "monitor /dev/ttyACM0" "crash /dev/ttyACM0" \
  "fw 192.0.2.1 meshcore mesh out" "logger install"; do
  expect deny "tools/badge.sh $verb"
  expect deny "bash tools/badge.sh $verb"
  expect deny "cd /x; ./tools/badge.sh $verb"
done
expect deny "tools/badge.sh"
expect deny "FOO=1 /abs/badge.sh ports"
expect deny "tools/v3-load-ram.sh k.bin; tools/badge.sh flash-fw /dev/ttyACM0"
expect deny "tools/v3-load-ram.sh k.bin && ssh host ls"
expect deny "ssh host 'ls'"
expect deny "scp k.bin host:/tmp"
expect deny "esptool --port /dev/ttyACM0 chip-id"
expect deny "esptool.py write-flash 0 x.bin"
expect deny "echo hi | env ssh host"
# Agents: text that only names these stays fine (grep, git, reading)
expect pass "grep -n load-ram tools/badge.sh"
expect pass "cat tools/badge.sh"
expect pass "git commit -m 'feat(tools): badge.sh load-ram takes --baud (#614)'"
expect pass "ls tools"
# Main: no agent_id, unchanged
expect pass "tools/badge.sh load-ram /dev/ttyACM0 k.bin" ""
expect pass "tools/badge.sh flash-fw /dev/ttyACM0" ""
expect pass "ssh host ls" ""
expect pass "tools/v3-load-ram.sh k.bin" ""
expect ask "tools/badge.sh flash-fw /dev/ttyACM0 --wipe-storage" ""
expect ask "tools/badge.sh restore /dev/ttyACM0 f.bin --yes" ""
expect deny "tools/badge.sh fw 192.0.2.1 mesh send Public hi" ""
[ "$fail" = 0 ] && echo "guard_agent_test: ok"
exit "$fail"
