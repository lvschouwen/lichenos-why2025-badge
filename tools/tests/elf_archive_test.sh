#!/usr/bin/env bash
# Host test for tools/elf_archive.sh (#194): one copy per image, archive capped at ELF_KEEP images.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=tools/elf_archive.sh
. "$here/../elf_archive.sh"
tmp=$(mktemp -d); trap 'rm -r "$tmp"' EXIT
ELFS=$tmp/elf ELF_KEEP=3
fail() { echo "FAIL: $*" >&2; exit 1; }

mkdir -p "$ELFS"
# five older images: three hardlinked pairs and two lone version-only files from before #194
for i in 1 2 3; do
    echo "old$i" > "$ELFS/sha-old$i.elf"; ln "$ELFS/sha-old$i.elf" "$ELFS/fw-old$i.elf"
    touch -d "@$((1000000000 + i))" "$ELFS/sha-old$i.elf"
done
for i in 4 5; do echo "old$i" > "$ELFS/fw-old$i.elf"; touch -d "@$((1000000000 + i))" "$ELFS/fw-old$i.elf"; done

build=$tmp/build; mkdir -p "$build"
echo "new image" > "$build/badge_fw.elf"
echo '{"project_version": "fw-new"}' > "$build/project_description.json"
sha=$(sha256sum "$build/badge_fw.elf" | cut -d' ' -f1)
archive_elf "$build" >/dev/null

[ -f "$ELFS/sha-$sha.elf" ] && [ -f "$ELFS/fw-new.elf" ] || fail "new ELF not kept under both names"
[ "$(stat -c %i "$ELFS/sha-$sha.elf")" = "$(stat -c %i "$ELFS/fw-new.elf")" ] || fail "version name is a second copy, not a hardlink"
# kept: new + the two newest older images (old5, old4); old3..old1 and their pairs gone
expected="fw-new.elf fw-old4.elf fw-old5.elf sha-$sha.elf"
got=$(find "$ELFS" -mindepth 1 -printf '%f\n' | sort | tr '\n' ' ' | sed 's/ $//')
[ "$got" = "$expected" ] || fail "kept '$got', expected '$expected'"

# re-archiving the same build adds nothing and keeps it newest
archive_elf "$build" >/dev/null
[ "$(find "$ELFS" -mindepth 1 | wc -l)" = 4 ] || fail "re-archiving the same build changed the archive"
echo "elf_archive_test: ok"
