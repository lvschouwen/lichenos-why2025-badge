#!/usr/bin/env bash
# PreToolUse (Bash) guard for Lichen OS work (#183).
# Irreversible, workflow-breaking or off-limits on-air commands get a prompt or a refusal instead of running silently:
#  1. `badge.sh flash-fw ... --wipe-storage` or `--storage-image <bin>` (#300): both replace the MeshCore identity,
#     contacts and channels.
#  2. `git checkout -b/-B`, `git switch`, `git reset` issued from the MAIN checkout (a resumed agent once
#     did this and turned the ff-merge into a silent no-op). Agents work in .claude/worktrees/*.
# "ask" prompts the user in the main session and stops a subagent, which must then report. It is held until every
# rule ran, so a deny anywhere in the command wins (#487). Fails open.
set -u
input=$(cat 2>/dev/null || true)
cmd=$(printf '%s' "$input" | jq -r '.tool_input.command // empty' 2>/dev/null || true)
cwd=$(printf '%s' "$input" | jq -r '.cwd // empty' 2>/dev/null || true)
# An ask is held until every rule has run (#487): a later deny in the same command still refuses it
pending_ask=
ask() {
  [ -n "$pending_ask" ] || pending_ask=$1
}
finish() {
  [ -z "$pending_ask" ] ||
    jq -cn --arg r "$pending_ask" '{hookSpecificOutput:{hookEventName:"PreToolUse",permissionDecision:"ask",permissionDecisionReason:$r}}'
  exit 0
}
deny() {
  jq -cn --arg r "$1" '{hookSpecificOutput:{hookEventName:"PreToolUse",permissionDecision:"deny",permissionDecisionReason:$r}}'
  exit 0
}
# The on-air allow list (rule 3) is editable from the main session (#487: one dev badge, the bench adds its test
# nodes); only what it lets on air is judged below.
[ -n "$cmd" ] || finish
# 0. Subagents and teammates (the hook input carries an agent_id) reach the badge only through the MAIN checkout's
#    tools/v3-load-ram.sh (#614: RAM-bounded, locked, no flash), and only as one plain invocation: its absolute path,
#    then arguments of [A-Za-z0-9_./+:=@%,-] separated by spaces. No separator, expansion, redirect, newline, wrapper
#    or variable prefix gets through, and a slot's own (editable) copy is refused. The main checkout is the git common
#    dir's parent, never the agent's slot. Everything else naming badge.sh, ssh, scp or esptool in command position
#    (after a separator, a quote or a wrapper word) is refused, so a grep or a commit message naming them passes.
#    Main has no agent_id and is judged by the rules below only.
agent_id=$(printf '%s' "$input" | jq -r '.agent_id // empty' 2>/dev/null || true)
if [ -n "$agent_id" ]; then
  common=$(git -C "${cwd:-.}" rev-parse --path-format=absolute --git-common-dir 2>/dev/null || true)
  trusted=
  case $common in /*/.git) trusted=${common%/.git}/tools/v3-load-ram.sh ;; esac
  wrapper_re='^(/[A-Za-z0-9_./-]+)( +[A-Za-z0-9_./+:=@%,-]+)* *$'
  if [ -n "$trusted" ] && [[ $cmd =~ $wrapper_re ]] && [ "${BASH_REMATCH[1]}" = "$trusted" ]; then
    finish
  fi
  cw="(^|[;&|(\`'\"{]|\\b(then|do|else|exec|command|env|sudo|time|xargs|nohup|bash|sh|source|timeout[[:space:]]+[0-9.]+[smh]?))[[:space:]]*([A-Za-z_][A-Za-z0-9_]*=[^[:space:]]*[[:space:]]+)*([^[:space:];&|'\"]*/)?"
  if printf '%s\n' "$cmd" | grep -qE "${cw}(badge\.sh|v3-load-ram\.sh|ssh|scp|esptool(\.py)?)([[:space:]]|\$)"; then
    deny "agents may run only the main checkout's tools/v3-load-ram.sh, as one plain command (no ; & | \$ \` ( ) { } < > or newline, no wrapper or variable prefix, not a slot copy) toward the badge; every other badge.sh verb, ssh and esptool is main's (#614)"
  fi
fi
if printf '%s' "$cmd" | grep -qE 'badge\.sh[^|;&]*flash-fw[^|;&]*--wipe-storage'; then
  ask "flash-fw --wipe-storage erases the MeshCore identity, contacts and channels on the badge. First-install only; needs Lucas."
fi
# 1b. `badge.sh flash-fw ... --storage-image <bin>` (#300) replaces the whole flash drive with <bin> the same way.
# Quotes and backslashes go first, as the shell drops them: 'restore' or re\store is the same verb
if printf '%s' "$cmd" | tr -d "'\"\\\\" | grep -qE 'badge\.sh[^|;&]*[[:space:]]restore([[:space:]]|$)'; then
  ask "badge.sh restore writes a backup image over the badge's flash drive (MeshCore identity, contacts, channels), also with --yes. Needs Lucas (#450)."
fi
if printf '%s' "$cmd" | grep -qE 'badge\.sh[^|;&]*flash-fw[^|;&]*--storage-image'; then
  ask "flash-fw --storage-image replaces everything on the badge's flash drive (MeshCore identity, contacts, channels) with the given image. The one-time LittleFS move (#300); needs Lucas."
