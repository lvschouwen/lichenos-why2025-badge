#!/usr/bin/env bash
# Builds Lichen: ./firmware/build.sh build (any idf.py arguments). A build first builds the C6 image (c6/) and runs
# the host tests (host_tests/), so the P4 image always carries a matching C6 image.
set -euo pipefail

IDF_DIR=${BADGE_IDF_PATH:-~/esp/esp-idf}
REQUIRED_IDF=6.1

idf_ver_file="$IDF_DIR/tools/cmake/version.cmake"
[ -f "$idf_ver_file" ] || { echo "$IDF_DIR is not an esp-idf checkout (set BADGE_IDF_PATH)" >&2; exit 1; }
idf_major=$(sed -n 's/^set(IDF_VERSION_MAJOR \([0-9]*\))/\1/p' "$idf_ver_file")
idf_minor=$(sed -n 's/^set(IDF_VERSION_MINOR \([0-9]*\))/\1/p' "$idf_ver_file")
if [ "$idf_major.$idf_minor" != "$REQUIRED_IDF" ]; then
    echo "$IDF_DIR is esp-idf $idf_major.$idf_minor but the firmware needs $REQUIRED_IDF.x (set BADGE_IDF_PATH)" >&2
    exit 1
fi

here=$(cd "$(dirname "$0")" && pwd)
repo=$(dirname "$here")

# Only commands that build need the C6 image, the host tests and fresh sdkconfigs; monitor, menuconfig etc. don't
building=0
for arg in "$@"; do
    case $arg in
        build|all|app|flash|reconfigure|app-flash|storage|storage-flash) building=1 ;;
    esac
done

# shellcheck disable=SC1091
. "$IDF_DIR/export.sh" >/dev/null

if [ "$building" = 1 ]; then
    # sdkconfig is generated from sdkconfig.defaults once and then kept, so later edits to the defaults would
    # silently have no effect: set a stale one aside (as sdkconfig.old) so it is regenerated
    for dir in "$here" "$repo/c6"; do
        if [ -f "$dir/sdkconfig" ] && [ "$dir/sdkconfig.defaults" -nt "$dir/sdkconfig" ]; then
            echo "$dir/sdkconfig.defaults is newer than $dir/sdkconfig, regenerating it (old one: sdkconfig.old)"
            mv -f "$dir/sdkconfig" "$dir/sdkconfig.old"
        fi
    done

    cmake -S "$repo/host_tests" -B "$here/build/host_tests" -G Ninja >/dev/null
    cmake --build "$here/build/host_tests"
    ctest --test-dir "$here/build/host_tests" --output-on-failure

    IDF_TARGET=esp32c6 idf.py -C "$repo/c6" build
    "$here/tools/stage_storage.sh"
fi

if [ "$building" = 1 ]; then
    idf.py -C "$here" "$@"
    # Released images ship this file; it must match the dependencies just built (#18)
    BADGE_IDF_PATH="$IDF_DIR" python3 "$repo/tools/third_party_licenses.py" --check
else
    exec idf.py -C "$here" "$@"
fi
