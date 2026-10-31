#!/usr/bin/env python3
"""Packs a directory into an assets image for the "assets" partition (#22).

  tools/pack_assets.py <dir> <out.bin> [--max-size BYTES]

Every regular file under <dir> goes in, named by its path relative to <dir> with "/" separators, sorted, so the same
directory always gives the same image. The layout is described in firmware/components/assets/assets_fmt.h; this is
its only writer, host_tests/assets_fmt_test.c checks that both sides agree.
"""

import argparse
import struct
import sys
import zlib
from pathlib import Path

FORMAT = 1
HEADER_LEN = 32
ENTRY_LEN = 48
NAME_MAX = 40
ALIGN = 16


def pack(root: Path) -> bytes:
    files = sorted(p for p in root.rglob("*") if p.is_file())
    names = [p.relative_to(root).as_posix() for p in files]
    for name in names:
        if not name or len(name.encode()) >= NAME_MAX:
            sys.exit(f"asset name must be 1..{NAME_MAX - 1} bytes: {name!r}")
    offset = HEADER_LEN + len(files) * ENTRY_LEN
    table, blobs = b"", b""
    for name, path in zip(names, files):
        pad = -offset % ALIGN
        blobs += b"\0" * pad
        offset += pad
        data = path.read_bytes()
        table += name.encode().ljust(NAME_MAX, b"\0") + struct.pack("<II", offset, len(data))
        blobs += data
        offset += len(data)
    body = table + blobs
    total = HEADER_LEN + len(body)
    header = b"LCHA" + struct.pack("<HHIII", FORMAT, 0, len(files), total, zlib.crc32(body)) + bytes(12)
    return header + body


def main() -> None:
    parser = argparse.ArgumentParser(description="Pack a directory into an assets image")
    parser.add_argument("dir", type=Path)
    parser.add_argument("out", type=Path)
    parser.add_argument("--max-size", type=lambda s: int(s, 0), default=0, help="partition size; fail when larger")
    args = parser.parse_args()
    image = pack(args.dir)
    if args.max_size and len(image) > args.max_size:
        sys.exit(f"assets image is {len(image)} bytes, the partition holds {args.max_size}")
    args.out.write_bytes(image)
    count = struct.unpack_from("<I", image, 8)[0]
    print(f"assets: {len(image)} bytes, {count} files -> {args.out}")


if __name__ == "__main__":
    main()
