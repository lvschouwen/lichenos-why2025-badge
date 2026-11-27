#!/usr/bin/env python3
"""Refuse a RAM image that could touch flash or leave the v3 kernel's RAM window (#614, limits from #611).

    v3_load_ram_check.py <image.elf|image.bin>

Accepts an ELF32 (program headers) or an esptool image (what `elf2image` writes). Exit 0 when every segment fits,
1 with one reason per line on stderr otherwise.

  L2MEM      0x4FF00000-0x4FFC0000   every segment, bss included
  loaded     0x4FF10000-0x4FF3AFC0   every byte the image writes (image start up to the ROM stack)
  flash      0x40000000-0x4BFFFFFF and 0x80000000 up  named in the message; both are outside L2MEM anyway
"""
import struct
import sys

L2_LO, L2_HI = 0x4FF00000, 0x4FFC0000
LOAD_LO, LOAD_HI = 0x4FF10000, 0x4FF3AFC0
MAX_SEGMENTS = 16


def flash_mapped(a: int) -> bool:
    return 0x40000000 <= a <= 0x4BFFFFFF or a >= 0x80000000


def check_range(what: str, lo: int, size: int, win_lo: int, win_hi: int, win_name: str, errs: list) -> None:
    hi = lo + size
    if size == 0:
        return
    if flash_mapped(lo) or flash_mapped(hi - 1):
        errs.append(f"{what}: 0x{lo:08x}+0x{size:x} is in the flash-mapped range")
    elif lo < win_lo or hi > win_hi:
        errs.append(f"{what}: 0x{lo:08x}-0x{hi:08x} is outside {win_name} 0x{win_lo:08x}-0x{win_hi:08x}")


def check_elf(d: bytes, errs: list) -> None:
    if d[4] != 1 or d[5] != 1:
        errs.append("ELF is not 32-bit little-endian")
        return
    (phoff,) = struct.unpack_from("<I", d, 28)
    phentsize, phnum = struct.unpack_from("<HH", d, 42)
    if phentsize < 32 or phnum > 64 or phoff + phentsize * phnum > len(d):
        errs.append("ELF program header table is malformed")
        return
    loads = 0
    for i in range(phnum):
        ptype, off, vaddr, paddr, filesz, memsz = struct.unpack_from("<IIIIII", d, phoff + i * phentsize)
        if ptype != 1:
            continue
        loads += 1
        if off + filesz > len(d):
            errs.append(f"segment {i}: file data runs past the end of the file")
        for name, addr in (("vaddr", vaddr), ("paddr", paddr)):
            check_range(f"segment {i} {name}", addr, memsz, L2_LO, L2_HI, "L2MEM", errs)
            check_range(f"segment {i} loaded {name}", addr, filesz, LOAD_LO, LOAD_HI, "the loaded window", errs)
    if loads == 0:
        errs.append("ELF has no loadable segment")


def check_bin(d: bytes, errs: list) -> None:
    if len(d) < 24:
        errs.append("image is shorter than its header")
        return
    nseg = d[1]
    (entry,) = struct.unpack_from("<I", d, 4)
    if nseg == 0 or nseg > MAX_SEGMENTS:
        errs.append(f"image claims {nseg} segments")
        return
    check_range("entry", entry, 1, LOAD_LO, LOAD_HI, "the loaded window", errs)
    pos = 24
    for i in range(nseg):
        if pos + 8 > len(d):
            errs.append(f"segment {i}: header is cut off")
            return
        addr, size = struct.unpack_from("<II", d, pos)
        if pos + 8 + size > len(d):
            errs.append(f"segment {i}: data runs past the end of the file")
            return
        check_range(f"segment {i}", addr, size, LOAD_LO, LOAD_HI, "the loaded window", errs)
        pos += 8 + size


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    try:
        with open(sys.argv[1], "rb") as f:
            d = f.read()
    except OSError as e:
        print(f"cannot read image: {e}", file=sys.stderr)
        return 1
    errs: list = []
    if d[:4] == b"\x7fELF" and len(d) >= 52:
        check_elf(d, errs)
    elif d[:1] == b"\xe9":
        check_bin(d, errs)
    else:
        errs.append("not an ELF and not an esptool image")
    for e in errs:
        print(f"refused: {e}", file=sys.stderr)
    return 1 if errs else 0


if __name__ == "__main__":
    sys.exit(main())
