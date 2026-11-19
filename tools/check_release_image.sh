#!/usr/bin/env bash
# check_release_image.sh <release|dev> <image.bin> | config <sdkconfig> | --selftest (#401).
# release: the debug-build and bench-open markers (console.c holds it only with CONFIG_LICHEN_DEV_COMMANDS) and two older debug-only
#   wordings must be absent from a release image. dev: both markers must be present in a developer image, so the release
#   check cannot go vacuous if the marker is optimised away or renamed. config: the release sdkconfig has every
#   debug-command symbol off. --selftest runs all three on fakes.
set -euo pipefail

MARKER="LICHEN-DEBUG-BUILD-MARKER" # keep in step with lichen_debug_build_marker in fw_console/console.c
# The bench-open switch's own marker (#488, lichen_bench_open_marker, only with CONFIG_LICHEN_BENCH_OPEN): a developer
# image must hold it too, so the release check cannot go vacuous for this switch either
BENCH_MARKER="LICHEN-BENCH-OPEN-MARKER"
# Canaries beside the marker: console wordings that only exist with the debug commands (fw dev, the hidden-commands
# note), and the mesh identity test arm (#455: "mesh ident simulate", which blocks TX). Not UI text; "developer mode" itself is, and is left out on purpose.
CANARIES=("fw dev on" "debug ones hidden" "ident simulate" "test twdt")
# Every Kconfig symbol that compiles a debug or test command (grep "config LICHEN_" firmware/components/*/Kconfig*):
# LICHEN_NVS_ENCRYPT_BURN_CMD (irreversible eFuse burn) and LICHEN_NVS_DEBUG_FORCE_DOWN (fw nvs-down) are commands;
# LICHEN_UI_LOCK_CHECK_COUNT and _ASSERT are the LVGL lock-owner debug checks (release carries _OFF).
# ESP_SYSTEM_USE_FRAME_POINTER (#474) is no command but a developer-build cost (code size and speed): the release
# config pins it off.
# Not listed, release-neutral: LICHEN_CLOCK_PSRAM_RECORD, LICHEN_DISPLAY_DIRECT_FLUSH*, LICHEN_SENSORS_*, LICHEN_NVS_*.
DEBUG_SYMBOLS=(LICHEN_DEV_COMMANDS LICHEN_BENCH_OPEN LICHEN_NVS_ENCRYPT_BURN_CMD LICHEN_NVS_DEBUG_FORCE_DOWN
               LICHEN_UI_LOCK_CHECK_COUNT LICHEN_UI_LOCK_CHECK_ASSERT ESP_SYSTEM_USE_FRAME_POINTER)

usage() { echo "usage: $0 <release|dev> <image.bin> | config <sdkconfig> | --selftest" >&2; return 2; }

check_image() {
    local kind=$1 image=$2 word
    [ -f "$image" ] || { echo "$kind image $image does not exist" >&2; return 1; }
    if [ "$kind" = release ]; then
        for word in "$MARKER" "$BENCH_MARKER" "${CANARIES[@]}"; do
            if grep -aqF "$word" "$image"; then
                echo "release image $image holds \"$word\": LICHEN_DEV_COMMANDS or LICHEN_BENCH_OPEN is not off" >&2
                return 1
            fi
        done
        echo "release image: no developer commands ($image)"
    else
        for word in "$MARKER" "$BENCH_MARKER"; do
            if ! grep -aqF "$word" "$image"; then
                echo "developer image $image lacks \"$word\": the release check would pass on anything" >&2
                return 1
            fi
        done
        echo "developer image: debug-build and bench-open markers present ($image)"
    fi
}

check_config() {
    local file=$1 sym bad=0
    [ -f "$file" ] || { echo "sdkconfig $file does not exist" >&2; return 1; }
    # Absence alone would pass an empty or unrelated file: the release config always carries the "is not set" line
    if ! grep -q '^CONFIG_' "$file" || ! grep -qxF '# CONFIG_LICHEN_DEV_COMMANDS is not set' "$file"; then
        echo "$file is not a release sdkconfig: no CONFIG_ lines or no \"# CONFIG_LICHEN_DEV_COMMANDS is not set\"" >&2
        return 1
    fi
    for sym in "${DEBUG_SYMBOLS[@]}"; do
        if grep -qE "^CONFIG_${sym}=(y|[1-9])" "$file"; then
            echo "release sdkconfig $file has CONFIG_$sym on" >&2
            bad=1
        fi
    done
    [ "$bad" = 0 ] || return 1
    echo "release sdkconfig: no debug-command symbol on ($file)"
}

