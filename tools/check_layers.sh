#!/usr/bin/env bash
# Layer checker (#218): components, REQUIRES, includes and link-time symbol edges against tools/layers.conf, ratcheted
# by tools/layers-baseline.txt. Rules and options: the header of tools/check_layers.py (--report lists every violation).
# Run after the IDF build (rule C reads firmware/build); needs riscv32-esp-elf-nm on PATH (IDF export.sh) unless --no-symbols.
# Then the SD apps' export table (#519): regenerated and diffed, and the built apps' undefined symbols checked against it.
set -euo pipefail
here=$(dirname "$0")
python3 -I "$here/check_layers.py" "$@"
case " $* " in
    *" --gen-manifest "*|*" --report "*|*" --init "*|*" --shrink "*) exit 0 ;;
esac
elfs=()
case " $* " in
    *" --no-symbols "*) ;; # no build was made: its apps are not either
    *) for f in "$here"/../firmware/build/sdapps/*/app.elf; do [ -e "$f" ] && elfs+=("$f"); done ;;
esac
python3 -I "$here/gen_export_table.py" --check --elf "${elfs[@]}"
