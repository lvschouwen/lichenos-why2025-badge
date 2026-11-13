#!/bin/bash
# usage: merge-one.sh <slot> <branch> <tag>   (merges into the main checkout; stops on anything unexpected)
# Logs and notes go to $SOAK_SCRATCH (default: this directory; set it to a scratch directory outside the repo).
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(git -C "$HERE" rev-parse --show-toplevel)" || exit 1
S="${SOAK_SCRATCH:-$HERE}"; W="$ROOT/.claude/worktrees/$1"; B="$2"; T="$3"
cd "$ROOT" || exit 1
[ -z "$(git status --porcelain)" ] || { echo "$T: main tree not clean"; exit 1; }
[ -z "$(git -C "$W" status --porcelain)" ] || { echo "$T: slot not clean"; exit 1; }
ORIG=$(git -C "$W" rev-parse HEAD); BASE=$(git merge-base main "$ORIG")
git diff "$BASE" "$ORIG" -- tools/standard-baseline.txt | grep -E '^-[a-z]' | grep -vE '^-size' | sed 's/^-//' > "$S/$T-remove.txt"
git diff "$BASE" "$ORIG" -- tools/standard-baseline.txt | grep -E '^\+[a-z]' | grep -vE '^\+size' > "$S/$T-added.txt"
[ -s "$S/$T-added.txt" ] && { echo "$T: branch ADDS non-size baseline entries; stop"; cat "$S/$T-added.txt"; exit 1; }
git -C "$W" rebase main >/dev/null 2>&1
for _ in 1 2 3 4 5 6; do
  git -C "$W" status -s | grep -qE '^(UU|AA)' || break
  for f in $(git -C "$W" diff --name-only --diff-filter=U); do
    case "$f" in
      tools/standard-baseline.txt) git -C "$W" checkout --ours "$f" && git -C "$W" add "$f";;
      host_tests/CMakeLists.txt|firmware/components/*/CMakeLists.txt|firmware/main/CMakeLists.txt) python3 -I "$HERE/union.py" "$W/$f" && git -C "$W" add "$f" || { echo "$T: union failed in $f"; exit 1; };;
      *) echo "$T: UNHANDLED CONFLICT in $f"; exit 1;;
    esac
  done
  GIT_EDITOR=true git -C "$W" rebase --continue >/dev/null 2>&1
done
git -C "$W" status -sb | grep -q "^## $B" || { echo "$T: rebase did not finish"; git -C "$W" status -sb | head -3; exit 1; }
if [ -s "$S/$T-remove.txt" ]; then
  grep -vxF -f "$S/$T-remove.txt" "$W/tools/standard-baseline.txt" > "$W/tools/standard-baseline.txt.new" && mv "$W/tools/standard-baseline.txt.new" "$W/tools/standard-baseline.txt"
  if [ -n "$(git -C "$W" status --porcelain)" ]; then git -C "$W" commit -q -am "chore(build): drop the baseline entries this branch made obsolete

Re-applied after the rebase took main's tools/standard-baseline.txt."; echo "$T: re-removed $(wc -l < "$S/$T-remove.txt") baseline entr(ies)"; fi
fi
git merge --ff-only "$B" >/dev/null 2>&1 || { echo "$T: ff merge failed"; exit 1; }
~/.claude/bin/agent-check > "$S/check-main-$T.log" 2>&1; rc=$?
grep -E "tests passed|tests failed" "$S/check-main-$T.log" | tail -1
grep -q "agent-check: green" "$S/check-main-$T.log" || { echo "$T: CHECK NOT GREEN (exit $rc)"; grep -E "FAILED|error:|fail " "$S/check-main-$T.log" | head -8; exit 1; }
st="$(git status --porcelain)"
if [ "$st" = " M tools/standard-baseline.txt" ]; then
  git diff | grep -E '^[-+][a-z]' | grep -v size && { echo "$T: check changed NON-size baseline lines; stop"; exit 1; }
  git add tools/standard-baseline.txt && git commit -q -m "chore(build): standard baseline size lines after the $T merge

Co-Authored-By: Claude Fable 5.2 <noreply@anthropic.com>"
elif [ -n "$st" ]; then echo "$T: unexpected tree state after the check"; git status -s | head -5; exit 1; fi
echo "$T: merged, check green, HEAD $(git rev-parse --short HEAD)"
