"""Host tests for check_image.py: synthetic ELF and esptool images."""
import struct
import unittest

import check_image as ci


def elf(segs):
    """segs: (paddr, filesz, memsz). Minimal ELF32 LE with program headers right after the 52-byte header."""
    d = bytearray(52 + 32 * len(segs))
    d[:4] = b"\x7fELF"
    d[4], d[5] = 1, 1
    struct.pack_into("<I", d, 0x1C, 52)
    struct.pack_into("<HH", d, 0x2A, 32, len(segs))
    for i, (pa, fs, ms) in enumerate(segs):
        struct.pack_into("<8I", d, 52 + 32 * i, 1, 0, pa, pa, fs, ms, 6, 4)
    return bytes(d)


def binimg(entry, segs):
    d = bytearray([0xE9, len(segs), 0, 0]) + struct.pack("<I", entry) + bytes(16)
    for addr, size in segs:
        d += struct.pack("<II", addr, size) + bytes(size)
    return bytes(d)


class ElfTests(unittest.TestCase):
    def test_clean(self):
        self.assertEqual(ci.check_elf_bytes(elf([(0x4FF10000, 0x1000, 0x1000), (0x4FF40000, 0, 0x4800)])), [])

    def test_flash_segment_is_refused(self):
        errs = ci.check_elf_bytes(elf([(0x42000000, 0x100, 0x100)]))
        self.assertEqual(len(errs), 1)
        self.assertIn("outside", errs[0])

    def test_loaded_image_into_rom_stack(self):
        self.assertTrue(ci.check_elf_bytes(elf([(0x4FF10000, 0x2B000, 0x2B000)])))

    def test_loaded_image_past_the_old_stage_bound(self):
        # #658: the load window ends at 0x4FF3ABD0, so an image past the old 0x4FF28ED0 bound passes
        self.assertEqual(ci.check_elf_bytes(elf([(0x4FF10000, 0x19000, 0x19000)])), [])

    def test_noload_into_cache_carve(self):
        self.assertTrue(ci.check_elf_bytes(elf([(0x4FF40000, 0, 0x50000)])))

    def test_noload_below_the_load_limit(self):
        self.assertTrue(ci.check_elf_bytes(elf([(0x4FF30000, 0, 0x1000)])))

    def test_not_an_elf(self):
        with self.assertRaises(ValueError):
            ci.check_elf_bytes(b"nope" * 20)


def elf_with_symbols(syms: dict) -> bytes:
    """ELF32 with a null section, .symtab (link 2) and .strtab; section headers at the end."""
    strtab = b"\0"
    sym = bytes(16)
    for name, value in syms.items():
        sym += struct.pack("<IIIBBH", len(strtab), value, 0, 0, 0, 1)
        strtab += name + b"\0"
    d = bytearray(52) + sym + strtab
    symoff, stroff, shoff = 52, 52 + len(sym), len(d)
    d[:4] = b"\x7fELF"
    d[4], d[5] = 1, 1
    struct.pack_into("<I", d, 0x20, shoff)
    struct.pack_into("<HHH", d, 0x2E, 40, 3, 2)
    d += bytes(40)
    d += struct.pack("<10I", 0, 2, 0, 0, symoff, len(sym), 2, 0, 4, 16)
    d += struct.pack("<10I", 0, 3, 0, 0, stroff, len(strtab), 0, 0, 1, 0)
    return bytes(d)


class RamTextTests(unittest.TestCase):
    def test_in_l2mem(self):
        self.assertEqual(ci.check_ram_text_bytes(elf_with_symbols({b"_ram_text_start": 0x4FF10200, b"_ram_text_end": 0x4FF10280})), [])

    def test_outside_l2mem(self):
        self.assertTrue(ci.check_ram_text_bytes(elf_with_symbols({b"_ram_text_start": 0x42000000, b"_ram_text_end": 0x42000080})))

    def test_empty(self):
        self.assertTrue(ci.check_ram_text_bytes(elf_with_symbols({b"_ram_text_start": 0x4FF10200, b"_ram_text_end": 0x4FF10200})))

    def test_missing(self):
        self.assertTrue(ci.check_ram_text_bytes(elf_with_symbols({b"other": 1})))


class BinTests(unittest.TestCase):
    def test_clean(self):
        self.assertEqual(ci.check_bin_bytes(binimg(0x4FF10000, [(0x4FF10000, 0x190C)])), [])

    def test_flash_mapped_segment(self):
        errs = ci.check_bin_bytes(binimg(0x4FF10000, [(0x4FF10000, 16), (0x40000000, 16)]))
        self.assertEqual(len(errs), 1)

    def test_entry_outside(self):
        self.assertTrue(ci.check_bin_bytes(binimg(0x4FF20000, [(0x4FF10000, 16)])))

    def test_truncated(self):
        with self.assertRaises(ValueError):
            ci.check_bin_bytes(binimg(0x4FF10000, [(0x4FF10000, 16)])[:26])


if __name__ == "__main__":
    unittest.main()
