#!/usr/bin/env bash
# Layer checker (#218): components, REQUIRES, includes and link-time symbol edges against tools/layers.conf, ratcheted
# by tools/layers-baseline.txt. Rules and options: the header of tools/check_layers.py (--report lists every violation).
# Run after the IDF build (rule C reads firmware/build); needs riscv32-esp-elf-nm on PATH (IDF export.sh) unless --no-symbols.
set -euo pipefail
exec python3 -I "$(dirname "$0")/check_layers.py" "$@"
