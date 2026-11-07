# shellcheck shell=bash
# ELF archive for core-dump decoding (#74, #194), sourced by badge.sh and tools/tests/elf_archive_test.sh.
# Needs ELFS (archive dir); ELF_KEEP (default 20) is how many images stay.

# Keeps the ELF of the firmware just built under its SHA-256 and under its version, so a core dump from it can be
# decoded later. The SHA-256 of the ELF file is what the image carries as its "ELF file SHA256" and what a core dump
# names the crashed image by: after a rollback the running version is not the crashed one, the SHA still is.
# The version name is a hardlink to the SHA file, so one image costs one copy.
archive_elf() {
    local build=$1 version sha
    mkdir -p "$ELFS"
    sha=$(sha256sum "$build/badge_fw.elf" | cut -d' ' -f1)
    if [ -f "$ELFS/sha-$sha.elf" ]; then
        touch "$ELFS/sha-$sha.elf"
    else
        cp "$build/badge_fw.elf" "$ELFS/sha-$sha.elf"
    fi
    echo "ELF kept as $ELFS/sha-$sha.elf"
    if version=$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1] + "/project_description.json"))["project_version"])' "$build"); then
        ln -f "$ELFS/sha-$sha.elf" "$ELFS/$version.elf" && echo "ELF kept as $ELFS/$version.elf"
    else
        echo "warning: no project_version in $build, ELF kept by SHA only" >&2
    fi
    prune_elfs
}

# Keeps the newest ELF_KEEP images (by mtime) and deletes the rest. Names sharing an inode are one image.
prune_elfs() {
    local keep=${ELF_KEEP:-20}
    [ -d "$ELFS" ] || return 0
    find "$ELFS" -maxdepth 1 -type f -name '*.elf' -printf '%T@ %i %p\n' | sort -rn |
        awk -v keep="$keep" '{ ino = $2; path = substr($0, length($1) + length($2) + 3)
                               if (!(ino in rank)) rank[ino] = ++n
                               if (rank[ino] > keep) print path }' |
        while IFS= read -r old; do rm -f -- "$old"; done
}
