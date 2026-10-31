#!/usr/bin/env bash
# Back up, flash and monitor the badge through a USB host reachable over SSH.
#
# Only Espressif USB devices (VID 303a) are ever touched, so other serial
# devices on the host stay untouched.
#
# Env:
#   BADGE_HOST     ssh host the badge is plugged into   (default: read from
#                  ${XDG_CONFIG_HOME:-~/.config}/why2025-badge/host)
#   BADGE_VENV     venv with esptool on that host        (default: ~/badge-venv)
#   BADGE_BACKUPS  local dir for flash backups           (default: ~/badge-backups)
#   BADGE_BAUD     esptool baud rate (default: 460800; 921600 corrupts reads over CH340)
#   BADGE_EXTRA_PORTS  space-separated non-Espressif ports that are known to be
#                      the badge, e.g. a CH340 bridge   (default: none)
#   BADGE_OTA_TOKEN    the badge's update token ("fw token") (default: read
#                      from ${XDG_CONFIG_HOME:-~/.config}/why2025-badge/ota-token)
#   BADGE_OTA_PORT     port of the badge's update server  (default: 8080)
#   BADGE_FIRMWARE_BIN image ota-firmware installs        (default: firmware/build/badge_fw.bin)
#   BADGE_OTA_COMPRESS 0 sends uncompressed even when the receiver takes deflate (default: 1)
#
# Paths containing ~ and the remote command strings are meant to expand on the
# badge host, not locally.
# shellcheck disable=SC2088,SC2029,SC2016
set -euo pipefail

HOST=${BADGE_HOST:-$(cat "${XDG_CONFIG_HOME:-$HOME/.config}/why2025-badge/host" 2>/dev/null || true)}
HOST=${HOST//[[:space:]]/}
VENV=${BADGE_VENV:-'~/badge-venv'}
BACKUPS=${BADGE_BACKUPS:-$HOME/badge-backups}
BAUD=${BADGE_BAUD:-460800}
REMOTE_DIR='~/badge'
REPO=$(cd "$(dirname "$0")/.." && pwd)

usage() {
    cat <<EOF
Usage: $(basename "$0") <command> [args]

  ports                     list Espressif ports on $HOST and the chip behind each
  backup <port>             read the whole flash of the chip on <port> into $BACKUPS
  flash-fw <port> [--no-storage]
                            write firmware/build (Lichen: bootloader, partitions, app, storage) to the P4;
                            storage replaces everything on the badge's flash drive (MeshCore identity,
                            contacts, settings), --no-storage leaves it alone
  crash <port> [elf]        fetch the core dump over the console ("fw crash dump"), check it and decode it with
                            the ELF kept by flash-fw/ota-firmware in $ELFS (or [elf])
  restore <port> <file> --yes
                            write a backup image back at offset 0 (checks the .sha256 and that the chip
                            matches the backup's name; --yes confirms overwriting the whole flash)
  monitor <port>            open a serial console (exit with Ctrl-])
  console <port> <line>     send one line to the badge's console without resetting it and show the reply;
                            commands start with "fw " (try "fw help")

Over Wi-Fi, while the badge is on Wi-Fi:
  ota-info <ip>                       show the firmware version the badge runs
  ota-firmware <ip> [--no-reboot]     install firmware/build/badge_fw.bin (or $BADGE_FIRMWARE_BIN) and reboot into it
  ota-file <ip> <file> <badge-path>   push one file, e.g. 'FLASH0:[c6]firmware.bin'
  ota-rm <ip> <badge-path>            delete one file on the badge
  ota-reboot <ip>

<port> is a /dev/serial/by-id/... path or /dev/ttyACMn as shown by 'ports'.
EOF
    exit 1
}

ELFS=${BADGE_ELFS:-$BACKUPS/elf}
IDF=${BADGE_IDF_PATH:-$HOME/esp/esp-idf-v6.1}

remote() {
    [ -n "$HOST" ] || { echo "no badge host: put its SSH name in ${XDG_CONFIG_HOME:-~/.config}/why2025-badge/host (or BADGE_HOST)" >&2; exit 1; }
    ssh "$HOST" "$@"
}
esptool() { remote "$VENV/bin/esptool $*"; }

