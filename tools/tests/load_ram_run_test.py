#!/usr/bin/env python3
"""Host test (#614): load_ram_run.py mirrors the v3 UART output into the serial logger's file as whole lines with the
logger's 'HH:MM:SS ' prefix, between start and end markers, while the logger has the port closed."""
import io
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import load_ram_run  # noqa: E402  (imports without esptool: that import is inside main)


class Keep(io.StringIO):
    def close(self):
        self.kept = self.getvalue()
        super().close()


class MirrorTest(unittest.TestCase):
    def mirror(self):
        f = Keep()
        return f, load_ram_run.LogMirror(f, clock=lambda: "12:34:56 ")

    def test_whole_lines_prefixed(self):
        f, m = self.mirror()
        m.marker("start (load-ram.bin, 20 s)")
        m.feed(b"BOOT v3\r\nti")
        self.assertNotIn("ti", f.getvalue().splitlines()[-1])  # a partial line waits
        m.feed(b"ck 1\r\n")
        m.feed(b"END ok\n")
        m.close("end (rc 0), hard reset")
        self.assertEqual(f.kept.splitlines(), [
            "12:34:56 --- v3 load-ram: start (load-ram.bin, 20 s)",
            "12:34:56 BOOT v3",
            "12:34:56 tick 1",
            "12:34:56 END ok",
            "12:34:56 --- v3 load-ram: end (rc 0), hard reset",
        ])

    def test_partial_last_line_and_bad_bytes(self):
        f, m = self.mirror()
        m.feed(b"\xffhalf")
        m.close("end (rc 3), hard reset")
        self.assertEqual(f.kept.splitlines(), ["12:34:56 \ufffdhalf", "12:34:56 --- v3 load-ram: end (rc 3), hard reset"])

    def test_real_prefix_shape(self):
        self.assertRegex(load_ram_run.stamp(), r"^\d\d:\d\d:\d\d $")

    def test_open_mirror_appends_and_skips_missing_dir(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertIsNone(load_ram_run.open_mirror(os.path.join(d, "nope", "log")))
            path = os.path.join(d, "log")
            with open(path, "w") as f:
                f.write("old\n")
            m = load_ram_run.open_mirror(path)
            m.feed(b"x\n")
            m.close("end")
            with open(path) as f:
                lines = f.read().splitlines()
            self.assertEqual(lines[0], "old")
            self.assertTrue(lines[1].endswith(" x"))


if __name__ == "__main__":
    sys.exit(unittest.main(verbosity=1))
