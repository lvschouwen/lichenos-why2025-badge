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
#   BADGE_ELFS     ELFs kept for crash decoding           (default: $BADGE_BACKUPS/elf)
#   BADGE_ELF_KEEP how many firmware images keep their ELF (default: 20; older ones are deleted)
#   BADGE_BAUD     esptool baud rate (default: 460800; 921600 corrupts reads over CH340)
#   BADGE_EXTRA_PORTS  space-separated non-Espressif ports that are known to be
#                      the badge, e.g. a CH340 bridge   (default: none)
#   BADGE_OTA_TOKEN    the badge's update token ("fw token") (default: read
#                      from ${XDG_CONFIG_HOME:-~/.config}/why2025-badge/ota-token)
#   BADGE_OTA_PORT     port of the badge's update server  (default: 8080)
#   BADGE_FIRMWARE_BIN image ota-firmware installs        (default: firmware/build/badge_fw.bin)
#   BADGE_OTA_COMPRESS 0 sends uncompressed even when the receiver takes deflate, whole
#                      bodies and pieces alike (default: 1)
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
  flash-fw <port> [--wipe-storage | --storage-image <bin>]
                            write firmware/build (Lichen: bootloader, partitions, app, assets) to the P4 and keep
                            the storage partition (MeshCore identity, contacts, settings); --wipe-storage also
                            writes storage.bin, which replaces everything on the badge's flash drive (first
                            install only); --storage-image writes <bin> there instead (exactly the partition's
                            size, a LittleFS image from tools/pack_storage.sh: the one-time USB move of a FAT
                            badge, #300). --no-storage is accepted as the old name of the default.
  crash <port> [elf]        fetch the core dump over the console ("fw crash dump"), check it and decode it with
                            the ELF kept by flash-fw/ota-firmware in $ELFS (or [elf])
  restore <port> <file> --yes
                            write a backup image back at offset 0 (checks the .sha256 and that the chip
                            matches the backup's name; --yes confirms overwriting the whole flash)
  monitor <port>            open a serial console: through the logger's socket while it runs (line by line, leave with
                            Ctrl-D), else miniterm with the logger paused (exit with Ctrl-])
  logger <install|start|stop|restart|status>
                            the serial logger on $HOST (tools/serial_log.py, writes ~/badge-logs/badge-serial.log there); install =
                            copy it + udev rule + service that start it when the badge connects (#276); it keeps the
                            port and serves ~/badge-logs/badge-console.sock: console, monitor and crash go through it
                            (sent lines logged as ">> ...", secrets masked) and pause it only when it does not run;
                            esptool commands (flash-fw, backup, restore, ports) always pause it (#324)
  console <port> <line>     send one line to the badge's console without resetting it and show the reply;
                            commands start with "fw " (try "fw help")

Read-only status (fw get, mem, boot, net, the log) goes over Wi-Fi while the badge is on Wi-Fi ("fw <ip>", "log <ip>"
below): it does not touch USB. Use the USB console for the rest: Wi-Fi down, early boot, thermal shed.

Over Wi-Fi, while the badge is on Wi-Fi. <host> is an address or the badge's name, lichen-xxxx.local (shown in
Settings and by "fw wifi"; needs mDNS on this machine, e.g. avahi/nss-mdns):
  ota-info <host>                       show the firmware version the badge runs
  ota-firmware <host> [--no-reboot]     install firmware/build/badge_fw.bin (or \$BADGE_FIRMWARE_BIN) and reboot into it
                                      (--no-reboot: the badge keeps running the old version until you reboot it,
                                      e.g. ota-reboot; after about 3 min without one it restarts into the new one)
  ota-file <host> <file> <badge-path>   push one file, e.g. 'FLASH0:[c6]firmware.bin'
  ota-assets <host> [--no-reboot]       write firmware/build/assets.bin (or \$BADGE_ASSETS_BIN) into the assets
                                      partition (fonts) and reboot (--no-reboot: as for ota-firmware)
  ota-c6 <host> [dir]                   push the C6 bundle (bootloader, partitions, firmware + .md5; default dir
                                      firmware/storage_staging/c6) in a safe order, reboot, wait for Wi-Fi
  ota-rm <host> <badge-path>            delete one file on the badge
  ota-reboot <host>
  log <ip> [--follow]                 print the badge's whole log (all pages; what its console prints, kept in RAM, with
                                      sequence numbers and uptimes); --follow keeps polling every second. Lines printed
                                      by ROM code (panics, the DPI underrun note) are not in it. It can show node names
                                      and messages: treat it like a screenshot
  crashes <ip>                        print the crash records stored on the badge (reason, task, pc, backtrace, firmware
                                      version; no RAM dump) over Wi-Fi, the same text as Crash log > Copy to SD card
  fw <ip> <command...>                run one console command over Wi-Fi and print its output, e.g.
                                      "fw 192.0.2.7 mem" ("fw " in front of the command is optional)
  screenshot <ip> [out.png]           save what the screen shows as a PNG (default: a private dir under \$XDG_RUNTIME_DIR or \$TMPDIR; it
                                      can show private messages, so keep it out of the repo)

Host only (touches no badge, no ssh):
  new-app <id> "<Name>"     scaffold a new app: firmware/components/lichen_<id>/ (model, screens, CMake), its host test
                            and the register, layer and test lines; refuses an id in use or a name with quotes (#385,
                            docs/app-api.md "An app in 10 minutes")

