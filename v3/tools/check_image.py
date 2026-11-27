#!/usr/bin/env python3
"""Check the v3 ESP32-P4 bring-up image is RAM-only and inside the limits (#611).

    check_image.py <kernel.elf> <kernel.bin>

ELF: every PT_LOAD with file content lies in the load window [LOAD_BASE, ROM_STACK_TOP); every PT_LOAD without file
content (NOLOAD: bss, stacks) lies in [NOLOAD_BASE, NOLOAD_LIMIT); nothing is mapped anywhere else (no flash, no
cache window). BIN (the esptool image the ROM loads): same window for every segment, and the entry is inside it.
The numbers mirror platform/esp32p4/src/mem.rs and kernel/link-esp32p4.ld.
Exit 0 = clean, 1 = findings (printed), 2 = unreadable input.
"""
import struct
import sys

LOAD_BASE = 0x4FF10000
ROM_STACK_TOP = 0x4FF3AFC0
NOLOAD_BASE = 0x4FF40000
NOLOAD_LIMIT = 0x4FF80000
PT_LOAD = 1


def check_elf_bytes(d: bytes) -> list[str]:
    if d[:4] != b"\x7fELF" or d[4] != 1 or d[5] != 1:
        raise ValueError("not a little-endian ELF32")
    phoff, = struct.unpack_from("<I", d, 0x1C)
    phentsize, phnum = struct.unpack_from("<HH", d, 0x2A)
    errs = []
    for i in range(phnum):
        p_type, _off, vaddr, paddr, filesz, memsz, _flags, _align = struct.unpack_from("<8I", d, phoff + i * phentsize)
        if p_type != PT_LOAD or memsz == 0:
            continue
        if filesz:
            lo, hi, what = LOAD_BASE, ROM_STACK_TOP, "loaded"
            span = (paddr, paddr + filesz)
        else:
            lo, hi, what = NOLOAD_BASE, NOLOAD_LIMIT, "NOLOAD"
            span = (paddr, paddr + memsz)
        if vaddr != paddr:
            errs.append(f"elf segment {i}: vaddr {vaddr:#x} != paddr {paddr:#x}")
        if not (lo <= span[0] and span[1] <= hi):
            errs.append(f"elf segment {i} ({what}) {span[0]:#x}..{span[1]:#x} outside {lo:#x}..{hi:#x}")
        if filesz and memsz != filesz:
            # a loaded segment with a zero tail would make the ROM load or skip bytes the linker meant to be bss
            errs.append(f"elf segment {i}: memsz {memsz:#x} != filesz {filesz:#x}")
    return errs


def check_bin_bytes(d: bytes) -> list[str]:
    if len(d) < 24 or d[0] != 0xE9:
        raise ValueError("not an esptool image")
    nseg = d[1]
    entry, = struct.unpack_from("<I", d, 4)
    errs = []
    off = 24
    spans = []
    for i in range(nseg):
        if off + 8 > len(d):
            raise ValueError("truncated image")
        addr, size = struct.unpack_from("<II", d, off)
        spans.append((addr, addr + size))
        if not (LOAD_BASE <= addr and addr + size <= ROM_STACK_TOP):
            errs.append(f"bin segment {i} {addr:#x}..{addr + size:#x} outside {LOAD_BASE:#x}..{ROM_STACK_TOP:#x}")
        off += 8 + size
    if nseg == 0:
        errs.append("bin has no segments")
    if not any(lo <= entry < hi for lo, hi in spans):
        errs.append(f"bin entry {entry:#x} is not inside a loaded segment")
    return errs


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    try:
        errs = check_elf_bytes(open(argv[1], "rb").read()) + check_bin_bytes(open(argv[2], "rb").read())
    except (OSError, ValueError, struct.error) as e:
        print(f"check_image: {e}", file=sys.stderr)
        return 2
    for e in errs:
        print(f"check_image: {e}")
    if not errs:
        print("check_image: RAM-only, inside the limits")
    return 1 if errs else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
