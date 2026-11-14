#!/usr/bin/env bash
# Builds Lichen: ./firmware/build.sh build (any idf.py arguments). A build first builds the C6 image (c6/) and runs
# the host tests (host_tests/), so the P4 image always carries a matching C6 image.
# ./firmware/build.sh release build: the release image (#373), sdkconfig.defaults plus sdkconfig.release.defaults
# (no debug or test commands) in firmware/build-release with its own sdkconfig; the dev build's tree is not touched.
# The full check (./firmware/build.sh build) ends with that release build and its checks (#401), so a release-only
# break fails the gate. BADGE_SKIP_RELEASE=1 (exactly 1) leaves it out on purpose and then exits 3 at the end, so agent-check never
# stamps a partial check as green (the dev image is still fully built and checked by then).
set -euo pipefail

# --plan (first argument) prints which steps this invocation would run and exits: for tests/build_plan_test.sh only,
# it changes nothing about a real run.
plan=0
if [ "${1:-}" = --plan ]; then
    plan=1
    shift
fi

release=0
if [ "${1:-}" = release ]; then
    release=1
    shift
fi

# --nested-from-check: private to the release build the developer build ends with (#401): static checks, host tests and
# the C6 image have just run, so they are not repeated. An argument, not an environment variable, so nothing left in
# a shell can thin out a real check; only the release path accepts it.
nested=0
args=()
for arg in "$@"; do
    if [ "$arg" = --nested-from-check ]; then nested=1; else args+=("$arg"); fi
done
set -- ${args[@]+"${args[@]}"}
if [ "$nested" = 1 ] && [ "$release" = 0 ]; then
    echo "--nested-from-check is only for the release build the full check starts itself (build.sh release build)" >&2
    exit 2
fi
case "${BADGE_SKIP_RELEASE:-}" in
    ''|1) ;;
    *) echo "BADGE_SKIP_RELEASE must be 1 or unset, not '$BADGE_SKIP_RELEASE'" >&2; exit 2 ;;
esac
if [ "$plan" = 1 ]; then
    echo "prechecks=$([ "$nested" = 1 ] && echo no || echo yes) release_step=$([ "$release" = 0 ] && [ -z "${BADGE_SKIP_RELEASE:-}" ] && echo yes || echo no)"
    exit 0
fi

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

bdir=$here/build
sdkconfig=$here/sdkconfig
idf_args=()
if [ "$release" = 1 ]; then
    bdir=$here/build-release
    sdkconfig=$bdir/sdkconfig
    mkdir -p "$bdir"
    idf_args=(-B "$bdir" -D "SDKCONFIG=$sdkconfig" -D "SDKCONFIG_DEFAULTS=$here/sdkconfig.defaults;$here/sdkconfig.release.defaults")
fi

# Only commands that build need the C6 image, the host tests and fresh sdkconfigs; monitor, menuconfig etc. don't
building=0
for arg in "$@"; do
    case $arg in
        build|all|app|flash|reconfigure|app-flash|storage|storage-flash) building=1 ;;
    esac
done

# PSRAM at 120 MHz (#94) is not an IDF option: it needs tools/idf-patches/ applied to the IDF checkout. Without it
# Kconfig would drop the unknown symbol and fall back to the 200 MHz default, which flips bits on this badge.
if grep -q '^CONFIG_SPIRAM_SPEED_120M=y' "$here/sdkconfig.defaults" &&
   ! grep -q 'SPIRAM_SPEED_120M' "$IDF_DIR/components/esp_psram/esp32p4/Kconfig.spiram"; then
    echo "sdkconfig.defaults asks for PSRAM at 120 MHz but $IDF_DIR is not patched for it:" >&2
    echo "  git -C $IDF_DIR apply $repo/tools/idf-patches/0001-esp32p4-psram-120mhz.patch" >&2
    exit 1
fi

# shellcheck disable=SC1091
. "$IDF_DIR/export.sh" >/dev/null

# ccache (#196): shared by every worktree slot, so paths are hashed relative to the checkout it builds
if command -v ccache >/dev/null; then
    export IDF_CCACHE_ENABLE=${IDF_CCACHE_ENABLE:-1}
    export CCACHE_BASEDIR=${CCACHE_BASEDIR:-$repo} CCACHE_NOHASHDIR=${CCACHE_NOHASHDIR:-1}
    host_launcher=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
else
    echo "note: ccache not installed, builds run uncached (#196)" >&2
    host_launcher=()
fi