<port> is a /dev/serial/by-id/... path or /dev/ttyACMn as shown by 'ports'.
EOF
    exit 1
}

ELFS=${BADGE_ELFS:-$BACKUPS/elf}
ELF_KEEP=${BADGE_ELF_KEEP:-20}
IDF=${BADGE_IDF_PATH:-$HOME/esp/esp-idf-v6.1}

remote() {
    [ -n "$HOST" ] || { echo "no badge host: put its SSH name in ${XDG_CONFIG_HOME:-~/.config}/why2025-badge/host (or BADGE_HOST)" >&2; exit 1; }
    ssh "$HOST" "$@"
}
# Run a command on the badge host with the serial logger (tools/serial_log.py, #61) paused: it closes the port while
# ~/badge-logs/badge-serial.log.pause exists and confirms with .paused. The pause is removed when the remote shell exits
# (also on errors); if ssh dies, the logger ignores a pause older than 10 minutes.
PORT_PAUSE='P=$HOME/badge-logs/badge-serial.log; if pgrep -f "[s]erial_log.py" >/dev/null; then : > "$P.pause"; trap '"'"'rm -f "$P.pause"'"'"' EXIT; i=0; while [ ! -e "$P.paused" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i+1)); done; [ -e "$P.paused" ] || echo "warning: the serial logger did not release the port" >&2; fi;'
port_remote() { remote "$PORT_PAUSE $*"; }
# The console through the logger's socket (#324): run serial_log.py <client args> on the badge host while the socket
# exists; when the client says the socket cannot serve this port (exit 3: no logger, an old one, paused for a flash,
# lost, or another port), run the fallback with the logger paused instead, as before.
CONSOLE_SOCK='$HOME/badge-logs/badge-console.sock'
SOCKET_FIRST='if [ -S "'"$CONSOLE_SOCK"'" ]; then '"$VENV/bin/python $REMOTE_DIR"'/serial_log.py'
SOCKET_ELSE='; rc=$?; [ $rc -eq 3 ] || exit $rc; fi;'
socket_or_pause() {
    local client=$1
    shift
    remote "$SOCKET_FIRST $client$SOCKET_ELSE $PORT_PAUSE $*"
}
esptool() { port_remote "$VENV/bin/esptool $*"; }

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

# shellcheck source=tools/elf_archive.sh
. "$REPO/tools/elf_archive.sh"
# The allow list is always the repo's own file: no override from the caller's environment (#287)
unset onair_allow_override ONAIR_ALLOW_FILE
# shellcheck source=tools/onair_guard.sh
. "$REPO/tools/onair_guard.sh"

# Refuses a console line with a C0 control character other than tab, or DEL (#324). First, before onair_gate: the gate
# reads the line as typed, so "se<^A>nd" is not "send" to it, and nothing on the way may turn it into one. Line breaks
# are refused here too.
refuse_control_chars() {
    # counted, not captured: $(...) would drop a trailing newline
    if [ "$(printf '%s' "$1" | LC_ALL=C tr -d '\t' | LC_ALL=C tr -dc '\000-\037\177' | wc -c)" -ne 0 ]; then
        echo "refusing: control characters in a console line (only tab is allowed)" >&2
        exit 1
    fi
}

# Every console line passes here before it is sent (#287): on air only #badgetest and the test node. A line that
# changes what the badge floods (advert interval/manual) is refused too, with no override (an agent could set one):
# Lucas changes those in MeshCore Settings on the badge.
onair_gate() {
    local verdict
    verdict=$(onair_verdict "$1")
    case $verdict in
        deny\ *) echo "refusing: ${verdict#deny }" >&2; exit 1 ;;
        ask\ *) echo "refusing: ${verdict#ask } (MeshCore Settings on the badge)" >&2; exit 1 ;;
    esac
}

# Before an on-air line with a target (#290): $1 is the badge's answer to the scope query, read through the same path
# the line is about to take. Refused unless the badge's console scope is the allow file's scope line (no line, no
# answer or another scope: refused), so a console flood never travels further than the bench allows.
onair_scope_gate() {
    local verdict
    verdict=$(onair_scope_verdict "$1")
    case $verdict in
        pass) ;;
        *) echo "refusing: ${verdict#deny }" >&2; exit 1 ;;
    esac
}

# The storage partition's size from firmware/partitions.csv, in bytes
storage_partition_size() {
    awk -F, '{ for (i = 1; i <= NF; i++) gsub(/^[ \t]+|[ \t]+$/, "", $i) } $1 == "storage" { print $5 }' \
        "$REPO/firmware/partitions.csv" | head -1
}

# --storage-image <bin> (#300): <bin> must be exactly the partition's size and mount as the badge's LittleFS (the
# pack_storage list reads every file), so a FAT slice, a whole-flash backup or a cut copy is refused before the port
storage_image_ok() {
    local image=$1 want size
    [ -f "$image" ] || { echo "flash-fw: no storage image $image" >&2; return 1; }
    want=$(( $(storage_partition_size) ))
    size=$(stat -c %s "$image")
    [ "$size" = "$want" ] || { echo "flash-fw: $image is $size bytes, the storage partition is $want" >&2; return 1; }
    "$REPO/tools/pack_storage.sh" list "$image" >/dev/null ||
        { echo "flash-fw: $image is not a LittleFS image with the badge's config (tools/pack_storage.sh)" >&2; return 1; }
}

