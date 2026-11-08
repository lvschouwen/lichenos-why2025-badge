#!/usr/bin/env bash
# Decision logic of tools/git-hooks/pre-push with a stubbed build command (#252), and install-hooks idempotence.
set -uo pipefail
src=$(cd "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d)
trap 'mv "$work" "$work.done" 2>/dev/null' EXIT
fail=0
check() { # name expected-rc actual-rc
    if [ "$2" = "$3" ]; then echo "ok   $1"; else echo "FAIL $1: want $2 got $3"; fail=1; fi
}

mkdir -p "$work/repo/tools/git-hooks" "$work/repo/firmware"
cd "$work/repo" || exit 1
git init -q . && git config user.email t@t && git config user.name t
cp "$src/tools/git-hooks/pre-push" tools/git-hooks/
cp "$src/tools/install-hooks.sh" tools/
echo a >a && git add -A && git commit -qm one
head=$(git rev-parse HEAD)
tree=$(git rev-parse 'HEAD^{tree}')
zero=0000000000000000000000000000000000000000
hook=tools/git-hooks/pre-push
run() { echo "$1" | BADGE_PREPUSH_BUILD_CMD="$2" bash "$hook" >/dev/null 2>&1; }

run "refs/heads/main $head refs/heads/feat $zero" false; check "non-main branch skips" 0 $?
run "(delete) $zero refs/heads/main $head" false; check "delete of main skips" 0 $?
run "refs/heads/main $head refs/heads/main $zero" false; check "red check refuses" 1 $?
run "refs/heads/main $head refs/heads/main $zero" true; check "green check passes" 0 $?
mkdir -p firmware/build && touch "firmware/build/.green-$tree"
run "refs/heads/main $head refs/heads/main $zero" false; check "stamp skips the build" 0 $?
mv "firmware/build/.green-$tree" "$work/stamp"
echo b >>a
run "refs/heads/main $head refs/heads/main $zero" true; check "dirty tree refuses" 1 $?
git checkout -q -- a

bash tools/install-hooks.sh >/dev/null && bash tools/install-hooks.sh >/dev/null; check "install twice" 0 $?
[ -L .git/hooks/pre-push ]; check "pre-push linked" 0 $?
exit $fail
