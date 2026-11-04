#!/usr/bin/env bash
# Manage the warm worktree pool under .claude/worktrees/<slot>.
# A cold IDF build takes ~15 min, so slots are reset and reused, never removed.
# All git writes go through `git -C <slot>`, never against the main checkout.
set -euo pipefail

ROOT="${AGENT_WT_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && git rev-parse --show-toplevel)}"
POOL="$ROOT/.claude/worktrees"

die() { echo "agent-wt: $*" >&2; exit 1; }

usage() {
  cat >&2 <<USAGE
usage:
  agent-wt.sh list
  agent-wt.sh reset [--force] <slot> <branch> [base]
  agent-wt.sh new <slot> [base]
  agent-wt.sh done <slot>
USAGE
  exit 2
}

slot_dir() {
  local d="$POOL/$1"
  [[ "$1" =~ ^[A-Za-z0-9._-]+$ ]] || die "bad slot name: $1"
  echo "$d"
}

need_slot() {
  local d
  d="$(slot_dir "$1")"
  [[ -e "$d/.git" ]] || die "no such slot: $1 (use 'new')"
  echo "$d"
}

is_dirty() { [[ -n "$(git -C "$1" status --porcelain)" ]]; }

# Reads refs from the shared object store; -C the root only for read-only queries.
branch_exists() { git -C "$ROOT" show-ref --verify --quiet "refs/heads/$1"; }

cmd_list() {
  local d slot br sha ab dirty warm
  [[ -d "$POOL" ]] || die "no pool at $POOL"
  for d in "$POOL"/*/; do
    d="${d%/}"
    [[ -e "$d/.git" ]] || continue
    slot="$(basename "$d")"
    br="$(git -C "$d" symbolic-ref -q --short HEAD || echo '(detached)')"
    sha="$(git -C "$d" rev-parse --short HEAD)"
    ab="$(git -C "$d" rev-list --left-right --count main...HEAD | awk '{print "+" $2 "/-" $1}')"
    if is_dirty "$d"; then dirty=yes; else dirty=no; fi
    if [[ -d "$d/firmware/build" ]]; then warm=warm; else warm=cold; fi
    printf '%-10s %-24s %s  ahead/behind %s  dirty=%s  %s\n' \
      "$slot" "$br" "$sha" "$ab" "$dirty" "$warm"
  done
}

cmd_reset() {
  local force=0
  if [[ "${1:-}" == "--force" ]]; then force=1; shift; fi
  [[ $# -ge 2 && $# -le 3 ]] || usage
  local slot="$1" branch="$2" base="${3:-main}" d
  d="$(need_slot "$slot")"
  if is_dirty "$d" && [[ $force -eq 0 ]]; then
    die "slot $slot is dirty (use --force to discard)"
  fi
  if branch_exists "$branch" && ! git -C "$d" merge-base --is-ancestor "$branch" "$base"; then
    die "branch $branch exists and is not fully merged into $base"
  fi
  git -C "$d" fetch -q || echo "agent-wt: fetch failed, continuing offline" >&2
  # --force also discards local changes; untracked build output is kept.
  if [[ $force -eq 1 ]]; then
    git -C "$d" checkout -q -f -B "$branch" "$base"
  else
    git -C "$d" checkout -q -B "$branch" "$base"
  fi
  # Submodules use file:// URLs in some setups; git blocks that transport by default.
  git -C "$d" -c protocol.file.allow=always submodule update --init --recursive -q
  echo "$d"
}

cmd_new() {
  [[ $# -ge 1 && $# -le 2 ]] || usage
  local slot="$1" base="${2:-main}" d
  d="$(slot_dir "$slot")"
  [[ ! -e "$d" ]] || die "slot $slot already exists"
  git -C "$ROOT" worktree add --detach "$d" "$base"
  git -C "$d" -c protocol.file.allow=always submodule update --init --recursive -q
  echo "agent-wt: note: the first build in $slot is cold (about 15 minutes)" >&2
  echo "$d"
}

cmd_done() {
  [[ $# -eq 1 ]] || usage
  local d br
  d="$(need_slot "$1")"
  br="$(git -C "$d" symbolic-ref -q --short HEAD)" || die "slot $1 is already detached"
  [[ "$br" != main ]] || die "slot $1 is on main"
  is_dirty "$d" && die "slot $1 is dirty"
  git -C "$d" merge-base --is-ancestor "$br" main || die "branch $br is not merged into main"
  git -C "$d" checkout -q --detach main
  git -C "$d" branch -d "$br"
  echo "$d"
}

[[ $# -ge 1 ]] || usage
sub="$1"; shift
case "$sub" in
  list) cmd_list "$@" ;;
  reset) cmd_reset "$@" ;;
  new) cmd_new "$@" ;;
  done) cmd_done "$@" ;;
  *) usage ;;
esac