# cmd_flash_fw <port> [--wipe-storage|--storage-image <bin>|--no-storage] [build dir]: Lichen from firmware/build.
# Keeps the storage partition unless --wipe-storage is given (#183); --no-storage is the old name of the default.
# --storage-image <bin> writes <bin> into the storage partition instead of the built storage.bin: the one-time USB
# move of a FAT badge to LittleFS (#300), with the files packed by tools/pack_storage.sh.
cmd_flash_fw() {
    local port=$1 mode=${2:-} build=${3:-$REPO/firmware/build} image=""
    if [ "$mode" = --storage-image ]; then
        image=${3:-}
        build=${4:-$REPO/firmware/build}
        storage_image_ok "$image" || exit 1
    fi
    require_espressif_port "$port"
    local args="$build/flash_args"
    [ -f "$args" ] || { echo "no $args, build first" >&2; exit 1; }
    local wipe_storage=0
    case "$mode" in
        --wipe-storage) wipe_storage=1 ;;
        ''|--no-storage|--storage-image) ;;
        *) echo "flash-fw: unknown option $mode (use --wipe-storage to overwrite /storage)" >&2; exit 1 ;;
    esac
    # flash_args lists offset/file pairs relative to build/; mirror exactly those files.
    local files
    files=$(grep -oE '[^ ]+\.bin' "$args")
    local flash_args=@flash_args
    if [ "$wipe_storage" = 1 ]; then
        echo "warning: --wipe-storage overwrites the storage partition (everything on the badge's flash drive: MeshCore identity, contacts, channels, settings)" >&2
    else
        # Keep the storage partition (MeshCore, settings) by dropping its offset/file pair from the arguments
        files=$(grep -v 'storage.bin' <<<"$files" || true)
        grep -v 'storage.bin' "$args" > "$args.nostorage" || true
        flash_args=@flash_args.nostorage
        if [ -n "$image" ]; then
            # The given image at the storage partition's offset, the one flash_args gives storage.bin
            local offset
            offset=$(awk '$2 == "storage.bin" { print $1 }' "$args")
            [ -n "$offset" ] || { rm -f "$args.nostorage"; echo "flash-fw: no storage.bin offset in $args" >&2; exit 1; }
            echo "$offset storage-image.bin" >> "$args.nostorage"
            echo "warning: --storage-image replaces everything on the badge's flash drive with $image" >&2
        fi
    fi
    remote "rm -rf $REMOTE_DIR/images/p4 && mkdir -p $REMOTE_DIR/images/p4"
    # shellcheck disable=SC2086  # one word per file is intended
    (cd "$build" && tar cf - flash_args $files) | remote "tar xf - -C $REMOTE_DIR/images/p4"
    if [ "$wipe_storage" != 1 ]; then
        scp "$args.nostorage" "$HOST:$REMOTE_DIR/images/p4/flash_args.nostorage"
        rm -f "$args.nostorage"
    fi
    [ -z "$image" ] || scp "$image" "$HOST:$REMOTE_DIR/images/p4/storage-image.bin"
    port_remote "cd $REMOTE_DIR/images/p4 && $VENV/bin/esptool --chip esp32p4 --port '$port' --baud $BAUD write-flash $flash_args"
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
        # Socket: until the dump's end line (or 5 s quiet when there is none); console_session.py prefixes each line
        # with its time, which only its branch strips
        socket_or_pause "--session '$port' 'fw crash dump' 5 60 '--- coredump end '" \
            "$VENV/bin/python $REMOTE_DIR/console_session.py '$port' 1 60 'fw crash dump@2' | sed 's/^ *[0-9.]* //'" |
            tr -d '\r' > "$tmp/session.txt"
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

# The serial logger on the badge host (tools/serial_log.py -> ~/badge/serial_log.py, log in ~/badge-logs/badge-serial.log).
# A udev rule (tools/flash-host/99-why2025-badge.rules) starts badge-serial-log.service when the badge's CH340 appears,
# also at boot (#276). esptool users pause it (PORT_PAUSE); console users go through its socket (#324), so it can run
# all the time.
LOGGER_UNIT=badge-serial-log.service
cmd_logger() {
    case $1 in
        install)
            scp "$REPO/tools/serial_log.py" "$HOST:$REMOTE_DIR/serial_log.py" &&
                scp "$REPO/tools/flash-host/99-why2025-badge.rules" "$REPO/tools/flash-host/$LOGGER_UNIT.in" "$HOST:/tmp/" &&
                remote "set -e; sed -e \"s|@USER@|\$USER|g\" -e \"s|@HOME@|\$HOME|g\" /tmp/$LOGGER_UNIT.in > /tmp/$LOGGER_UNIT;
                        sudo install -m 644 /tmp/99-why2025-badge.rules /etc/udev/rules.d/99-why2025-badge.rules;
                        sudo install -m 644 /tmp/$LOGGER_UNIT /etc/systemd/system/$LOGGER_UNIT;
                        rm -f /tmp/99-why2025-badge.rules /tmp/$LOGGER_UNIT.in /tmp/$LOGGER_UNIT;
                        sudo systemctl daemon-reload; sudo udevadm control --reload;
                        for n in /sys/bus/usb-serial/drivers/ch341-uart/ttyUSB*; do
                            [ -e \"\$n\" ] && sudo udevadm trigger --action=add --name-match=\"\${n##*/}\"; done;
                        sudo udevadm settle; sudo systemctl start $LOGGER_UNIT;
                        sleep 2; systemctl is-active $LOGGER_UNIT; ls -l /dev/badge-console" &&
                echo "installed; a running logger picks up a new serial_log.py after: badge.sh logger restart" ;;
        start | stop | restart) remote "sudo systemctl $1 $LOGGER_UNIT && systemctl is-active $LOGGER_UNIT || true" ;;
        status)
            remote "systemctl --no-pager status $LOGGER_UNIT 2>&1 | head -n 4; P=\$HOME/badge-logs/badge-serial.log;
                    [ -e \$P.pause ] && echo 'pause requested'; [ -e \$P.paused ] && echo 'paused';
                    S=$CONSOLE_SOCK; if [ -S \"\$S\" ]; then echo \"console socket \$S:\";
                    cat \$HOME/badge-logs/badge-console.status 2>/dev/null; else echo 'no console socket'; fi;
                    tail -n 3 \$P 2>/dev/null" ;;
        *) usage ;;
    esac
}

