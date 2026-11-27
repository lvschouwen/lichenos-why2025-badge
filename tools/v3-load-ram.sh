#!/usr/bin/env bash
# Run a v3 RAM image on the badge: the one badge-facing command a v3 implementer agent may run (#614, #610).
#
#   tools/v3-load-ram.sh <image.elf|image.bin> [--timeout N] [--follow]
#
# Checks first, then calls `badge.sh load-ram` (RAM load through the ROM download mode, nothing written to flash,
# ends in a hard reset back to the flash image):
#   - the input is copied into a private temp dir first; the copy is what is checked, converted and uploaded, so
#     the file cannot change between check and use
#   - tools/v3_load_ram_check.py: every segment inside L2MEM 0x4FF00000-0x4FFC0000, none flash-mapped, every loaded
#     byte inside 0x4FF10000-0x4FF3AFC0 (an ELF is converted to an image first and the result is checked too)
#   - one run at a time: flock on ~/badge-logs/v3-load-ram.lock (the kernel drops it when this script ends, so a
#     crash never leaves a stale lock)
#   - refuses while main holds the bench: the file ~/badge-logs/bench.lock exists. Main creates it (touch) before
#     flashing or soaking and removes it afterwards.
#
# Trust: the guard hook (.claude/hooks/guard-badge.sh) lets an agent run only the copy of this script in the MAIN
# checkout, with plain arguments. So a slot's edited copy never runs for an agent, and this script calls badge.sh and
# the checker from the main checkout (found through git's common dir) with a fixed PATH.
#
# Env: BADGE_PORT, else the first line of ${XDG_CONFIG_HOME:-~/.config}/why2025-badge/port, is the serial port on the
# flash host (a port from that file is also passed as BADGE_EXTRA_PORTS, so a CH340 bridge works); it must look like /dev/ttyACM0, /dev/ttyUSB0 or /dev/serial/by-id/<name>. --timeout N seconds (default
# 20; with --follow default 120; 1..900); --follow keeps the kernel running until the timeout instead of stopping at
# its 'END' line.
set -euo pipefail
PATH=/usr/local/bin:/usr/bin:/bin:$HOME/.local/bin
export PATH
here=$(cd "$(dirname "$0")" && pwd)
common=$(git -C "$here" rev-parse --path-format=absolute --git-common-dir 2>/dev/null || true)
if [ -n "$common" ]; then root=$(cd "$common/.." && pwd); else root=$(cd "$here/.." && pwd); fi
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

port_file=${XDG_CONFIG_HOME:-$HOME/.config}/why2025-badge/port
port=${BADGE_PORT:-}
extra_ports=${BADGE_EXTRA_PORTS:-}
if [ -z "$port" ]; then
    port=$(head -n1 "$port_file" 2>/dev/null || true)
    # the owner wrote this port down as the badge, so it may be a CH340 (1a86) that badge.sh's Espressif vendor check
    # would refuse; a port from the environment gets no such pass
    extra_ports=$port
fi
[ -n "$port" ] || { echo "refusing: no port (set BADGE_PORT or write it to ~/.config/why2025-badge/port)" >&2; exit 1; }
[[ $port =~ ^/dev/(serial/by-id/[A-Za-z0-9_.:-]+|tty(ACM|USB)[0-9]+)$ ]] ||
    { echo "refusing: port '$port' is not /dev/ttyACMn, /dev/ttyUSBn or /dev/serial/by-id/<name>" >&2; exit 1; }

tmp=$(mktemp -d "${TMPDIR:-/tmp}/v3-load-ram.XXXXXX")
trap 'rm -f "$tmp"/*; rmdir "$tmp"' EXIT
cp -- "$image" "$tmp/input"
image=$tmp/input
check=$root/tools/v3_load_ram_check.py
python3 -I "$check" "$image" || exit 1
if [ "$(head -c4 "$image" | tr -d '\0')" = $'\x7fELF' ]; then
    # IDF 6.1's esptool first (a pip --user esptool may be broken), then whatever the fixed PATH has
    esptool_bin=''
    for c in "$HOME"/.espressif/python_env/idf6.1_*/bin/esptool; do [ -x "$c" ] && { esptool_bin=$c; break; }; done
    [ -n "$esptool_bin" ] || esptool_bin=$(command -v esptool) || { echo "refusing: esptool not found" >&2; exit 1; }
    "$esptool_bin" --chip esp32p4 elf2image --output "$tmp/image.bin" "$image" >/dev/null
    python3 -I "$check" "$tmp/image.bin" || exit 1
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
# badge.sh gets a clean environment: no BADGE_VENV, BADGE_BAUD, XDG_CONFIG_HOME or other caller setting reaches it,
# and the ssh host is checked here, so a host file reading '-oProxyCommand=...' never becomes an ssh option
host=${BADGE_HOST:-$(cat "$HOME/.config/why2025-badge/host" 2>/dev/null || true)}
host=${host//[[:space:]]/}
[[ $host =~ ^[A-Za-z0-9][A-Za-z0-9._-]*$ ]] || { echo "refusing: badge host '$host' is not a plain host name" >&2; exit 1; }
env -i HOME="$HOME" PATH="$PATH" ${SSH_AUTH_SOCK:+SSH_AUTH_SOCK="$SSH_AUTH_SOCK"} BADGE_HOST="$host" \
    BADGE_EXTRA_PORTS="$extra_ports" "$root/tools/badge.sh" load-ram "$port" "$image" --baud 460800 --timeout "$secs" $follow
