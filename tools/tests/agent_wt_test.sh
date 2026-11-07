#!/usr/bin/env bash
# Host test for tools/agent-wt.sh (#203): numbered slots and rename, in a throwaway repo.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
tmp=$(mktemp -d); trap 'git -C "$tmp/repo" worktree prune 2>/dev/null; rm -r "$tmp"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }
git init -q -b main "$tmp/repo"
printf 'build/\n' > "$tmp/repo/.gitignore" # like the real repo: build output never makes a slot dirty
git -C "$tmp/repo" add .gitignore
git init -q -b main "$tmp/sub"
git -C "$tmp/sub" -c user.email=t@t -c user.name=t commit -q --allow-empty -m sub
git -C "$tmp/repo" -c protocol.file.allow=always submodule -q add "$tmp/sub" sub # slots carry submodules: git worktree move refuses those
git -C "$tmp/repo" -c user.email=t@t -c user.name=t commit -q -m init
wt() { AGENT_WT_ROOT="$tmp/repo" bash "$here/../agent-wt.sh" "$@" 2>/dev/null; }

[ "$(basename "$(wt new --next)")" = wt-1 ] || fail "first --next is not wt-1"
[ "$(basename "$(wt new --next)")" = wt-2 ] || fail "second --next is not wt-2"
mkdir -p "$tmp/repo/.claude/worktrees/wt-2/firmware/build"
echo cache > "$tmp/repo/.claude/worktrees/wt-2/firmware/build/CMakeCache.txt"
mkdir -p "$tmp/repo/.claude/worktrees/wt-2/firmware/build/bootloader"
echo cache > "$tmp/repo/.claude/worktrees/wt-2/firmware/build/bootloader/CMakeCache.txt"
echo obj > "$tmp/repo/.claude/worktrees/wt-2/firmware/build/a.o"
[ "$(basename "$(wt rename wt-2 wt-5)")" = wt-5 ] || fail "rename did not print the new slot"
d=$tmp/repo/.claude/worktrees/wt-5
[ -e "$d/.git" ] && [ ! -e "$tmp/repo/.claude/worktrees/wt-2" ] || fail "slot not moved"
[ ! -e "$d/firmware/build" ] || fail "build dir kept: it holds the old slot path in generated files"
ls "$tmp/repo/.claude/worktrees/.stale"/wt-5-*/firmware/build/a.o >/dev/null 2>&1 || fail "old build not kept aside in .stale"
git -C "$tmp/repo" worktree list | grep -q "/wt-5 " || fail "git does not know the moved worktree"
git -C "$d" status --porcelain >/dev/null || fail "moved slot is broken"
[ "$(git -C "$d/sub" rev-parse --show-toplevel)" = "$d/sub" ] || fail "submodule still points at the old slot"
[ "$(basename "$(wt new --next)")" = wt-2 ] || fail "--next does not reuse the freed name"
wt rename wt-1 wt-5 && fail "rename onto an existing slot succeeded"
echo "agent_wt_test: ok"