cmd_monitor() {
    local port=$1
    require_espressif_port "$port"
    ssh -t "$HOST" "$SOCKET_FIRST --monitor '$port'$SOCKET_ELSE $PORT_PAUSE $VENV/bin/python -m serial.tools.miniterm --raw '$port' 115200"
}

# Writes one line to the console UART without resetting the badge and prints what comes back within two seconds.
# The CH340 drives EN from RTS and BOOT from DTR, each only while the other is released. The kernel asserts both
# at open (harmless together); pyserial would then release DTR first and leave RTS pulling EN, a reset. So DTR is
# left alone at open (dsrdtr), RTS is released first and DTR after.
cmd_console() {
    local port=$1 line=$2
    case $line in *$'\n'*|*$'\r'*) echo "refusing: no line breaks in a console line" >&2; exit 1;; esac
    refuse_control_chars "$line"
    onair_gate "$line"
    require_espressif_port "$port"
    if onair_needs_scope "$line"; then
        onair_scope_gate "$(console_line "$port" "$(onair_scope_query "$line")" || true)"
    fi
    console_line "$port" "$line"
}

# Through the logger's socket when it runs (the same two-second window), the pause path below otherwise: the client
# does not read stdin, so the script stays there for the fallback. The line travels base64-encoded through the remote
# shell, so quotes and $ in it arrive as typed. Both ends check the decoded line for control characters again.
console_line() {
    local port=$1 line=$2 line64
    refuse_control_chars "$line"
    line64=$(printf '%s' "$line" | base64 | tr -d '\n')
    socket_or_pause "--b64 --send '$port' '$line64' 2" "$VENV/bin/python - '$port' '$line64'" <<'PY'
import base64, sys, time, serial
port, line = sys.argv[1], base64.b64decode(sys.argv[2]).decode("utf-8")
if any(ch != "\t" and (ch < " " or ch == "\x7f") for ch in line):  # as serial_log.has_control (#324)
    sys.exit("refusing: control characters in a console line (only tab is allowed)")
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
    local rc=0
    printf 'header = "Authorization: Bearer %s"\n' "$token" |
        curl --config - -sS --fail-with-body --connect-timeout 5 "$@" || rc=$?
    [ $rc -eq 7 ] && echo "the badge's update server doesn't answer: is Wi-Fi up and Settings › Updates over Wi-Fi on, not closed by its auto-off? ('fw ota' / 'fw ota on' on the USB console)" >&2
    return $rc
}

# The receiver's /info line (with the token) up to its "; hw {...}" object (#321: hw values are data, not features, so
# no feature match may hit them), empty when it doesn't answer; no pipe into grep, so pipefail can't turn an
# early-closing grep into "feature missing"
ota_info_line() {
    local line
    line=$(ota_curl "http://$1:$OTA_PORT/info" || true)
    printf '%s\n' "${line%%; hw *}"
}

# Whether the receiver at <ip> takes compressed bodies (it says "deflate" in /info); asked once per run
OTA_DEFLATE=
ota_deflate() {
    if [ -z "$OTA_DEFLATE" ]; then
        OTA_DEFLATE=no
        if [ "${BADGE_OTA_COMPRESS:-1}" != 0 ] && [[ $(ota_info_line "$1") == *deflate* ]]; then
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
        if [[ $(ota_info_line "$1") == *chunks* ]]; then
            OTA_CHUNKS=yes
        fi
    fi
    [ "$OTA_CHUNKS" = yes ]
}

# Whether the receiver takes compressed pieces (it says "zpieces" in /info, #298); asked once per run
OTA_ZPIECES=
ota_zpieces() {
    if [ -z "$OTA_ZPIECES" ]; then
        OTA_ZPIECES=no
        if [ "${BADGE_OTA_COMPRESS:-1}" != 0 ] && [[ $(ota_info_line "$1") == *zpieces* ]]; then
            OTA_ZPIECES=yes
        fi
    fi
    [ "$OTA_ZPIECES" = yes ]
}

# ota_zpiece <route> <offset> <piece> <out>: write the piece as a zlib stream to <out> when the receiver takes one and
# it comes out smaller; false = send the piece as it is. The first firmware piece always goes plain: the badge checks
# the image header in it before it erases anything (#298).
ota_zpiece() {
    local route=$1 offset=$2 piece=$3 out=$4
    [ "$OTA_ZPIECES" = yes ] || return 1
    [ "$route" != firmware ] || [ "$offset" -gt 0 ] || return 1
    python3 -c 'import sys, zlib; sys.stdout.buffer.write(zlib.compress(open(sys.argv[1], "rb").read(), 9))' "$piece" > "$out" || return 1
    [ "$(stat -c %s "$out")" -lt "$(stat -c %s "$piece")" ]
}

