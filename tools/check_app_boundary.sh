#!/usr/bin/env bash
# App boundary check (#178, docs/app-api.md "Boundary check"): the L6 profile of the layer checker (#218), i.e. the app
# components only, against tools/layers.conf and the L6 part of tools/layers-baseline.txt. Extra arguments
# (--no-symbols, --report) go to tools/check_layers.py. The SD-card apps (sdapps/, #522) are standalone ELF projects, not
# components: tools/check_sdapp_boundary.py applies the same L6 header and symbol rules to their sources. --root is passed
# on to both.
set -euo pipefail
here=$(dirname "$0")
root=()
prev=
for a in "$@"; do
    [ "$prev" = --root ] && root=(--root "$a")
    prev=$a
done
python3 -I "$here/check_layers.py" --only-layer L6 "$@"
python3 -I "$here/check_sdapp_boundary.py" "${root[@]}"
