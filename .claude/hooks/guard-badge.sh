#!/usr/bin/env bash
# PreToolUse (Bash) guard for Lichen OS work (#183).
# Irreversible, workflow-breaking or off-limits on-air commands get a prompt or a refusal instead of running silently:
#  1. `badge.sh flash-fw ... --wipe-storage` or `--storage-image <bin>` (#300): both replace the MeshCore identity,
#     contacts and channels.
#  2. `git checkout -b/-B`, `git switch`, `git reset` issued from the MAIN checkout (a resumed agent once
#     did this and turned the ff-merge into a silent no-op). Agents work in .claude/worktrees/*.
# "ask" prompts the user in the main session and stops a subagent, which must then report. Fails open.
set -u
input=$(cat 2>/dev/null || true)
cmd=$(printf '%s' "$input" | jq -r '.tool_input.command // empty' 2>/dev/null || true)
cwd=$(printf '%s' "$input" | jq -r '.cwd // empty' 2>/dev/null || true)
file=$(printf '%s' "$input" | jq -r '.tool_input.file_path // empty' 2>/dev/null || true)
ask() {
  jq -cn --arg r "$1" '{hookSpecificOutput:{hookEventName:"PreToolUse",permissionDecision:"ask",permissionDecisionReason:$r}}'
  exit 0
}
deny() {
  jq -cn --arg r "$1" '{hookSpecificOutput:{hookEventName:"PreToolUse",permissionDecision:"deny",permissionDecisionReason:$r}}'
  exit 0
}
# The on-air allow list (rule 3) and its overrides are Lucas's alone: refused, not asked (an ask can be settled
# without him in auto mode). He edits the file himself.
case "$file$cmd" in
  *onair-allow.local* | *ONAIR_ALLOW_FILE*)
    deny "the on-air allow list (.claude/onair-allow.local) and its overrides are Lucas's to change or use himself (#287)" ;;
esac
[ -n "$cmd" ] || exit 0
if printf '%s' "$cmd" | grep -qE 'badge\.sh[^|;&]*flash-fw[^|;&]*--wipe-storage'; then
  ask "flash-fw --wipe-storage erases the MeshCore identity, contacts and channels on the badge. First-install only; needs Lucas."
fi
# 1b. `badge.sh flash-fw ... --storage-image <bin>` (#300) replaces the whole flash drive with <bin> the same way.
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
reach='(^|[^[:alnum:]_-])badge\.sh[[:space:]]+(fw|console)[[:space:]]|(^|[^[:alnum:]_-])ssh[[:space:]]|/dev/tty|curl[^|;&]*/console'
# Backslash-newline continuations join first, so a target on the next line is still seen
cmd_joined=$(printf '%s\n' "$cmd" | sed -e ':a' -e '/\\$/N' -e 's/\\\n/ /' -e 'ta')
# Never looser than main, and one console line must never hide the next. grep -o does not rescan text it consumed, so
# a verb's match (up to two words after it) could swallow the next verb whatever stands between them (a newline in
# any escape form: \n, \12, \xa, \u000a, %0a, a here-doc or a second line). So the text is judged twice and any one
# refusal refuses the whole command: (A) whole, as main did, so a target is never cut apart; (B) cut in front of every
# console verb, so no match can consume a later verb. The text is the command as written and with its
# backslash-newlines joined, from every line once any line reaches the badge. Limit: a payload encoded (base64, xxd)
# or built from pieces (echo -n) never shows its verb to a text hook (#440).
onair_alt='mesh[[:space:]]+(send|dm|remote|telemetry|trace|discover|history|advert|scope)([[:space:]]+[^[:space:];&|)]+){0,2}|onair([[:space:]]+[^[:space:];&|)]+){0,2}'
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
  done < <(printf '%s\n' "$text" | sed -E 's/(mesh|onair)[[:space:]]/\n&/g')
}
if printf '%s\n%s' "$cmd_joined" "$cmd" | grep -qE "$reach"; then
  while IFS= read -r hit; do
    [ -n "$hit" ] || continue
    verdict=$(onair_verdict "$hit")
    case "$verdict" in
      deny\ *) deny "${verdict#deny }" ;;
      ask\ *) deny "${verdict#ask }: Lucas sets it in MeshCore Settings on the badge" ;;
    esac
  done < <(onair_hits "$cmd_joined"; onair_hits "$cmd")
fi
main_dir=$(git -C "${cwd:-.}" rev-parse --show-toplevel 2>/dev/null || true)
main_checkout=${CLAUDE_PROJECT_DIR:-/home/lucas/coding/why2025-badge}
if [ "$main_dir" = "$main_checkout" ] && printf '%s' "$cmd" | grep -qE '(^|[;&|]\s*)git\s+(-C\s+\S+\s+)?(checkout\s+-[bB]\b|switch\b|reset\b)'; then
  case "$cmd" in *"/.claude/worktrees/"*) exit 0 ;; esac
  ask "git checkout -b/-B, switch or reset in the MAIN checkout: agents work in .claude/worktrees/*; main only merges ff-only. Confirm if this is deliberate."
fi
exit 0
