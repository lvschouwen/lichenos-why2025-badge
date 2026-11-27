#!/usr/bin/env bash
# Run a v3 RAM image on the badge: the one badge-facing command a v3 implementer agent may run (#614, #610).
#
#   tools/v3-load-ram.sh <image.elf|image.bin> [--timeout N] [--follow]
#
# Checks first, then calls `badge.sh load-ram` (RAM load through the ROM download mode, nothing written to flash,
# ends in a hard reset back to the flash image):
#   - tools/v3_load_ram_check.py: every segment inside L2MEM 0x4FF00000-0x4FFC0000, none flash-mapped, every loaded
#     byte inside 0x4FF10000-0x4FF3AFC0 (an ELF is converted to an image first and the result is checked too)
#   - one run at a time: flock on ~/badge-logs/v3-load-ram.lock (the kernel drops it when this script ends, so a
#     crash never leaves a stale lock)
#   - refuses while main holds the bench: the file ~/badge-logs/bench.lock exists. Main creates it (touch) before
#     flashing or soaking and removes it afterwards.
#
# Env: BADGE_PORT, else the first line of ${XDG_CONFIG_HOME:-~/.config}/why2025-badge/port, is the serial port on the
# flash host. --timeout N seconds (default 20; with --follow default 120; 1..900); --follow keeps the kernel
# running until the timeout instead of stopping at its 'END' line. The guard hook (.claude/hooks/guard-badge.sh)
# lets agents run this script and nothing else toward the badge.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
logs=$HOME/badge-logs
usage() { echo "usage: v3-load-ram.sh <image.elf|image.bin> [--timeout N] [--follow]" >&2; exit 2; }

image='' secs='' follow=''
while [ $# -gt 0 ]; do
    case $1 in
        --timeout) [ $# -ge 2 ] || usage; secs=$2; shift 2 ;;
        --follow) follow=--follow; shift ;;
        -*) usage ;;
        *) [ -z "$image" ] || usage; image=$1; shift ;;
    esac
done
[ -n "$image" ] || usage
case $secs in *[!0-9]*) echo "refusing: --timeout must be a number" >&2; exit 2 ;; esac
if [ -z "$secs" ]; then
    if [ -n "$follow" ]; then secs=120; else secs=20; fi
fi
{ [ "$secs" -ge 1 ] && [ "$secs" -le 900 ]; } || { echo "refusing: --timeout must be 1..900" >&2; exit 2; }
[ -f "$image" ] || { echo "refusing: no such image: $image" >&2; exit 1; }

port=${BADGE_PORT:-$(head -n1 "${XDG_CONFIG_HOME:-$HOME/.config}/why2025-badge/port" 2>/dev/null || true)}
[ -n "$port" ] || { echo "refusing: no port (set BADGE_PORT or write it to ~/.config/why2025-badge/port)" >&2; exit 1; }

python3 -I "$here/v3_load_ram_check.py" "$image" || exit 1
tmp=$(mktemp -d "${TMPDIR:-/tmp}/v3-load-ram.XXXXXX")
trap 'rm -f "$tmp"/*; rmdir "$tmp"' EXIT
if [ "$(head -c4 "$image")" = $'\x7fELF' ]; then
    esptool --chip esp32p4 elf2image --output "$tmp/image.bin" "$image" >/dev/null
    python3 -I "$here/v3_load_ram_check.py" "$tmp/image.bin" || exit 1
    image=$tmp/image.bin
fi

mkdir -p "$logs"
bench_held() { [ -e "$logs/bench.lock" ] && { echo "refusing: main holds the bench ($logs/bench.lock exists)" >&2; return 0; }; return 1; }
! bench_held || exit 1
exec 9>"$logs/v3-load-ram.lock"
flock -n 9 || { echo "refusing: another v3-load-ram run holds the lock" >&2; exit 1; }
# checked again under the lock: main may have taken the bench while the checks ran
! bench_held || exit 1

# shellcheck disable=SC2086 # $follow is empty or one flag
"$here/badge.sh" load-ram "$port" "$image" --baud 460800 --timeout "$secs" $follow