# The chip behind <port> as one lowercase word, e.g. esp32p4 (empty if it doesn't answer)
chip_of() {
    esptool --port "$1" chip-id 2>/dev/null | sed -n 's/^Chip type: *//p' | head -1 | awk '{print $1}' |
        tr -cd 'A-Za-z0-9' | tr 'A-Z' 'a-z'
}

require_espressif_port() {
    local port=$1 allowed
    # Some badges sit behind a generic USB-serial bridge (e.g. CH340) that has no
    # serial number to recognise it by, so those must be named explicitly.
    for allowed in ${BADGE_EXTRA_PORTS:-}; do
        [ "$port" = "$allowed" ] && return 0
    done
    remote "udevadm info -q property -n '$port' | grep -qx 'ID_VENDOR_ID=303a'" ||
        { echo "refusing: $port is not an Espressif (303a) USB device (add it to BADGE_EXTRA_PORTS if it really is the badge)" >&2; exit 1; }
}

cmd_ports() {
    remote 'for p in /dev/serial/by-id/*; do
        [ -e "$p" ] || continue
        udevadm info -q property -n "$p" | grep -qx ID_VENDOR_ID=303a || continue
        echo "$p -> $(readlink -f "$p")"
    done' | while read -r port _ dev; do
        chip=$(chip_of "$dev")
        echo "$port  ($dev)  ${chip:-unknown}"
    done
}

cmd_backup() {
    local port=$1
    require_espressif_port "$port"
    mkdir -p "$BACKUPS"
    remote "mkdir -p $REMOTE_DIR/backups $REMOTE_DIR/images"
    local name chip
    chip=$(chip_of "$port")
    [ -n "$chip" ] || { echo "refusing: no answer from the chip on $port (is it in download mode?)" >&2; exit 1; }
    name="badge-$chip-$(basename "$port")-$(date +%Y%m%d-%H%M%S).bin"
    esptool --port "$port" --baud "$BAUD" read-flash 0 ALL "$REMOTE_DIR/backups/$name"
    scp "$HOST:$REMOTE_DIR/backups/$name" "$BACKUPS/$name"
    (cd "$BACKUPS" && sha256sum "$name" | tee "$name.sha256")
    echo "backup kept on $HOST:$REMOTE_DIR/backups and in $BACKUPS"
}

# Keeps the ELF of the firmware just built under its version and under its SHA-256, so a core dump from it can be
# decoded later (#74). The SHA-256 of the ELF file is what the image carries as its "ELF file SHA256" and what a core
# dump names the crashed image by: after a rollback the running version is not the crashed one, the SHA still is
archive_elf() {
    local build=$1 version sha
    mkdir -p "$ELFS"
    sha=$(sha256sum "$build/badge_fw.elf" | cut -d' ' -f1)
    cp "$build/badge_fw.elf" "$ELFS/sha-$sha.elf" && echo "ELF kept as $ELFS/sha-$sha.elf"
    version=$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1] + "/project_description.json"))["project_version"])' "$build") ||
        { echo "warning: no project_version in $build, ELF kept by SHA only" >&2; return 0; }
    cp "$build/badge_fw.elf" "$ELFS/$version.elf" && echo "ELF kept as $ELFS/$version.elf"
}

