#!/usr/bin/env bash
# Code and resource standard gate (#202), run by firmware/build.sh build after the P4 image is built.
# Limits, baseline rules and how to measure: docs/code-standard.md.
# A missing lizard FAILS the gate (a skipped gate is a gate that never runs); the one way past it is the explicit
# BADGE_SKIP_STANDARD=1, which prints loudly.
set -euo pipefail
here=$(cd "${BASH_SOURCE[0]%/*}" && pwd)
if [ "${BADGE_SKIP_STANDARD:-}" = 1 ]; then
    echo "!!! BADGE_SKIP_STANDARD=1: the code and resource standard gate (#202) is SKIPPED, nothing is checked !!!" >&2
    exit 0
fi
if ! command -v lizard >/dev/null; then
    echo "standard: lizard is not installed (pipx install lizard), so the gate cannot run." >&2
    echo "standard: install it, or set BADGE_SKIP_STANDARD=1 to skip the gate on purpose (#202)." >&2
    exit 1
fi
exec python3 -I "$here/check_standard.py" "$@"