# ota_room <ip> <bytes> [what]: stop with a plain message when the badge says (in /info, #299) that /storage can't take a
# file of <bytes>; an upload is written next to the old file before the swap, so it needs its own room plus a cluster.
# A receiver that doesn't say is let through: it answers 507 itself, or fails the write as before.
ota_room() {
    local ip=$1 bytes=$2 what=${3:-the file} line free need
    line=$(ota_info_line "$ip")
    [[ $line =~ storage\ free\ ([0-9]+) ]] || return 0
    free=${BASH_REMATCH[1]}
    need=$(( (bytes + 4095) / 4096 * 4096 + 4096 ))
    if [ "$free" -lt "$need" ]; then
        echo "not enough space on the badge's internal storage for $what: needs $need bytes, $free free." >&2
        echo "Nothing was sent. See what takes the room with 'fw $ip storage' and free some first." >&2
        return 1
    fi
}

# Whether a refused piece is worth sending again: the badge says "send it again" for damage in transit or in memory;
# anything else it answered (no room, wrong offset, bad path, ...) comes back the same, so it's reported at once (#299)
ota_piece_retry() {
    case $1 in
        *"send it again"*) return 0 ;;
        *"curl: (22)"*) return 1 ;; # an HTTP error the badge answered (--fail-with-body)
        *"curl: ("*) return 0 ;;    # no answer from the badge: the link, try again
        *) return 1 ;;
    esac
}

OTA_PIECE=${BADGE_OTA_PIECE:-131072} # bytes per piece; a damaged piece is resent alone
# At most 256 KiB: the badge drains a refused piece up to that size, so its reply isn't lost to a reset (#299)
[ "$OTA_PIECE" -le 262144 ] || OTA_PIECE=262144
OTA_PIECE_TRIES=${BADGE_OTA_PIECE_TRIES:-6}

# ota_put_pieces <ip> <route> <local file> [badge path]: upload in checked pieces, the whole file checked at the end.
# Pieces go compressed when the receiver takes that (#298); offsets, sizes and hashes stay those of the file itself.
ota_put_pieces() {
    local ip=$1 route=$2 file=$3 path=${4:-} total file_sha piece zpiece piece_sha offset=0 index=0 try reply snapshot
    local wire=0 started=$SECONDS size body
    ota_zpieces "$ip" || true
    # Work on a private copy: a rebuild during the push must not mix two images (#37)
    snapshot=$(mktemp)
    cp -- "$file" "$snapshot" || { rm -f -- "$snapshot"; return 1; }
    file=$snapshot
    total=$(stat -c %s "$file")
    file_sha=$(sha256sum "$file" | cut -d' ' -f1)
    piece=$(mktemp)
    zpiece=$(mktemp)
    while [ "$offset" -lt "$total" ]; do
        dd if="$file" of="$piece" bs="$OTA_PIECE" skip="$index" count=1 status=none
        piece_sha=$(sha256sum "$piece" | cut -d' ' -f1)
        size=$(stat -c %s "$piece")
        local args=(-H 'Expect:' -H "X-Content-SHA256: $piece_sha" -H "X-Offset: $offset" -H "X-Total-Size: $total"
                    -H "X-File-SHA256: $file_sha")
        body=$piece
        if ota_zpiece "$route" "$offset" "$piece" "$zpiece"; then
            body=$zpiece
            args+=(-H 'Content-Encoding: deflate' -H "X-Piece-Size: $size")
        fi
        args+=(-T "$body")
        [ -n "$path" ] && args+=(-H "X-Path: $path")
        # The piece that completes an install says whether the badge may restart by itself (#298)
        [ "$OTA_HOLD" = yes ] && [ $((offset + size)) -ge "$total" ] && args+=(-H 'X-Reboot: hold')
        for ((try = 1; ; try++)); do
            # Damage on the badge's link depends on where bytes fall in the TCP stream (#40): a filler header of
            # varying length shifts the body, so a retry doesn't hit the same spot again
            local pad
            pad=$(printf '%*s' $((RANDOM % 16)) '' | tr ' ' x)
            if reply=$(ota_curl "${args[@]}" -H "X-Pad: x$pad" "http://$ip:$OTA_PORT/$route" 2>&1); then
                break
            fi
            if ! ota_piece_retry "$reply"; then
                rm -f -- "$piece" "$zpiece" "$snapshot"
                echo "the badge refused the piece at $offset: $reply" >&2
                return 1
            fi
            if [ "$try" -ge "$OTA_PIECE_TRIES" ]; then
                rm -f -- "$piece" "$zpiece" "$snapshot"
                echo "giving up on the piece at $offset: $reply" >&2
                return 1
            fi
            echo "  piece at $offset failed ($try/$OTA_PIECE_TRIES), resending" >&2
        done
        wire=$((wire + $(stat -c %s "$body")))
        offset=$((offset + size))
        index=$((index + 1))
    done
    rm -f -- "$piece" "$zpiece" "$snapshot"
    echo "$reply"
    # The time split of an update (#298): what went over the air and how long all pieces took
    echo "sent $total bytes in $index pieces, $wire on the wire, in $((SECONDS - started)) s"
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
    [ "$OTA_HOLD" = yes ] && args+=(-H 'X-Reboot: hold')
    local status=0
    ota_curl "${args[@]}" "http://$ip:$OTA_PORT/$route" || status=$?
    [ "$body" = "$file" ] || rm -f -- "$body"
    return $status
}