# cmd_flash_fw <port> [--no-storage] [build dir]: Lichen from firmware/build
cmd_flash_fw() {
    local port=$1 no_storage=${2:-} build=${3:-$REPO/firmware/build}
    require_espressif_port "$port"
    local args="$build/flash_args"
    [ -f "$args" ] || { echo "no $args, build first" >&2; exit 1; }
    # flash_args lists offset/file pairs relative to build/; mirror exactly those files.
    local files
    files=$(grep -oE '[^ ]+\.bin' "$args")
    local flash_args=@flash_args
    if [ "$no_storage" = --no-storage ]; then
        # Keep the storage partition (MeshCore, settings) by dropping its offset/file pair from the arguments
        files=$(grep -v 'storage.bin' <<<"$files" || true)
        grep -v 'storage.bin' "$args" > "$args.nostorage" || true
        flash_args=@flash_args.nostorage
    else
        echo "warning: this overwrites the storage partition (everything on the badge's flash drive); use --no-storage to keep it" >&2
    fi
    remote "rm -rf $REMOTE_DIR/images/p4 && mkdir -p $REMOTE_DIR/images/p4"
    # shellcheck disable=SC2086  # one word per file is intended
    (cd "$build" && tar cf - flash_args $files) | remote "tar xf - -C $REMOTE_DIR/images/p4"
    if [ "$no_storage" = --no-storage ]; then
        scp "$args.nostorage" "$HOST:$REMOTE_DIR/images/p4/flash_args.nostorage"
        rm -f "$args.nostorage"
    fi
    remote "cd $REMOTE_DIR/images/p4 && $VENV/bin/esptool --chip esp32p4 --port '$port' --baud $BAUD write-flash $flash_args"
    [ ! -f "$build/badge_fw.elf" ] || archive_elf "$build"
}

