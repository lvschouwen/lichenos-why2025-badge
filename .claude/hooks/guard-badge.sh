#!/usr/bin/env bash
# PreToolUse (Bash) guard for Lichen OS work (#183).
# Two irreversible or workflow-breaking commands get a permission prompt instead of running silently:
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
main_dir=$(git -C "${cwd:-.}" rev-parse --show-toplevel 2>/dev/null || true)
if [ "$main_dir" = "/home/lucas/coding/why2025-badge" ] && printf '%s' "$cmd" | grep -qE '(^|[;&|]\s*)git\s+(-C\s+\S+\s+)?(checkout\s+-[bB]\b|switch\b|reset\b)'; then
  case "$cmd" in *"/.claude/worktrees/"*) exit 0 ;; esac
  ask "git checkout -b/-B, switch or reset in the MAIN checkout: agents work in .claude/worktrees/*; main only merges ff-only. Confirm if this is deliberate."
fi
exit 0