# With the token the receiver also reports why the badge last started and its last crash
cmd_ota_info() { ota_curl "http://$1:$OTA_PORT/info"; }

cmd_ota_reboot() { ota_curl -X POST "http://$1:$OTA_PORT/reboot"; }

# After an install the badge restarts by itself about 2 s later when no reboot came (#298): the reboot is sent at
# once, and a badge that is already restarting is what was asked for: connection refused (7), or closed or cut
# without a reply (52, 56). Any other failure (no such host, a timeout, an error reply) is reported.
ota_reboot_installed() {
    local reply rc=0
    reply=$(cmd_ota_reboot "$1" 2>&1) || rc=$?
    case $rc in
        0) echo "$reply" ;;
        7 | 52 | 56) echo "the badge restarts by itself" ;;
        *) echo "$reply" >&2; return 1 ;;
    esac
}

# --no-reboot (#298): the request that completes the install says X-Reboot: hold, so the badge waits for a reboot
OTA_HOLD=

# Ask the badge to stop everything else (MeshCore, LoRa, screens) before the image starts, and wait for it
# to say "ready" (it prints the DMA RAM it has free then). Without /prepare in /info the push goes ahead as is.
ota_prepare() {
    local ip=$1 reply
    [[ $(ota_info_line "$ip") == *prepare* ]] || return 0
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
    [ "$reboot" != --no-reboot ] || OTA_HOLD=yes
    ota_prepare "$ip" || exit 1
    local bin=${BADGE_FIRMWARE_BIN:-$REPO/firmware/build/badge_fw.bin}
    ota_put "$ip" firmware "$bin"
    [ "$reboot" = --no-reboot ] || ota_reboot_installed "$ip"
    [ ! -f "$(dirname "$bin")/badge_fw.elf" ] || archive_elf "$(dirname "$bin")"
}

cmd_ota_file() {
    [ -f "$2" ] || { echo "no such file: $2" >&2; exit 1; }
    ota_room "$1" "$(stat -c %s "$2")" "$2" || exit 1
    ota_put "$1" file "$2" "$3"
}

# The assets partition (#22): always sent in checked pieces (the receiver takes nothing else there), after /prepare
# and followed by a reboot, because the running firmware reads the partition in place
cmd_ota_assets() {
    local ip=$1 reboot=${2:-}
    [ "$reboot" != --no-reboot ] || OTA_HOLD=yes
    local bin=${BADGE_ASSETS_BIN:-$REPO/firmware/build/assets.bin}
    [ -f "$bin" ] || { echo "no $bin, build first" >&2; exit 1; }
    ota_chunks "$ip" || { echo "this receiver can't take assets (no pieces support)" >&2; exit 1; }
    # The partition is read in place (memory-mapped): stop the apps and screens before it is rewritten
    ota_prepare "$ip" || exit 1
    ota_put_pieces "$ip" assets "$bin" || exit 1
    [ "$reboot" = --no-reboot ] || ota_reboot_installed "$ip"
}