if [ "$building" = 1 ]; then
    # sdkconfig is generated from sdkconfig.defaults once and then kept, so later edits to the defaults would
    # silently have no effect: set a stale one aside (as sdkconfig.old) so it is regenerated
    for dir in "$here" "$repo/c6"; do
        if [ -f "$dir/sdkconfig" ] && [ "$dir/sdkconfig.defaults" -nt "$dir/sdkconfig" ]; then
            echo "$dir/sdkconfig.defaults is newer than $dir/sdkconfig, regenerating it (old one: sdkconfig.old)"
            mv -f "$dir/sdkconfig" "$dir/sdkconfig.old"
        fi
    done
    if [ "$release" = 1 ] && [ -f "$sdkconfig" ] &&
       { [ "$here/sdkconfig.defaults" -nt "$sdkconfig" ] || [ "$here/sdkconfig.release.defaults" -nt "$sdkconfig" ]; }; then
        echo "the release defaults are newer than $sdkconfig, regenerating it (old one: sdkconfig.old)"
        mv -f "$sdkconfig" "$bdir/sdkconfig.old"
    fi

    # Static checks, host tests and the C6 image come first (#196); the release build the full check ends with
    # (--nested-from-check) has just had all of them
    if [ "$nested" = 0 ]; then
        "$repo/tools/static_checks.sh"

        cmake -S "$repo/host_tests" -B "$here/build/host_tests" -G Ninja "${host_launcher[@]}" >/dev/null
        cmake --build "$here/build/host_tests"
        ctest --test-dir "$here/build/host_tests" --output-on-failure
        python3 -I "$repo/tools/ui_overflow_sweep.py" --selftest # the sweep's guard logic, no badge needed (#133)

        IDF_TARGET=esp32c6 idf.py -C "$repo/c6" build
        "$here/tools/stage_storage.sh"
    fi
fi

# The release image's own checks (#373). The dev build's gates (layers, the size ratchet, .clangd) read and write
# firmware/build and its baselines, so they are left to the dev build.
release_checks() {
    python3 -I "$repo/tools/check_console_slots.py" --sdkconfig "$sdkconfig"
    "$repo/tools/pack_storage.sh" check-config "$bdir/config/sdkconfig.json" "$here/managed_components/joltwallet__littlefs"
    # No debug command may reach the image: the config symbols are off and only debug code holds the marker
    # (tools/check_release_image.sh, #401)
    "$repo/tools/check_release_image.sh" config "$sdkconfig"
    "$repo/tools/check_release_image.sh" release "$bdir/badge_fw.bin"
}

if [ "$building" = 1 ] && [ "$release" = 1 ]; then
    idf.py -C "$here" "${idf_args[@]}" "$@"
    for arg in "$@"; do
        case $arg in build|all|app) release_checks; break ;; esac
    done
    BADGE_IDF_PATH="$IDF_DIR" python3 "$repo/tools/third_party_licenses.py" --check
elif [ "$building" = 1 ]; then
    idf.py -C "$here" "$@"
    # Layers: requires, includes and undefined symbols of every component against tools/layers.conf, ratcheted by
    # tools/layers-baseline.txt (#218; the app boundary of #178 is its L6 profile)
    # The symbol rule needs freshly compiled objects: only commands that compile the app get it
    syms=--no-symbols
    for arg in "$@"; do
        case $arg in build|all|app|flash|app-flash) syms= ;; esac
    done
    "$repo/tools/check_layers.sh" $syms
    # /storage is LittleFS (#300): the image tool round-trips a test tree, its config matches the one this build gave
    # the badge, and storage.bin holds exactly the staged files (list + SHA-256)
    "$repo/tools/pack_storage.sh" selftest
    "$repo/tools/pack_storage.sh" check-config "$here/build/config/sdkconfig.json" "$here/managed_components/joltwallet__littlefs"
    for arg in "$@"; do
        case $arg in
            build|all|storage) "$repo/tools/pack_storage.sh" verify "$here/build/storage.bin" "$here/storage_staging"; break ;;
        esac
    done
    # The console table must keep headroom (#243); reads the sdkconfig this build just wrote
    python3 -I "$repo/tools/check_console_slots.py" --selftest
    python3 -I "$repo/tools/check_console_slots.py"
    # The release check is only worth something while the developer image holds the marker it looks for (#401)
    for arg in "$@"; do
        case $arg in build|all|app) "$repo/tools/check_release_image.sh" dev "$bdir/badge_fw.bin"; break ;; esac
    done
    # Size, complexity and stack standard with its ratchet baseline (#202); needs the image just built, so only the
    # commands that build it (reconfigure and storage do not)
    for arg in "$@"; do
        case $arg in build|all|app) "$repo/tools/check_standard.sh"; break ;; esac
    done
    "$repo/tools/gen_clangd.sh" # .clangd for clangd/LSP over this build's compile_commands.json (#196)
    # Released images ship this file; it must match the dependencies just built (#18)
    BADGE_IDF_PATH="$IDF_DIR" python3 "$repo/tools/third_party_licenses.py" --check
    # The release image (#373) in firmware/build-release, last so its own failure message is the one left on screen
    # (#401: a release-only break slipped through twice, #396 and #437). Only commands that build the image run it.
    for arg in "$@"; do
        case $arg in
            build|all|app)
                if [ -n "${BADGE_SKIP_RELEASE:-}" ]; then
                    echo "BADGE_SKIP_RELEASE set: the release build was skipped, this is not a full check (exit 3)" >&2
                    exit 3
                else
                    echo "== release build (#401) =="
                    "$here/build.sh" release "$@" --nested-from-check
                fi
                break ;;
        esac
    done
else
    exec idf.py -C "$here" "${idf_args[@]}" "$@"
fi
