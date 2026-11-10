#!/usr/bin/env bash
# tools/pack_storage.py in its own Python environment with the pinned littlefs-python (pack_storage-requirements.txt,
# every wheel by SHA-256), made on first use in firmware/build/littlefs_venv (LICHEN_LFS_VENV overrides it). That first
# use downloads the wheel from PyPI (a fresh build dir, or a changed requirements file): it needs network once. The build makes storage.bin with it;
# the USB move of a FAT badge to LittleFS (#300) packs the files taken off the old drive the same way:
#   tools/pack_storage.sh pack <src dir> <out.bin>      then      tools/pack_storage.sh verify <out.bin> <src dir>
# Every other pack_storage.py command (list, extract, check-config, selftest) passes through as well.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
repo=$(dirname "$here")
venv=${LICHEN_LFS_VENV:-$repo/firmware/build/littlefs_venv}
req=$here/pack_storage-requirements.txt
stamp=$venv/.requirements

if ! cmp -s "$req" "$stamp" 2>/dev/null; then
    mkdir -p "$(dirname "$venv")"
    python3 -m venv --clear "$venv"
    # Hashes only, wheels only: nothing unpinned is fetched, not even a build backend for an sdist
    "$venv/bin/python" -m pip install --quiet --disable-pip-version-check --require-hashes --only-binary=:all: -r "$req"
    cp "$req" "$stamp"
fi
exec "$venv/bin/python" -I "$here/pack_storage.py" "$@"