# GET /screenshot (#30, #146): the screen as a 16-bit BMP header plus run/literal packets (firmware/components/ota/
# ota_rle.h), decoded here and converted to PNG with python3-pil
cmd_screenshot() {
    local ip=$1 out=${2:-} work
    umask 077 # the picture can show private messages: nobody else reads it
    work=$(mktemp -d "${XDG_RUNTIME_DIR:-${TMPDIR:-/tmp}}/badge-shot.XXXXXX")
    # shellcheck disable=SC2064  # expand now: the temp paths are fixed when the trap is set
    trap "rm -f '$work/shot.bmp'; rmdir '$work' 2>/dev/null" EXIT # the dir stays when it holds the default PNG
    [ -n "$out" ] || out=$work/screenshot.png
    if ! ota_curl --max-time 20 -o "$work/shot.bmp" "http://$ip:$OTA_PORT/screenshot"; then
        echo "screenshot: no picture from the badge within 20 s (a slow link, another screenshot running, or the update server is off)" >&2
        head -c 300 "$work/shot.bmp" 2>/dev/null | tr -d '\000' >&2 # --fail-with-body: the badge's reason is in the file
        echo >&2
        exit 1
    fi
    python3 -I - "$work/shot.bmp" "$out" <<'PY' || exit 1
import io, struct, sys
from PIL import Image
d = open(sys.argv[1], "rb").read()
if d[:2] != b"BM" or len(d) < 66:
    sys.exit("screenshot: not a picture from the badge")
off, w, h = struct.unpack_from("<I", d, 10)[0], struct.unpack_from("<i", d, 18)[0], abs(struct.unpack_from("<i", d, 22)[0])
want, px, i = w * h, bytearray(), off
while i < len(d) and len(px) < want * 2:
    c = d[i]; i += 1
    if c < 0x80:
        px += d[i:i + 2 * (c + 1)]; i += 2 * (c + 1)
    else:
        if c == 0xFF:
            n = 128 + struct.unpack_from("<H", d, i)[0]; i += 2
        else:
            n = (c & 0x7F) + 1
        px += d[i:i + 2] * n; i += 2
if len(px) != want * 2 or i != len(d):
    sys.exit("screenshot: the picture was cut short (%d of %d pixels)" % (len(px) // 2, want))
bmp = bytearray(d[:off]) + px
struct.pack_into("<I", bmp, 2, len(bmp))
Image.open(io.BytesIO(bytes(bmp))).convert("RGB").save(sys.argv[2])
PY
    echo "$out"
}

# Console text from the radio (node names, messages) must not drive this terminal: drop C0 controls except \n and \t
printable() {
    LC_ALL=C tr -d '\000-\010\013-\037\177'
}

# GET /log (#38): lines after a sequence number; the badge answers the number to ask for next in X-Next-Seq. A smaller
# number than asked (the badge restarted) just starts over.
cmd_log() {
    local ip=$1 follow=${2:-} since=0 hdr body next missed code work full
    umask 077
    work=$(mktemp -d "${XDG_RUNTIME_DIR:-${TMPDIR:-/tmp}}/badge-log.XXXXXX")
    # shellcheck disable=SC2064  # expand now: the temp paths are fixed when the trap is set
    trap "rm -f '$work/hdr' '$work/body'; rmdir '$work' 2>/dev/null" EXIT
    hdr=$work/hdr body=$work/body
    while :; do
        if ! code=$(ota_curl --max-time 60 -D "$hdr" -o "$body" -w '%{http_code}' "http://$ip:$OTA_PORT/log?since=$since"); then
            head -c 300 "$body" 2>/dev/null | printable >&2
            # A wrong or missing token will not get better: stop instead of polling for ever
            if [ "$code" = 401 ] || [ "$code" = 403 ]; then
                exit 1
            fi
            [ -n "$follow" ] || exit 1
            sleep 2 # Wi-Fi hiccup or a reboot: keep trying
            continue
        fi
        printable < "$body"
        next=$(tr -d '\r' < "$hdr" | sed -n 's/^[Xx]-[Nn]ext-[Ss]eq: *\([0-9][0-9]*\)$/\1/p' | tail -n 1)
        missed=$(tr -d '\r' < "$hdr" | sed -n 's/^[Xx]-[Ll]og-[Mm]issed: *\([0-9][0-9]*\)$/\1/p' | tail -n 1)
        if [ "${missed:-0}" != 0 ]; then
            echo "-- $missed older line(s) were already dropped from the badge's log --" >&2
        fi
        # A full answer (24 KB cap) means more is waiting: ask again at once
        full=0
        [ "$(wc -c < "$body")" -lt 20000 ] || full=1
        if [ -z "$follow" ]; then
            # Drain: stop on a short page or when the badge stops advancing the number
            { [ "$full" = 1 ] && [ -n "$next" ] && [ "$next" != "$since" ]; } || break
        elif [ "$full" = 0 ]; then
            sleep 1
        fi
        if [ -n "$next" ]; then
            since=$next
        fi
    done
}

# GET /crashes (#170): the stored crash records as text. Nothing is sent from the badge unless this is run.
cmd_crashes() {
    local ip=$1 work
    umask 077
    work=$(mktemp -d "${XDG_RUNTIME_DIR:-${TMPDIR:-/tmp}}/badge-crashes.XXXXXX")
    # shellcheck disable=SC2064  # expand now: the temp paths are fixed when the trap is set
    trap "rm -f '$work/body'; rmdir '$work' 2>/dev/null" EXIT
    if ! ota_curl --max-time 20 -o "$work/body" "http://$ip:$OTA_PORT/crashes"; then
        head -c 300 "$work/body" 2>/dev/null | printable >&2
        exit 1
    fi
    printable < "$work/body"
}

# POST /console (#38): one fw command; the badge runs it like the UART console and answers with its output
cmd_fw() {
    local ip=$1
    shift
    local line="$*"
    refuse_control_chars "$line"
    onair_gate "$line"
    if onair_needs_scope "$line"; then
        onair_scope_gate "$(fw_post "$ip" "$(onair_scope_query "$line")" || true)"
    fi
    fw_post "$ip" "$line"
}

# One line to /console, its answer on stdout
fw_post() {
    local ip=$1 line=$2 work rc=0
    # In a private file, not on the command line: the line can hold a Wi-Fi password (the token goes in on stdin)
    umask 077
    work=$(mktemp "${XDG_RUNTIME_DIR:-${TMPDIR:-/tmp}}/badge-fw.XXXXXX")
    # shellcheck disable=SC2064  # expand now: the temp paths are fixed when the trap is set
    trap "rm -f '$work'" EXIT
    printf '%s' "$line" > "$work"
    ota_curl --max-time 60 -X POST -H 'Content-Type: text/plain' --data-binary "@$work" "http://$ip:$OTA_PORT/console" |
        printable || rc=$?
    rm -f "$work"
    return $rc
}

# The C6 bundle (#56): the P4 flashes the C6 at boot from /storage/c6 and refuses a .bin whose .md5 differs (the C6
# then keeps its old image and the boot goes on without it), so every .bin goes up right before its own .md5 and
# nothing reboots until all six files are there. Small files first, the 1 MB firmware last: a dropped connection
# then costs the least. The ota_server clears the C6 bundle key on any /storage/c6 write, so the next boot does the
# full check and reflashes the C6.
C6_BUNDLE_FILES=(partitions bootloader firmware)

c6_check_local() {
    local dir=$1 name bin md5 want size
    [ -d "$dir" ] || { echo "no directory $dir (build first)" >&2; return 1; }
    for name in "${C6_BUNDLE_FILES[@]}"; do
        bin=$dir/$name.bin md5=$dir/$name.bin.md5
        [ -f "$bin" ] && [ -f "$md5" ] || { echo "missing $bin or $md5" >&2; return 1; }
        size=$(wc -c < "$bin")
        [ "$size" -ge 1024 ] && [ "$size" -le 4194304 ] ||
            { echo "$bin has an implausible size ($size bytes)" >&2; return 1; }
        want=$(md5sum "$bin" | cut -d' ' -f1)
        [ "$(tr -d '[:space:]' < "$md5")" = "$want" ] ||
            { echo "$md5 does not match $bin (stale build?): nothing was sent" >&2; return 1; }
    done
}

cmd_ota_c6() {
    local ip=$1 dir=${2:-$REPO/firmware/storage_staging/c6} name f i
    c6_check_local "$dir" || exit 1
    # Checked before the first file, so a full drive can't leave a mixed bundle behind (#299): the files are swapped
    # in one at a time, so the largest one has to fit
    local largest
    largest=$(stat -c %s "$dir"/*.bin | sort -n | tail -1)
    ota_room "$ip" "$largest" "the C6 bundle (largest file $largest bytes)" || exit 1
    # No /prepare here: after it the receiver refuses file pushes (409) until the next boot
    for name in "${C6_BUNDLE_FILES[@]}"; do
        for f in "$name.bin" "$name.bin.md5"; do
            echo "sending $f..."
            if ! ota_put "$ip" file "$dir/$f" "FLASH0:[c6]$f"; then
                echo "FAILED at $f. The badge holds a mixed C6 bundle and was NOT rebooted: it keeps running and its" >&2
                echo "C6 is untouched. Run ota-c6 again (all six files are rewritten); do not reboot it before that works." >&2
                echo "If a reboot happened anyway and Wi-Fi stays away, use the USB rescue (flash-fw, which keeps /storage)." >&2
                exit 1
            fi
        done
    done
    echo "all six files are on the badge; rebooting (the C6 is reflashed at boot, about 1-2 minutes)..."
    cmd_ota_reboot "$ip" || true
    sleep 20 # let it go down before polling
    for i in $(seq 1 36); do
        if ota_curl --connect-timeout 3 "http://$ip:$OTA_PORT/info" > /dev/null 2>&1; then
            echo "Wi-Fi is back after about $((20 + (i - 1) * 5)) s: check with 'fw $ip c6'"
            return 0
        fi
        sleep 5
    done
    echo "no answer from the badge after 200 s: do not push anything else; check its screen and the serial log" >&2
    exit 1
}

cmd_ota_rm() { ota_curl -X DELETE -H "X-Path: $2" "http://$1:$OTA_PORT/file"; }

case "${1:-}" in
    ports)    cmd_ports ;;
    backup)   [ $# -eq 2 ] || usage; cmd_backup "$2" ;;
    flash-fw)
        if [ "${3:-}" = --storage-image ]; then
            [ $# -eq 4 ] || usage
            cmd_flash_fw "$2" "$3" "$4"
        else
            [ $# -ge 2 ] && [ $# -le 3 ] || usage
            cmd_flash_fw "$2" "${3:-}"
        fi ;;
    crash)    [ $# -ge 2 ] && [ $# -le 3 ] || usage; cmd_crash "$2" "${3:-}" ;;
    restore)  [ $# -eq 4 ] || usage; cmd_restore "$2" "$3" "$4" ;;
    monitor)  [ $# -eq 2 ] || usage; cmd_monitor "$2" ;;
    console)      [ $# -eq 3 ] || usage; cmd_console "$2" "$3" ;;
    logger)       [ $# -eq 2 ] || usage; cmd_logger "$2" ;;
    ota-info)     [ $# -eq 2 ] || usage; cmd_ota_info "$2" ;;
    ota-firmware) [ $# -ge 2 ] && [ $# -le 3 ] || usage; cmd_ota_firmware "$2" "${3:-}" ;;
    ota-file)     [ $# -eq 4 ] || usage; cmd_ota_file "$2" "$3" "$4" ;;
    ota-assets)   [ $# -ge 2 ] && [ $# -le 3 ] || usage; cmd_ota_assets "$2" "${3:-}" ;;
    ota-c6)       [ $# -ge 2 ] && [ $# -le 3 ] || usage; cmd_ota_c6 "$2" "${3:-}" ;;
    ota-rm)       [ $# -eq 3 ] || usage; cmd_ota_rm "$2" "$3" ;;
    ota-reboot)   [ $# -eq 2 ] || usage; cmd_ota_reboot "$2" ;;
    log)          { [ $# -eq 2 ] || { [ $# -eq 3 ] && [ "$3" = --follow ]; }; } || usage; cmd_log "$2" "${3:-}" ;;
    crashes)      [ $# -eq 2 ] || usage; cmd_crashes "$2" ;;
    fw)           [ $# -ge 3 ] || usage; ip=$2; shift 2; cmd_fw "$ip" "$@" ;;
    new-app)      [ $# -eq 3 ] || usage; exec python3 -I "$REPO/tools/new_app.py" "$2" "$3" ;; # host only: never reaches the badge (#385)
    screenshot)   [ $# -ge 2 ] && [ $# -le 3 ] || usage; cmd_screenshot "$2" "${3:-}" ;;
    *)        usage ;;
esac
