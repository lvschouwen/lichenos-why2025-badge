#!/usr/bin/env python3
"""Host test (#614): tools/v3-load-ram.sh refuses images that touch flash or leave the RAM window, refuses while main
holds the bench or another run holds the lock, and passes a RAM-only image on to badge.sh load-ram (a stub here).
Fixtures are built in memory: a tiny ELF32 with program headers and an esptool image."""
import os
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
import unittest

TOOLS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
RAM = 0x4FF10000
FLASH = 0x42000000


def elf(segs):
    """segs: (vaddr, filesz, memsz); data is zero-filled."""
    phoff, phsize = 52, 32
    data_off = phoff + phsize * len(segs)
    hdr = b"\x7fELF" + bytes([1, 1, 1]) + bytes(9) + struct.pack("<HHIIIIIHHHHHH", 2, 243, 1, RAM, phoff, 0, 0, 52, 32, len(segs), 40, 0, 0)
    ph, body = b"", b""
    for va, fsz, msz in segs:
        ph += struct.pack("<IIIIIIII", 1, data_off + len(body), va, va, fsz, msz, 7, 4)
        body += bytes(fsz)
    return hdr + ph + body


def image(segs, entry=RAM):
    out = struct.pack("<BBBBI", 0xE9, len(segs), 2, 0x20, entry) + bytes(16)
    for addr, size in segs:
        out += struct.pack("<II", addr, size) + bytes(size)
    return out


class WrapperTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp)
        tools = os.path.join(self.tmp, "tools")
        os.mkdir(tools)
        subprocess.run(["git", "init", "-q", self.tmp], check=True)  # the wrapper finds its checkout through git
        for f in ("v3-load-ram.sh", "v3_load_ram_check.py"):
            shutil.copy(os.path.join(TOOLS, f), tools)
        self.record = os.path.join(self.tmp, "badge-args")
        self.write_exec(os.path.join(tools, "badge.sh"), f'#!/bin/sh\necho "$@" > "{self.record}"\ncp "$3" "{self.tmp}/seen"\n')
        self.good_bin = os.path.join(self.tmp, "good.bin")
        self.put(self.good_bin, image([(RAM, 64)]))
        # stand-in for the elf-to-image converter: always yields the good image, and says it ran
        stub = os.path.join(self.tmp, ".local", "bin")  # the wrapper's fixed PATH ends in $HOME/.local/bin
        os.makedirs(stub)
        self.write_exec(os.path.join(stub, "esptool"), f'#!/bin/sh\ncp "{self.good_bin}" "$5"\ntouch "{self.tmp}/converted"\n')
        self.env = dict(os.environ, HOME=self.tmp, BADGE_PORT="/dev/ttyACM7", XDG_CONFIG_HOME=self.tmp)

    def write_exec(self, path, text):
        with open(path, "w") as f:
            f.write(text)
        os.chmod(path, os.stat(path).st_mode | stat.S_IXUSR)

    def put(self, path, data):
        with open(path, "wb") as f:
            f.write(data)
        return path

    def run_wrapper(self, *args):
        r = subprocess.run(["bash", os.path.join(self.tmp, "tools", "v3-load-ram.sh"), *args], env=self.env,
                           capture_output=True, text=True, timeout=30)
        return r.returncode, r.stderr

    def called(self):
        return os.path.exists(self.record)

    def refused(self, path, why):
        rc, err = self.run_wrapper(path)
        self.assertEqual(rc, 1, err)
        self.assertIn(why, err)
        self.assertFalse(self.called())

    def test_flash_segment_elf_refused(self):
        self.refused(self.put(f"{self.tmp}/a.elf", elf([(RAM, 64, 64), (FLASH, 64, 64)])), "flash-mapped")

    def test_flash_segment_bin_refused(self):
        self.refused(self.put(f"{self.tmp}/a.bin", image([(FLASH, 64)])), "flash-mapped")

    def test_high_flash_alias_refused(self):
        self.refused(self.put(f"{self.tmp}/a.bin", image([(0x80000000, 64)])), "flash-mapped")

    def test_too_big_refused(self):
        self.refused(self.put(f"{self.tmp}/a.bin", image([(RAM, 0x30000)])), "outside the loaded window")

    def test_too_big_elf_refused(self):
        self.refused(self.put(f"{self.tmp}/a.elf", elf([(RAM, 0x30000, 0x30000)])), "outside the loaded window")

    def test_bss_outside_l2mem_refused(self):
        self.refused(self.put(f"{self.tmp}/a.elf", elf([(RAM, 64, 0x200000)])), "outside L2MEM")

    def test_bss_above_load_window_accepted(self):
        elf_path = self.put(f"{self.tmp}/a.elf", elf([(RAM, 64, 64), (0x4FF40000, 0, 0x1000)]))
        self.assertEqual(self.run_wrapper(elf_path)[0], 0)

    def test_entry_outside_refused(self):
        self.refused(self.put(f"{self.tmp}/a.bin", image([(RAM, 64)], entry=FLASH)), "entry")

    def test_garbage_refused(self):
        self.refused(self.put(f"{self.tmp}/a.bin", b"hello"), "not an ELF")

    def test_ram_bin_accepted(self):
        rc, err = self.run_wrapper(self.good_bin)
        self.assertEqual(rc, 0, err)
        with open(self.record) as f:
            args = f.read().split()
        self.assertEqual(args[:2], ["load-ram", "/dev/ttyACM7"])
        self.assertEqual(args[3:], ["--baud", "460800", "--timeout", "20"])

    def test_ram_elf_converted_and_accepted(self):
        rc, err = self.run_wrapper(self.put(f"{self.tmp}/a.elf", elf([(RAM, 64, 64)])), "--follow", "--timeout", "5")
        self.assertEqual(rc, 0, err)
        self.assertTrue(os.path.exists(f"{self.tmp}/converted"))
        with open(self.record) as f:
            args = f.read().split()
        self.assertEqual(args[-3:], ["--timeout", "5", "--follow"])

    def test_copy_is_what_gets_checked_and_passed_on(self):
        rc, err = self.run_wrapper(self.good_bin)
        self.assertEqual(rc, 0, err)
        with open(self.record) as f:
            sent = f.read().split()[2]
        self.assertNotEqual(sent, self.good_bin)  # a private copy, not the caller's file
        self.assertIn("v3-load-ram.", sent)
        with open(f"{self.tmp}/seen", "rb") as f, open(self.good_bin, "rb") as g:
            self.assertEqual(f.read(), g.read())
        self.assertFalse(os.path.exists(sent))  # and it is gone afterwards

    def test_bad_ports_refused(self):
        for port in ["x; touch /tmp/pwned", "/dev/ttyACM0 --x", "/dev/ttyACM", "/dev/../etc/x", "$(id)", "ttyACM0",
                     "/dev/serial/by-id/a b", "/dev/serial/by-id/a;b", "/dev/ttyACM0\n"]:
            self.env["BADGE_PORT"] = port
            rc, err = self.run_wrapper(self.good_bin)
            self.assertEqual(rc, 1, port)
            self.assertIn("is not /dev/", err)
            self.assertFalse(self.called())

    def test_good_ports_accepted(self):
        for port in ["/dev/ttyACM0", "/dev/ttyUSB12", "/dev/serial/by-id/usb-Espressif_USB_JTAG:serial_debug_unit_AB-if00"]:
            self.env["BADGE_PORT"] = port
            self.assertEqual(self.run_wrapper(self.good_bin)[0], 0, port)

    def test_bench_lock_refuses(self):
        os.makedirs(f"{self.tmp}/badge-logs", exist_ok=True)
        open(f"{self.tmp}/badge-logs/bench.lock", "w").close()
        rc, err = self.run_wrapper(self.good_bin)
        self.assertEqual(rc, 1)
        self.assertIn("main holds the bench", err)
        self.assertFalse(self.called())

    def test_held_run_lock_refuses_and_is_not_stale(self):
        os.makedirs(f"{self.tmp}/badge-logs", exist_ok=True)
        holder = subprocess.Popen(["flock", f"{self.tmp}/badge-logs/v3-load-ram.lock", "sleep", "30"],
                                  start_new_session=True)
        self.addCleanup(holder.wait)
        subprocess.run(["sleep", "0.3"])
        rc, err = self.run_wrapper(self.good_bin)
        self.assertEqual(rc, 1)
        self.assertIn("another v3-load-ram run", err)
        os.killpg(holder.pid, 9)  # the sleep child holds the lock too
        holder.wait()
        self.assertEqual(self.run_wrapper(self.good_bin)[0], 0)  # a leftover lock file never blocks

    def test_no_port_refused(self):
        del self.env["BADGE_PORT"]
        rc, err = self.run_wrapper(self.good_bin)
        self.assertEqual(rc, 1)
        self.assertIn("no port", err)

    def test_bad_timeout(self):
        self.assertEqual(self.run_wrapper(self.good_bin, "--timeout", "0")[0], 2)
        self.assertEqual(self.run_wrapper(self.good_bin, "--timeout", "x")[0], 2)
        self.assertEqual(self.run_wrapper(self.good_bin, "--timeout", "901")[0], 2)


if __name__ == "__main__":
    sys.exit(unittest.main(verbosity=1))
