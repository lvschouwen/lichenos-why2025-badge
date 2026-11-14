#!/usr/bin/env bash
# check_release_image.sh <release|dev> <image.bin> (#401): the debug-build marker (console.c holds it only with
# CONFIG_LICHEN_DEV_COMMANDS) must be absent from a release image and present in a developer image. The second half
# keeps the first from going vacuous if the marker is ever optimised away or renamed. --selftest runs both on fakes.
set -euo pipefail

MARKER="LICHEN-DEBUG-BUILD-MARKER" # keep in step with lichen_debug_build_marker in fw_console/console.c

check() {
    local kind=$1 image=$2
    [ -f "$image" ] || { echo "$kind image $image does not exist" >&2; return 1; }
    case $kind in
        release)
            if grep -aqF "$MARKER" "$image"; then
                echo "release image $image holds the debug-build marker: LICHEN_DEV_COMMANDS is not off" >&2
                return 1
            fi
            echo "release image: no developer commands ($image)" ;;
        dev)
            if ! grep -aqF "$MARKER" "$image"; then
                echo "developer image $image lacks the debug-build marker: the release check would pass on anything" >&2
                return 1
            fi
            echo "developer image: debug-build marker present ($image)" ;;
        *) echo "usage: $0 <release|dev> <image.bin> | --selftest" >&2; return 2 ;;
    esac
}

selftest() {

    tmp=$(mktemp -d); trap 'rm -r "$tmp"' EXIT
    # Ordinary UI text that is always built must not trip the release check (the #401 false positive)
    printf 'junk\0Switch developer mode off? dev [on|off|status]\0more' > "$tmp/clean.bin"
    printf 'junk\0%s\0more' "$MARKER" > "$tmp/marked.bin"
    check release "$tmp/clean.bin" >/dev/null || { echo "selftest: clean release image rejected" >&2; exit 1; }
    ! check release "$tmp/marked.bin" 2>/dev/null || { echo "selftest: marked release image accepted" >&2; exit 1; }
    check dev "$tmp/marked.bin" >/dev/null || { echo "selftest: marked dev image rejected" >&2; exit 1; }
    ! check dev "$tmp/clean.bin" 2>/dev/null || { echo "selftest: unmarked dev image accepted" >&2; exit 1; }
    ! check release "$tmp/missing.bin" 2>/dev/null || { echo "selftest: missing image accepted" >&2; exit 1; }
    echo "check_release_image selftest ok"
}

if [ "${1:-}" = --selftest ]; then selftest; else check "${1:-}" "${2:-}"; fi
