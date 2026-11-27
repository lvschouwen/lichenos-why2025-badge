#!/usr/bin/env bash
# Host test (#614): the guard hook lets a subagent or teammate (hook input with an agent_id) run ONLY the main
# checkout's tools/v3-load-ram.sh toward the badge, as one plain command. Every other badge.sh verb, ssh, scp, esptool,
# a slot's own copy of the wrapper and any injection around the wrapper are denied. The main session (no agent_id) is
# judged exactly as before: badge.sh verbs pass there.
set -uo pipefail
here=$(cd "$(dirname "$0")" && pwd)
tmp=$(mktemp -d) && trap 'rm -rf "$tmp"' EXIT
printf '# test node\nTESTNODE\nscope #ZZ-Bench\n' > "$tmp/allow"
# A main checkout and a slot (a git worktree of it), as the agents see them
main=$tmp/main slot=$tmp/main/.claude/worktrees/wt-1
git init -q "$main" && git -C "$main" -c user.name=t -c user.email=t@example.invalid commit -q --allow-empty -m init
git -C "$main" worktree add -q "$slot" -b slotbranch
# the hook anchors trust on its own checkout, so it runs from a copy inside the fake main checkout
mkdir -p "$main/.claude/hooks" "$main/tools"
cp "$here/../../.claude/hooks/guard-badge.sh" "$main/.claude/hooks/"
cp "$here/../onair_guard.sh" "$main/tools/"
hook=$main/.claude/hooks/guard-badge.sh
# an agent's own repo with its own wrapper copy
evil=$tmp/evil
git init -q "$evil" && mkdir -p "$evil/tools"
W=$main/tools/v3-load-ram.sh
fail=0
decision() { # $1 = command, $2 = agent id (empty for main), $3 = cwd; prints deny, ask or pass
  local out
  out=$(jq -cn --arg c "$1" --arg a "$2" --arg d "$3" \
    '{tool_input:{command:$c},cwd:$d} + (if $a == "" then {} else {agent_id:$a} end)' |
    ONAIR_ALLOW_FILE="$tmp/allow" bash "$hook")
  [ -n "$out" ] || { echo pass; return; }
  jq -r '.hookSpecificOutput.permissionDecision' <<<"$out"
}
expect() { # $1 = expected, $2 = command, $3 = agent id (default: an agent), $4 = cwd (default: the slot)
  local got
  got=$(decision "$2" "${3-agent-1}" "${4-$slot}")
  [ "$got" = "$1" ] || { echo "FAIL: '$2' (agent '${3-agent-1}'): $got, expected $1" >&2; fail=1; }
}
# Agents: the main checkout's wrapper, plain arguments only
expect pass "$W $slot/kernel.elf"
expect pass "$W $slot/kernel.elf --timeout 30 --follow"
expect pass "$W kernel.bin"
expect pass "$W $slot/a-b_c.d/k.bin   --timeout   5 "
expect pass "$W kernel.bin" agent-1 "$main"
# Agents: a slot's own copy, a relative path or any wrapping of the wrapper is denied
expect deny "$slot/tools/v3-load-ram.sh k.bin"
expect deny "tools/v3-load-ram.sh k.bin"
expect deny "./tools/v3-load-ram.sh k.bin"
expect deny "bash tools/v3-load-ram.sh k.bin"
expect deny "bash $W k.bin"
expect deny "env $W k.bin"
expect deny "timeout 20 $W k.bin"
expect deny "xargs $W k.bin"
expect deny "BADGE_PORT=/dev/ttyACM0 $W k.bin"
expect deny "cd /x && $W k.bin"
expect deny "bash -c '$W k.bin'"
expect pass "$W k.bin" agent-1 "$tmp"     # trust does not depend on the agent's cwd
expect deny "$evil/tools/v3-load-ram.sh k.bin" agent-1 "$evil"   # a repo the agent cd'd into is not trusted
expect deny "rm -f ~/badge-logs/bench.lock"
expect deny "touch /home/x/badge-logs/bench.lock"
expect pass "rm -f ~/badge-logs/bench.lock" ""   # main owns it
# Agents: injections after or around the wrapper are denied
expect deny "$W k.bin; ls"
expect deny "$W k.bin && ls"
expect deny "$W k.bin | tail -n 5"
expect deny "$W k.bin 2>&1"
expect deny "$W k.bin > /tmp/x"
expect deny "$W k.bin < /dev/null"
expect deny "$W \$(id)"
expect deny "$W \`id\`"
expect deny "$W 'k.bin'"
expect deny "$W (k.bin)"
expect deny "$W {a,b}"
expect deny "$W k.bin &"
expect deny "$W k.bin"$'\n'"ls"
expect deny "$W k.bin"$'\n'"$W k.bin"
expect deny "$W k.bin"$'\t'"--follow"
expect deny "ls"$'\n'"$W k.bin"
# Agents: everything else toward the badge is denied
for verb in "load-ram /dev/ttyACM0 k.bin" "flash-fw /dev/ttyACM0" "ota-firmware 192.0.2.1" "console /dev/ttyACM0 x" \
  "restore /dev/ttyACM0 f.bin --yes" "backup /dev/ttyACM0" "ports" "monitor /dev/ttyACM0" "crash /dev/ttyACM0" \
  "fw 192.0.2.1 meshcore mesh out" "logger install"; do
  expect deny "tools/badge.sh $verb"
  expect deny "bash tools/badge.sh $verb"
  expect deny "cd /x; ./tools/badge.sh $verb"
  expect deny "$main/tools/badge.sh $verb"
done
expect deny "tools/badge.sh"
expect deny "FOO=1 /abs/badge.sh ports"
expect deny "$W k.bin; tools/badge.sh flash-fw /dev/ttyACM0"
expect deny "$W k.bin && ssh host ls"
expect deny "ssh host 'ls'"
expect deny "scp k.bin host:/tmp"
expect deny "esptool --port /dev/ttyACM0 chip-id"
expect deny "esptool.py write-flash 0 x.bin"
expect deny "echo hi | env ssh host"
# Agents: text that only names these stays fine (grep, git, reading)
expect pass "grep -n load-ram tools/badge.sh"
expect pass "cat tools/badge.sh"
expect pass "cat tools/v3-load-ram.sh"
expect pass "git commit -m 'feat(tools): badge.sh load-ram takes --baud (#614)'"
expect pass "ls tools"
# Main: no agent_id, unchanged
expect pass "tools/badge.sh load-ram /dev/ttyACM0 k.bin" "" /
expect pass "tools/badge.sh flash-fw /dev/ttyACM0" "" /
expect pass "ssh host ls" "" /
expect pass "tools/v3-load-ram.sh k.bin" "" /
expect pass "bash tools/v3-load-ram.sh k.bin; ls" "" /
expect ask "tools/badge.sh flash-fw /dev/ttyACM0 --wipe-storage" "" /
expect ask "tools/badge.sh restore /dev/ttyACM0 f.bin --yes" "" /
expect deny "tools/badge.sh fw 192.0.2.1 mesh send Public hi" "" /
[ "$fail" = 0 ] && echo "guard_agent_test: ok"
exit "$fail"