run() {
    case ${1:-} in
        release|dev) [ $# -eq 2 ] || { usage; return 2; }; check_image "$1" "$2" ;;
        config) [ $# -eq 2 ] || { usage; return 2; }; check_config "$2" ;;
        *) usage ;;
    esac
}

selftest() {
    tmp=$(mktemp -d); trap 'rm -r "$tmp"' EXIT
    # Ordinary UI text that is always built must not trip the release check (the #401 false positive)
    printf 'junk\0Switch developer mode off? dev [on|off|status]\0more' > "$tmp/clean.bin"
    printf 'junk\0%s\0%s\0more' "$MARKER" "$BENCH_MARKER" > "$tmp/marked.bin"
    printf 'junk\0%s\0more' "$MARKER" > "$tmp/debug_only.bin" # a developer image whose bench-open marker went missing
    printf 'junk\0%s\0more' "$BENCH_MARKER" > "$tmp/bench_only.bin"
    printf 'junk\0debug ones hidden: fw dev\0more' > "$tmp/canary.bin"
    printf 'junk\0mesh chats | mesh ident simulate | mesh hist\0more' > "$tmp/ident.bin"
    ! run release "$tmp/ident.bin" 2>/dev/null || { echo "selftest: ident simulate release image accepted" >&2; exit 1; }
    run release "$tmp/clean.bin" >/dev/null || { echo "selftest: clean release image rejected" >&2; exit 1; }
    ! run release "$tmp/marked.bin" 2>/dev/null || { echo "selftest: marked release image accepted" >&2; exit 1; }
    ! run release "$tmp/bench_only.bin" 2>/dev/null || { echo "selftest: bench-open release image accepted" >&2; exit 1; }
    ! run dev "$tmp/debug_only.bin" 2>/dev/null || { echo "selftest: dev image without the bench-open marker accepted" >&2; exit 1; }
    ! run release "$tmp/canary.bin" 2>/dev/null || { echo "selftest: canary release image accepted" >&2; exit 1; }
    run dev "$tmp/marked.bin" >/dev/null || { echo "selftest: marked dev image rejected" >&2; exit 1; }
    ! run dev "$tmp/clean.bin" 2>/dev/null || { echo "selftest: unmarked dev image accepted" >&2; exit 1; }
    printf 'CONFIG_LICHEN_UI_LOCK_CHECK_OFF=y\n# CONFIG_LICHEN_DEV_COMMANDS is not set\n' > "$tmp/off.cfg"
    printf 'CONFIG_LICHEN_DEV_COMMANDS=y\n' > "$tmp/on.cfg"
    printf '# CONFIG_LICHEN_DEV_COMMANDS is not set\nCONFIG_ESP_SYSTEM_USE_FRAME_POINTER=y\n' > "$tmp/fp.cfg"
    printf '# CONFIG_LICHEN_DEV_COMMANDS is not set\nCONFIG_LICHEN_BENCH_OPEN=y\n' > "$tmp/bench.cfg"
    : > "$tmp/empty.cfg"; printf 'CONFIG_FOO=y\n' > "$tmp/other.cfg"
    ! run config "$tmp/empty.cfg" 2>/dev/null || { echo "selftest: empty config accepted" >&2; exit 1; }
    ! run config "$tmp/other.cfg" 2>/dev/null || { echo "selftest: config without the symbol accepted" >&2; exit 1; }
    printf '# CONFIG_LICHEN_DEV_COMMANDS is not set\nCONFIG_LICHEN_NVS_DEBUG_FORCE_DOWN=y\n' > "$tmp/on2.cfg"
    run config "$tmp/off.cfg" >/dev/null || { echo "selftest: clean config rejected" >&2; exit 1; }
    ! run config "$tmp/on.cfg" 2>/dev/null || { echo "selftest: dev commands config accepted" >&2; exit 1; }
    ! run config "$tmp/on2.cfg" 2>/dev/null || { echo "selftest: force-down config accepted" >&2; exit 1; }
    ! run config "$tmp/bench.cfg" 2>/dev/null || { echo "selftest: bench-open config accepted" >&2; exit 1; }
    ! run config "$tmp/fp.cfg" 2>/dev/null || { echo "selftest: frame-pointer config accepted" >&2; exit 1; }
    echo "check_release_image selftest ok"
}

if [ "${1:-}" = --selftest ]; then selftest; else run "$@"; fi
