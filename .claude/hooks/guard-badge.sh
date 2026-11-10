#!/usr/bin/env bash
# PreToolUse (Bash) guard for Lichen OS work (#183).
# Irreversible, workflow-breaking or off-limits on-air commands get a prompt or a refusal instead of running silently:
#  1. `badge.sh flash-fw ... --wipe-storage` (erases the MeshCore identity, contacts and channels).
#  2. `git checkout -b/-B`, `git switch`, `git reset` issued from the MAIN checkout (a resumed agent once
#     did this and turned the ff-merge into a silent no-op). Agents work in .claude/worktrees/*.
# "ask" prompts the user in the main session and stops a subagent, which must then report. Fails open.
set -u
input=$(cat 2>/dev/null || true)
cmd=$(printf '%s' "$input" | jq -r '.tool_input.command // empty' 2>/dev/null || true)
cwd=$(printf '%s' "$input" | jq -r '.cwd // empty' 2>/dev/null || true)
[ -n "$cmd" ] || exit 0
ask() {
  jq -cn --arg r "$1" '{hookSpecificOutput:{hookEventName:"PreToolUse",permissionDecision:"ask",permissionDecisionReason:$r}}'
  exit 0
}
if printf '%s' "$cmd" | grep -qE 'badge\.sh[^|;&]*flash-fw[^|;&]*--wipe-storage'; then
  ask "flash-fw --wipe-storage erases the MeshCore identity, contacts and channels on the badge. First-install only; needs Lucas."
fi
deny() {
  jq -cn --arg r "$1" '{hookSpecificOutput:{hookEventName:"PreToolUse",permissionDecision:"deny",permissionDecisionReason:$r}}'
  exit 0
}
# 3. On air only #badgetest and the Heltec test node (#287): a console command that transmits to a channel or a
#    contact is refused for any other target. Only lines that reach the badge (badge.sh fw|console, ssh, a
#    tty) are looked at, so a source grep or a commit message describing such a command passes. Allowed contact targets (the Heltec's name or key digits) live in
#    the gitignored .claude/onair-allow.local, one per line, so no device id is committed; without it, only
#    #badgetest sends pass.
reach='(^|[^[:alnum:]_-])badge\.sh[[:space:]]+(fw|console)[[:space:]]|(^|[^[:alnum:]_-])ssh[[:space:]]|/dev/tty'
if printf '%s' "$cmd" | grep -qE "$reach"; then
  allow_file=${ONAIR_ALLOW_FILE:-${CLAUDE_PROJECT_DIR:-/home/lucas/coding/why2025-badge}/.claude/onair-allow.local}
  allowed() { # $1 = target, matched case-insensitively against the allow file
    [ -r "$allow_file" ] || return 1
    grep -v '^[[:space:]]*#' "$allow_file" | sed 's/[[:space:]]*$//' | grep -qixF -- "$1"
  }
  while IFS= read -r hit; do
    [ -n "$hit" ] || continue
    read -r _ sub target _ <<<"$(printf '%s' "$hit" | tr -d "'\"\\\\")"
    case "$sub" in
      send)
        [ "$target" = "#badgetest" ] ||
          deny "on air only #badgetest: 'mesh send ${target:-?}' would transmit to another channel (#287, bench rules)" ;;
      dm | telemetry | trace | discover)
        [ -z "$target" ] || allowed "$target" ||
          deny "on air only the Heltec test node: 'mesh $sub $target' targets a contact not in .claude/onair-allow.local (#287)" ;;
      remote)
        case "$target" in "" | settings) ;; *)
          allowed "$target" ||
            deny "on air only the Heltec test node: 'mesh remote $target' targets a node not in .claude/onair-allow.local (#287)" ;;
        esac ;;
      advert)
        case "$target" in interval | manual) ask "mesh advert $target changes what the badge floods on air: needs Lucas" ;; esac ;;
    esac
  done < <(printf '%s\n' "$cmd" | grep -E "$reach" | grep -oE 'mesh[[:space:]]+(send|dm|remote|telemetry|trace|discover|advert)([[:space:]]+[^[:space:];&|)]+)?' || true)
fi
main_dir=$(git -C "${cwd:-.}" rev-parse --show-toplevel 2>/dev/null || true)
main_checkout=${CLAUDE_PROJECT_DIR:-/home/lucas/coding/why2025-badge}
if [ "$main_dir" = "$main_checkout" ] && printf '%s' "$cmd" | grep -qE '(^|[;&|]\s*)git\s+(-C\s+\S+\s+)?(checkout\s+-[bB]\b|switch\b|reset\b)'; then
  case "$cmd" in *"/.claude/worktrees/"*) exit 0 ;; esac
  ask "git checkout -b/-B, switch or reset in the MAIN checkout: agents work in .claude/worktrees/*; main only merges ff-only. Confirm if this is deliberate."
fi
exit 0
