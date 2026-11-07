#!/usr/bin/env bash
# Code and resource standard gate (#202), run by firmware/build.sh build after the P4 image is built.
# Limits, baseline rules and how to measure: docs/code-standard.md. Skips with a note if lizard is missing
# (the pattern of tools/static_checks.sh).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
if ! command -v lizard >/dev/null; then
    echo "note: lizard not installed (pipx install lizard), standard check skipped (#202)" >&2
    exit 0
fi
exec python3 -I "$here/check_standard.py" "$@"
