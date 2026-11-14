#!/usr/bin/env bash
# Host test for firmware/build.sh's step plan (#401), no firmware built: the nested argument is refused on the developer
# path, the old BADGE_RELEASE_NESTED variable changes nothing, BADGE_SKIP_RELEASE only skips on exactly 1.
set -euo pipefail
sh=$(dirname "$0")/../../firmware/build.sh
fail() { echo "FAIL: $*" >&2; exit 1; }
r=0; out=$(bash "$sh" --plan build --nested-from-check 2>&1) || r=$?
[ "$r" = 2 ] || fail "developer path accepted --nested-from-check ($r): $out"
[ "$(bash "$sh" --plan build)" = "prechecks=yes release_step=yes" ] || fail "developer plan"
[ "$(BADGE_RELEASE_NESTED=1 bash "$sh" --plan build)" = "prechecks=yes release_step=yes" ] || fail "BADGE_RELEASE_NESTED thins the developer build"
[ "$(BADGE_RELEASE_NESTED=1 bash "$sh" --plan release build)" = "prechecks=yes release_step=no" ] || fail "BADGE_RELEASE_NESTED thins the release build"
[ "$(bash "$sh" --plan release build --nested-from-check)" = "prechecks=no release_step=no" ] || fail "nested release plan"
[ "$(BADGE_SKIP_RELEASE=1 bash "$sh" --plan build)" = "prechecks=yes release_step=no" ] || fail "skip=1"
r=0; BADGE_SKIP_RELEASE=0 bash "$sh" --plan build >/dev/null 2>&1 || r=$?
[ "$r" = 2 ] || fail "BADGE_SKIP_RELEASE=0 not refused ($r)"
echo "build_plan_test ok"
