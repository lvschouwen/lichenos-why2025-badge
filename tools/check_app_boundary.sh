#!/usr/bin/env bash
# App boundary check (#178, docs/app-api.md "Boundary check"): the L6 profile of the layer checker (#218), i.e. the app
# components only, against tools/layers.conf and the L6 part of tools/layers-baseline.txt. Extra arguments
# (--no-symbols, --report) go to tools/check_layers.py.
set -euo pipefail
exec python3 -I "$(dirname "$0")/check_layers.py" --only-layer L6 "$@"
