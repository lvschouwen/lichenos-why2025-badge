#!/usr/bin/env bash
# Stages Lichen's FAT drive: the C6 image that the P4 flashes to the C6 at boot (each file with an .md5 holding just
# the hex digest, which the flasher compares), and lora.toml. firmware/build.sh builds the C6 image (c6/) first.
set -euo pipefail

repo=$(cd "$(dirname "$0")/../.." && pwd)
c6=$repo/c6/build
out=$repo/firmware/storage_staging

declare -A src=(
    [bootloader.bin]=$c6/bootloader/bootloader.bin
    [partitions.bin]=$c6/partition_table/partition-table.bin
    [firmware.bin]=$c6/network_adapter.bin
)
for f in "${!src[@]}"; do
    [ -f "${src[$f]}" ] || { echo "missing ${src[$f]}: build the C6 image first (firmware/build.sh build)" >&2; exit 1; }
done

mkdir -p "$out/c6"
for f in "${!src[@]}"; do
    cp "${src[$f]}" "$out/c6/$f"
    md5sum "$out/c6/$f" | cut -d' ' -f1 | tr -d '\n' > "$out/c6/$f.md5"
done
cp "$repo/firmware/lora.toml" "$out/lora.toml"