# Fetches the core dump with "fw crash dump", checks size and CRC32, decodes it against the ELF of the crashed image:
# found by the ELF SHA the dump names first, by the running version second (older ELFs were kept by version only)
cmd_crash() {
    local port=$1 elf=${2:-}
    require_espressif_port "$port"
    local tmp
    tmp=$(mktemp -d)
    # shellcheck disable=SC2064
    trap "rm -rf '$tmp'" RETURN
    local try
    for try in 1 2 3; do
        remote "$VENV/bin/python $REMOTE_DIR/console_session.py '$port' 1 60 'fw crash dump@2'" | sed 's/^ *[0-9.]* //' | tr -d '\r' > "$tmp/session.txt"
        python3 -I - "$tmp/session.txt" "$tmp/core.bin" "$tmp/version" "$tmp/elfsha" <<'PY' && break
import base64, re, sys, zlib
lines = open(sys.argv[1]).read().split("\n")
begin = next((i for i, l in enumerate(lines) if l.startswith("--- coredump begin ")), None)
end = next((i for i, l in enumerate(lines) if l.startswith("--- coredump end ")), None)
if begin is None or end is None or end < begin:
    sys.exit("no core dump in the reply (fw crash says: " + "; ".join(l for l in lines if l.startswith("fw: crash"))[:200] + ")")
size = int(lines[begin].split()[3])
crc = int(lines[end].split()[3], 16)
try:
    raw = base64.b64decode("".join(l for l in lines[begin + 1:end] if re.fullmatch(r"[A-Za-z0-9+/=]+", l)))
except ValueError as e:
    sys.exit("not valid base64 (%s)" % e)
if len(raw) != size:
    sys.exit("size mismatch: got %d bytes, the badge said %d (retry)" % (len(raw), size))
if zlib.crc32(raw) != crc:
    sys.exit("CRC32 mismatch: got %08x, the badge said %08x (retry)" % (zlib.crc32(raw), crc))
open(sys.argv[2], "wb").write(raw)
version = next((l.split()[3] for l in lines if l.startswith("fw: crash: version ")), "")
open(sys.argv[3], "w").write(version)
elfsha = next((l.split()[3] for l in lines if l.startswith("fw: crash: elf ")), "")
elfsha = elfsha if re.fullmatch(r"[0-9a-f]+", elfsha) else ""
open(sys.argv[4], "w").write(elfsha)
print("core dump: %d bytes, CRC32 %08x ok, crashed image ELF %s, running version %s" % (size, crc, elfsha or "?", version or "?"))
PY
        echo "retrying ($try/3)" >&2
    done
    [ -s "$tmp/core.bin" ] || exit 1
    if [ -n "$elf" ]; then
        echo "ELF: $elf (given)"
    else
        local sha match f
        sha=$(cat "$tmp/elfsha")
        match=""
        if [ -n "$sha" ]; then
            for f in "$ELFS"/sha-"$sha"*.elf; do [ -f "$f" ] && match=$f && break; done
            # ELFs kept before the SHA names (version only): hash them
            if [ -z "$match" ]; then
                for f in "$ELFS"/*.elf; do
                    [ -f "$f" ] && sha256sum "$f" | grep -q "^$sha" && match=$f && break
                done
            fi
        fi
        if [ -n "$match" ]; then
            elf=$match
            echo "ELF: $elf (by the crashed image's SHA $sha)"
        else
            elf="$ELFS/$(cat "$tmp/version").elf"
            echo "ELF: $elf (by the running version: no ELF kept for SHA ${sha:-?}; wrong if the badge rolled back)" >&2
        fi
    fi
    [ -f "$elf" ] || { echo "no ELF $elf (flash-fw and ota-firmware keep one per SHA and version; or pass the ELF as 2nd argument)" >&2; exit 1; }
    # shellcheck disable=SC1091
    ( . "$IDF/export.sh" >/dev/null 2>&1; esp-coredump --chip esp32p4 info_corefile -t raw -c "$tmp/core.bin" "$elf" )
}

cmd_restore() {
    local port=$1 file=$2 yes=$3
    require_espressif_port "$port"
    [ -f "$file" ] || { echo "no such file: $file" >&2; exit 1; }
    [ "$yes" = --yes ] || { echo "refusing: this overwrites the whole flash on $port; add --yes to confirm" >&2; exit 1; }
    [ -f "$file.sha256" ] || { echo "refusing: no $file.sha256 next to the backup to verify it with" >&2; exit 1; }
    (cd "$(dirname "$file")" && sha256sum -c "$(basename "$file").sha256" >/dev/null) ||
        { echo "refusing: $file does not match its .sha256" >&2; exit 1; }
    # Backups are named badge-<chip>-...: the chip on the port has to be the one the image came from
    local want have
    want=$(basename "$file")
    want=${want#badge-}
    want=${want%%-*}
    have=$(chip_of "$port")
    [ -n "$have" ] && [ "$have" = "$want" ] ||
        { echo "refusing: $file is a '$want' backup but the chip on $port is '${have:-unknown}'" >&2; exit 1; }
    remote "mkdir -p $REMOTE_DIR/images"
    scp "$file" "$HOST:$REMOTE_DIR/images/restore.bin"
    esptool --port "$port" --baud "$BAUD" write-flash 0 "$REMOTE_DIR/images/restore.bin"
}

cmd_monitor() {
    local port=$1
    require_espressif_port "$port"
    ssh -t "$HOST" "$VENV/bin/python -m serial.tools.miniterm --raw '$port' 115200"
}

# Writes one line to the console UART without resetting the badge and prints what comes back within two seconds.
# The CH340 drives EN from RTS and BOOT from DTR, each only while the other is released. The kernel asserts both
# at open (harmless together); pyserial would then release DTR first and leave RTS pulling EN, a reset. So DTR is
# left alone at open (dsrdtr), RTS is released first and DTR after.
cmd_console() {
    local port=$1 line=$2
    require_espressif_port "$port"
    case $line in *"'"*|*$'\n'*|*$'\r'*) echo "refusing: no quotes or line breaks in a console line" >&2; exit 1;; esac
    remote "$VENV/bin/python - '$port' '$line'" <<'PY'
import sys, time, serial
port, line = sys.argv[1], sys.argv[2]
s = serial.Serial()
s.port, s.baudrate, s.timeout = port, 115200, 0.2
s.dsrdtr = True
s.rts = False
s.open()
s.dtr = False
s.reset_input_buffer()
s.write((line + "\n").encode())
s.flush()
end = time.time() + 2.0
out = b""
while time.time() < end:
    out += s.read(4096)
s.close()
sys.stdout.write(out.decode("utf-8", "replace"))
PY
}

OTA_PORT=${BADGE_OTA_PORT:-8080}

ota_token() {
    local token=${BADGE_OTA_TOKEN:-}
    local file="${XDG_CONFIG_HOME:-$HOME/.config}/why2025-badge/ota-token"
    if [ -z "$token" ] && [ -r "$file" ]; then
        token=$(tr -d '[:space:]' < "$file")
    fi
    [[ $token =~ ^[0-9a-fA-F]{32}$ ]] ||
        { echo "no OTA token: run 'fw token' on the badge console and put the token in $file (or BADGE_OTA_TOKEN)" >&2; exit 1; }
    echo "$token"
}

# curl with the token passed through a config on stdin, so it never shows up in ps
ota_curl() {
    local token
    token=$(ota_token)
    printf 'header = "Authorization: Bearer %s"\n' "$token" |
        curl --config - -sS --fail-with-body --connect-timeout 5 "$@"
}

# Whether the receiver at <ip> takes compressed bodies (it says "deflate" in /info); asked once per run
OTA_DEFLATE=
ota_deflate() {
    if [ -z "$OTA_DEFLATE" ]; then
        OTA_DEFLATE=no
        if [ "${BADGE_OTA_COMPRESS:-1}" != 0 ] && curl -s --connect-timeout 5 "http://$1:$OTA_PORT/info" | grep -q deflate; then
            OTA_DEFLATE=yes
        fi
    fi
    [ "$OTA_DEFLATE" = yes ]
}

# Whether the receiver takes uploads in pieces (it says "chunks" in /info); asked once per run
OTA_CHUNKS=
ota_chunks() {
    if [ -z "$OTA_CHUNKS" ]; then
        OTA_CHUNKS=no
        if curl -s --connect-timeout 5 "http://$1:$OTA_PORT/info" | grep -q chunks; then
            OTA_CHUNKS=yes
        fi
    fi
    [ "$OTA_CHUNKS" = yes ]
}

OTA_PIECE=${BADGE_OTA_PIECE:-131072} # bytes per piece; a damaged piece is resent alone
OTA_PIECE_TRIES=${BADGE_OTA_PIECE_TRIES:-6}

# ota_put_pieces <ip> <route> <local file> [badge path]: upload in checked pieces, the whole file checked at the end
ota_put_pieces() {
    local ip=$1 route=$2 file=$3 path=${4:-} total file_sha piece piece_sha offset=0 index=0 try reply
    total=$(stat -c %s "$file")
    file_sha=$(sha256sum "$file" | cut -d' ' -f1)
    piece=$(mktemp)
    while [ "$offset" -lt "$total" ]; do
        dd if="$file" of="$piece" bs="$OTA_PIECE" skip="$index" count=1 status=none
        piece_sha=$(sha256sum "$piece" | cut -d' ' -f1)
        local args=(-H 'Expect:' -H "X-Content-SHA256: $piece_sha" -H "X-Offset: $offset" -H "X-Total-Size: $total"
                    -H "X-File-SHA256: $file_sha" -T "$piece")
        [ -n "$path" ] && args+=(-H "X-Path: $path")
        for ((try = 1; ; try++)); do
            # Damage on the badge's link depends on where bytes fall in the TCP stream (#40): a filler header of
            # varying length shifts the body, so a retry doesn't hit the same spot again
            local pad
            pad=$(printf '%*s' $((RANDOM % 16)) '' | tr ' ' x)
            if reply=$(ota_curl "${args[@]}" -H "X-Pad: x$pad" "http://$ip:$OTA_PORT/$route" 2>&1); then
                break
            fi
            if [ "$try" -ge "$OTA_PIECE_TRIES" ]; then
                rm -f -- "$piece"
                echo "giving up on the piece at $offset: $reply" >&2
                return 1
            fi
            echo "  piece at $offset failed ($try/$OTA_PIECE_TRIES), resending" >&2
        done
        offset=$((offset + $(stat -c %s "$piece")))
        index=$((index + 1))
    done
    rm -f -- "$piece"
    echo "$reply"
}

# ota_put <ip> <route> <local file> [badge path]
ota_put() {
    local ip=$1 route=$2 file=$3 path=${4:-} sha body
    [ -f "$file" ] || { echo "no such file: $file" >&2; exit 1; }
    if [ "$(stat -c %s "$file")" -gt "$OTA_PIECE" ] && ota_chunks "$ip"; then
        ota_put_pieces "$ip" "$route" "$file" "$path"
        return
    fi
    # The hash is of the file as it lands on the badge, compressed or not
    sha=$(sha256sum "$file" | cut -d' ' -f1)
    local args=(-H 'Expect:' -H "X-Content-SHA256: $sha")
    body=$file
    if ota_deflate "$ip"; then
        body=$(mktemp)
        python3 -c 'import sys, zlib; sys.stdout.buffer.write(zlib.compress(open(sys.argv[1], "rb").read(), 9))' "$file" > "$body"
        args+=(-H 'Content-Encoding: deflate')
    fi
    args+=(-T "$body")
    [ -n "$path" ] && args+=(-H "X-Path: $path")
    local status=0
    ota_curl "${args[@]}" "http://$ip:$OTA_PORT/$route" || status=$?
    [ "$body" = "$file" ] || rm -f -- "$body"
    return $status
}

# With the token the receiver also reports why the badge last started and its last crash
cmd_ota_info() { ota_curl "http://$1:$OTA_PORT/info"; }

cmd_ota_reboot() { ota_curl -X POST "http://$1:$OTA_PORT/reboot"; }

# Ask the badge to stop everything else (MeshCore, LoRa, screens) before the image starts, and wait for it
# to say "ready" (it prints the DMA RAM it has free then). Without /prepare in /info the push goes ahead as is.
ota_prepare() {
    local ip=$1 reply
    curl -s --connect-timeout 5 "http://$ip:$OTA_PORT/info" | grep -q prepare || return 0
    echo "preparing the badge: stopping MeshCore, LoRa and the screens..."
    reply=$(ota_curl --max-time 40 -X POST "http://$ip:$OTA_PORT/prepare" 2>&1) ||
        { echo "the badge did not get ready: $reply" >&2; return 1; }
    case $reply in
        ready*) echo "badge $reply" ;;
        *) echo "unexpected answer to prepare: $reply" >&2; return 1 ;;
    esac
}

cmd_ota_firmware() {
    local ip=$1 reboot=${2:-}
    ota_prepare "$ip" || exit 1
    local bin=${BADGE_FIRMWARE_BIN:-$REPO/firmware/build/badge_fw.bin}
    ota_put "$ip" firmware "$bin"
    [ ! -f "$(dirname "$bin")/badge_fw.elf" ] || archive_elf "$(dirname "$bin")"
    [ "$reboot" = --no-reboot ] || cmd_ota_reboot "$ip"
}

cmd_ota_file() { ota_put "$1" file "$2" "$3"; }

cmd_ota_rm() { ota_curl -X DELETE -H "X-Path: $2" "http://$1:$OTA_PORT/file"; }

case "${1:-}" in
    ports)    cmd_ports ;;
    backup)   [ $# -eq 2 ] || usage; cmd_backup "$2" ;;
    flash-fw) [ $# -ge 2 ] && [ $# -le 3 ] || usage; cmd_flash_fw "$2" "${3:-}" ;;
    crash)    [ $# -ge 2 ] && [ $# -le 3 ] || usage; cmd_crash "$2" "${3:-}" ;;
    restore)  [ $# -eq 4 ] || usage; cmd_restore "$2" "$3" "$4" ;;
    monitor)  [ $# -eq 2 ] || usage; cmd_monitor "$2" ;;
    console)      [ $# -eq 3 ] || usage; cmd_console "$2" "$3" ;;
    ota-info)     [ $# -eq 2 ] || usage; cmd_ota_info "$2" ;;
    ota-firmware) [ $# -ge 2 ] && [ $# -le 3 ] || usage; cmd_ota_firmware "$2" "${3:-}" ;;
    ota-file)     [ $# -eq 4 ] || usage; cmd_ota_file "$2" "$3" "$4" ;;
    ota-rm)       [ $# -eq 3 ] || usage; cmd_ota_rm "$2" "$3" ;;
    ota-reboot)   [ $# -eq 2 ] || usage; cmd_ota_reboot "$2" ;;
    *)        usage ;;
esac
