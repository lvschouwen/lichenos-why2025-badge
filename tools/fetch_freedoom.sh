#!/usr/bin/env bash
# Fetch the pinned Freedoom release and copy its two IWADs to an SD card for
# the DOOM app (#515): <card>/lichen/apps/doom/freedoom{1,2}.wad.
#
# Freedoom is BSD-3-Clause (see COPYING.txt in the zip). The WADs are never
# committed; the zip is downloaded from the official GitHub release, checked
# against the SHA-256 below (also listed in the release's signed CHECKSUM file)
# and cached, so repeat runs do not download again.
set -euo pipefail

VERSION="0.13.0"
SHA256="3f9b264f3e3ce503b4fb7f6bdcb1f419d93c7b546f4df3e874dd878db9688f59"
URL="https://github.com/freedoom/freedoom/releases/download/v${VERSION}/freedoom-${VERSION}.zip"

usage() {
    echo "usage: $0 <card-mount-dir>" >&2
    echo "  copies freedoom1.wad and freedoom2.wad (Freedoom ${VERSION}) to <card-mount-dir>/lichen/apps/doom/" >&2
    exit 2
}

[ $# -eq 1 ] || usage
card="$1"
[ -d "$card" ] || { echo "error: $card is not a directory (mount the SD card first)" >&2; exit 1; }

cache="${XDG_CACHE_HOME:-$HOME/.cache}/lichen"
zip="$cache/freedoom-${VERSION}.zip"
mkdir -p "$cache"

check_hash() {
    echo "$SHA256  $1" | sha256sum --check --status
}

if [ ! -f "$zip" ] || ! check_hash "$zip"; then
    rm -f "$zip.part"
    echo "downloading $URL"
    curl --fail --location --silent --show-error --output "$zip.part" "$URL"
    if ! check_hash "$zip.part"; then
        rm -f "$zip.part"
        echo "error: SHA-256 mismatch for the downloaded zip, refusing to use it" >&2
        exit 1
    fi
    mv "$zip.part" "$zip"
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
unzip -q -j "$zip" "freedoom-${VERSION}/freedoom1.wad" "freedoom-${VERSION}/freedoom2.wad" -d "$work"

dest="$card/lichen/apps/doom"
mkdir -p "$dest"
cp "$work/freedoom1.wad" "$work/freedoom2.wad" "$dest/"
sync
echo "Freedoom ${VERSION} copied to $dest"
