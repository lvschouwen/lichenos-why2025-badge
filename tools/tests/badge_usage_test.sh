#!/usr/bin/env bash
# Host test (#195): badge.sh without a command prints its usage and exits 1, even with no BADGE_* env set.
set -uo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=$(env -u BADGE_FIRMWARE_BIN -u BADGE_ASSETS_BIN bash "$here/../badge.sh" 2>&1); rc=$?
[ "$rc" = 1 ] || { echo "FAIL: exit $rc, expected 1: $out" >&2; exit 1; }
grep -q '^Usage: badge.sh <command>' <<<"$out" || { echo "FAIL: no usage text: $out" >&2; exit 1; }
grep -q 'or \$BADGE_FIRMWARE_BIN)' <<<"$out" || { echo "FAIL: env var name not shown literally" >&2; exit 1; }
echo "badge_usage_test: ok"