fi
# 3. On air only #badgetest and the test node (#287), rules in tools/onair_guard.sh (badge.sh applies the same ones
#    to every line it sends). Here only lines that reach the badge (badge.sh fw|console, ssh, a tty, curl .../console)
#    are looked at, so a source grep or a commit message describing such a command passes.
#    ONAIR_ALLOW_FILE is for the host test: the hook's environment is Claude Code's, not the command's.
onair_allow_override=${ONAIR_ALLOW_FILE:-}
# shellcheck source=tools/onair_guard.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/tools/onair_guard.sh"
#    #450: also wget, python's urlopen and serial.Serial, the by-id serial names and the logger's console device and socket.
reach='(^|[^[:alnum:]_-])badge\.sh[[:space:]]+(fw|console)[[:space:]]|(^|[^[:alnum:]_-])ssh[[:space:]]|/dev/tty|/dev/serial/|/dev/badge-console|badge-console\.sock|curl[^|;&]*/console|(^|[^[:alnum:]_-])wget[[:space:]]|urlopen|serial\.Serial'
# #450: a body read from a file toward the badge's port never shows its console line to this hook (badge.sh's own
# sends are judged inside badge.sh): refused, whichever side of the URL the file stands on
# Fails closed: any '@', an upload or a config file in a curl toward the port, any file body in a wget toward it
# (curl's "--config -" with the token on stdin stays: badge.sh's own form)
while IFS= read -r seg; do
  case "$seg" in *:8080*) ;; *) continue ;; esac
  if printf '%s' "$seg" | grep -qE -- "^curl.*(@|[[:space:]]-[[:alnum:]]*T|--upload-file|[[:space:]]-[[:alnum:]]*K[[:space:]=]*[^-[:space:]]|--config[[:space:]=]+[^-[:space:]])|^wget.*--(post|body)-file"; then
    deny "a body or config from a file toward the badge's port: the console line in it cannot be judged. Use tools/badge.sh fw (#450)"
  fi
done < <(printf '%s\n' "$cmd" | grep -oE '(curl|wget)[^|;&]*' || true)
# Backslash-newline continuations join first, so a target on the next line is still seen
cmd_joined=$(printf '%s\n' "$cmd" | sed -e ':a' -e '/\\$/N' -e 's/\\\n/ /' -e 'ta')
# Never looser than main, and one console line must never hide the next. grep -o does not rescan text it consumed, so
# a verb's match (up to two words after it) could swallow the next verb whatever stands between them (a newline in
# any escape form: \n, \12, \xa, \u000a, %0a, a here-doc or a second line). So the text is judged twice and any one
# refusal refuses the whole command: (A) whole, as main did, so a target is never cut apart; (B) cut in front of every
# console verb, so no match can consume a later verb. The text is the command as written and with its
# backslash-newlines joined, from every line once any line reaches the badge. Limit: a payload encoded (base64, xxd)
# or built from pieces (echo -n) never shows its verb to a text hook (#440).
onair_alt='mesh[[:space:]]+(send|dm|remote|telemetry|trace|discover|history|advert|scope|radio|name|loc|path|pathhash|autoadd)([[:space:]]+[^[:space:];&|)]+){0,2}|(onair|channel|contacts|factory-reset|nvs-down|nvs-enc|c6)([[:space:]]+[^[:space:];&|)]+){0,2}'
onair_chat='mesh[[:space:]]+chat([[:space:]]+[^[:space:];&|)]+){0,2}' # #438: its own pass
onair_grep_both() {
  local line
  line=$(cat)
  printf '%s\n' "$line" | grep -oE "$onair_alt" || true
  printf '%s\n' "$line" | grep -oE "$onair_chat" || true
}
onair_hits() {
  local text=$1 piece
  printf '%s\n' "$text" | onair_grep_both
  while IFS= read -r piece; do
    printf '%s\n' "$piece" | onair_grep_both
  done < <(printf '%s\n' "$text" | sed -E 's/(mesh|onair|channel|contacts|factory-reset|nvs-down|nvs-enc|c6)[[:space:]]/\n&/g')
}
if printf '%s\n%s' "$cmd_joined" "$cmd" | grep -qE "$reach"; then
  while IFS= read -r hit; do
    [ -n "$hit" ] || continue
    verdict=$(onair_verdict "$hit")
    case "$verdict" in
      deny\ *) deny "${verdict#deny }" ;;
      ask\ *) ask "${verdict#ask }: confirm to run it (#487)" ;;
    esac
  done < <(onair_hits "$cmd_joined"; onair_hits "$cmd")
fi
main_dir=$(git -C "${cwd:-.}" rev-parse --show-toplevel 2>/dev/null || true)
main_checkout=${CLAUDE_PROJECT_DIR:-/home/lucas/coding/why2025-badge}
if [ "$main_dir" = "$main_checkout" ] && printf '%s' "$cmd" | grep -qE '(^|[;&|]\s*)git\s+(-C\s+\S+\s+)?(checkout\s+-[bB]\b|switch\b|reset\b)'; then
  case "$cmd" in *"/.claude/worktrees/"*) finish ;; esac
  ask "git checkout -b/-B, switch or reset in the MAIN checkout: agents work in .claude/worktrees/*; main only merges ff-only. Confirm if this is deliberate."
fi
finish
