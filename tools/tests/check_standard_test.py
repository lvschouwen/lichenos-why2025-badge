#!/usr/bin/env python3
"""Host test of the standard gate's own logic (#202): parsing, ratchet, size deltas. Run with python3 -I."""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import check_standard as cs  # noqa: E402

LIZARD = """\
NLOC    CCN   token  PARAM  length  NS  location
------------------------------------------------
      9      1     42      0       9     0 small@10-18@firmware/main/a.c
    120     30    900      9     160     7 big@20-180@firmware/main/a.c
==============================================================
NLOC    Avg.NLOC  AvgCCN  Avg.token  function_cnt    file
--------------------------------------------------------------
   1600      14.7     3.4       82.9        26     firmware/main/a.c
"""

SU = (
    "../components/x/x.c:10:5:frame\t1100\tstatic\n"
    "../components/x/x.c:30:5:small\t64\tstatic\n"
    "../components/x/x.cpp:7:1:void ns::run(int)\t2000\tstatic\n"
    "../managed_components/y/y.c:1:1:skip\t4000\tstatic\n"
)


class Parsing(unittest.TestCase):
    def test_lizard(self):
        m = cs.parse_lizard(LIZARD)
        self.assertEqual(m[("ccn", "firmware/main/a.c:big")], 30)
        self.assertEqual(m[("len", "firmware/main/a.c:big")], 160)
        self.assertEqual(m[("nest", "firmware/main/a.c:big")], 7)
        self.assertEqual(m[("params", "firmware/main/a.c:big")], 9)
        self.assertEqual(m[("nloc", "firmware/main/a.c")], 1600)
        self.assertNotIn(("nloc", "86"), m)

    def test_stack_usage_skips_vendored_and_keeps_cpp_names(self):
        m = cs.parse_stack_usage(SU, "/r", "/r/firmware/build")
        self.assertEqual(m[("stack", "firmware/components/x/x.c:frame")], 1100)
        self.assertEqual(m[("stack", "firmware/components/x/x.cpp:void ns::run(int)")], 2000)
        self.assertEqual(len(m), 3)

    def test_baseline_roundtrip_and_errors(self):
        text = "# c\nccn\ta.c:f\t30\n\nsize\tliba.a\t100\n"
        b = cs.parse_baseline(text)
        self.assertEqual(b, {("ccn", "a.c:f"): 30, ("size", "liba.a"): 100})
        self.assertEqual(cs.parse_baseline(cs.format_baseline("# c", b)), b)
        with self.assertRaises(ValueError):
            cs.parse_baseline("ccn a.c:f 30\n")

    def test_archive_sizes_and_partition(self):
        m = cs.parse_archive_sizes('"Archive File","Total Size","DIRAM"\n"libmesh.a","373634","1"\n')
        self.assertEqual(m, {("size", "libmesh.a"): 373634})
        self.assertEqual(cs.partition_size("ota_0, app, ota_0, 0x20000, 0x400000\n", "ota_0"), 0x400000)


class Ratchet(unittest.TestCase):
    key = ("ccn", "a.c:f")

    def test_new_offender_fails(self):
        f, w, s = cs.compare_static({self.key: 26}, {})
        self.assertEqual(len(f), 1)
        self.assertTrue(f[0].startswith("NEW"))

    def test_baselined_offender_passes_but_worse_fails(self):
        self.assertEqual(cs.compare_static({self.key: 30}, {self.key: 30})[0], [])
        f, _, _ = cs.compare_static({self.key: 31}, {self.key: 30})
        self.assertTrue(f and f[0].startswith("WORSE"))

    def test_at_the_fail_line_is_not_a_violation(self):
        self.assertEqual(cs.compare_static({self.key: 25}, {})[0], [])

    def test_warn_only_in_changed_files(self):
        m = {self.key: 20}
        self.assertEqual(len(cs.compare_static(m, {}, {"a.c"})[1]), 1)
        self.assertEqual(cs.compare_static(m, {}, {"b.c"})[1], [])
        self.assertEqual(len(cs.compare_static(m, {})[1]), 1)

    def test_stale_entries_reported_and_shrink_never_adds(self):
        base = {self.key: 30, ("len", "a.c:g"): 200}
        meas = {self.key: 27, ("len", "a.c:g"): 100, ("ccn", "a.c:new"): 40}
        _, _, stale = cs.compare_static(meas, base)
        self.assertEqual(len(stale), 2)
        out = cs.shrink(meas, base)
        self.assertEqual(out, {self.key: 27})

    def test_shrink_keeps_size_entries_out_of_the_ratchet(self):
        self.assertEqual(cs.shrink({}, {("size", "liba.a"): 5}), {("size", "liba.a"): 5})

    def test_initial_baseline_holds_only_offenders_and_sizes(self):
        meas = {self.key: 26, ("ccn", "a.c:ok"): 3}
        self.assertEqual(cs.initial_baseline(meas, {("size", "l.a"): 9}), {self.key: 26, ("size", "l.a"): 9})


class Sizes(unittest.TestCase):
    def test_image_lines(self):
        slot = 1000
        self.assertEqual(cs.compare_image(800, slot)[:2], ([], []))
        self.assertEqual(len(cs.compare_image(860, slot)[1]), 1)
        self.assertEqual(len(cs.compare_image(930, slot)[0]), 1)
        self.assertEqual(cs.compare_image(920, slot)[0], [])  # exactly 92 % is not past the line

    def test_growth_per_archive(self):
        base = {("size", "a.a"): 1000, ("size", "b.a"): 1000, ("size", "c.a"): 100000}
        meas = {("size", "a.a"): 1000 + 3000, ("size", "b.a"): 1000 + 9000, ("size", "c.a"): 100000 - 9000,
                ("size", "new.a"): 9000}
        f, w, s = cs.compare_sizes(meas, base)
        self.assertEqual(len(f), 2)  # b.a and the new archive counted from zero
        self.assertEqual(len(w), 1)
        self.assertEqual(len(s), 1)


if __name__ == "__main__":
    unittest.main()
