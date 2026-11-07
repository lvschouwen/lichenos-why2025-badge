#!/usr/bin/env python3
"""Host test of the standard gate's own logic (#202): parsing, ratchet, size deltas. Run with python3 -I."""
import os
import subprocess
import sys
import tempfile
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
        with self.assertRaises(cs.GateError):
            cs.parse_baseline("ccn a.c:f 30\n")
        with self.assertRaises(cs.GateError):
            cs.parse_baseline("ccn\ta.c:f\t30\nccn\ta.c:f\t20\n")  # listed twice
        with self.assertRaises(cs.GateError):
            cs.parse_baseline("bogus\ta.c:f\t30\n")  # unknown metric

    def test_archive_sizes_and_partition(self):
        m = cs.parse_archive_sizes('"Archive File","Total Size","DIRAM"\n"libmesh.a","373634","1"\n')
        self.assertEqual(m, {("size", "libmesh.a"): 373634})
        self.assertEqual(cs.partition_size("ota_0, app, ota_0, 0x20000, 0x400000\n", "ota_0"), 0x400000)
        self.assertEqual(cs.partition_size("ota_0,app,ota_0,0x10000,3M,\n", "ota_0"), 3 * 1024 * 1024)


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
        self.assertEqual(len(stale), 2)  # H7: the caller turns these into failures
        out = cs.shrink(meas, base)
        self.assertEqual(out, {self.key: 27})
        with self.assertRaises(cs.GateError):  # M4: a partial measurement must not drop real offenders
            cs.shrink({}, base, ["lizard failed"])

    def test_shrink_keeps_size_entries_out_of_the_ratchet(self):
        self.assertEqual(cs.shrink({}, {("size", "liba.a"): 5}), {("size", "liba.a"): 5})

    def test_initial_baseline_holds_only_offenders_and_sizes(self):
        meas = {self.key: 26, ("ccn", "a.c:ok"): 3}
        self.assertEqual(cs.initial_baseline(meas, {("size", "l.a"): 9}), {self.key: 26, ("size", "l.a"): 9})


class Sizes(unittest.TestCase):
    def test_image_lines(self):
        slot = 1000
        self.assertEqual(cs.compare_image('x', 800, slot)[:2], ([], []))
        self.assertEqual(len(cs.compare_image('x', 860, slot)[1]), 1)
        self.assertEqual(len(cs.compare_image('x', 930, slot)[0]), 1)
        self.assertEqual(cs.compare_image('x', 920, slot)[0], [])  # exactly 92 % is not past the line

    def test_growth_per_archive(self):
        base = {("size", "a.a"): 1000, ("size", "b.a"): 1000, ("size", "c.a"): 100000}
        meas = {("size", "a.a"): 1000 + 3000, ("size", "b.a"): 1000 + 9000, ("size", "c.a"): 100000 - 9000,
                ("size", "new.a"): 9000}
        f, w, s = cs.compare_sizes(meas, base)
        self.assertEqual(len(f), 2)  # b.a and the new archive counted from zero
        self.assertEqual(len(w), 1)
        self.assertEqual(len(s), 1)


SH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "check_standard.sh")
FAKE_LIZARD = "import sys\nsys.stdout.write(%r)\nsys.stderr.write('boom')\nsys.exit(%d)\n"


class Tooling(unittest.TestCase):
    def test_h1_missing_lizard_fails_unless_explicitly_skipped(self):
        with tempfile.TemporaryDirectory() as empty:
            env = {"PATH": empty}
            r = subprocess.run(["/bin/bash", SH], env=env, capture_output=True, text=True)
            self.assertEqual(r.returncode, 1)
            self.assertIn("lizard is not installed", r.stderr)
            env["BADGE_SKIP_STANDARD"] = "1"
            r = subprocess.run(["/bin/bash", SH], env=env, capture_output=True, text=True)
            self.assertEqual(r.returncode, 0)
            self.assertIn("SKIPPED", r.stderr)

    def _fake(self, tmp, out, rc):
        path = os.path.join(tmp, "fakelizard.py")
        with open(path, "w") as f:
            f.write(FAKE_LIZARD % (out, rc))
        return (sys.executable, path)

    def test_h2_lizard_failure_and_missing_rows_are_problems(self):
        with tempfile.TemporaryDirectory() as tmp:
            _, problems = cs.measure_lizard(tmp, ["firmware/main/a.c"], self._fake(tmp, "", 2))
            self.assertTrue(problems and "exit 2" in problems[0])
            _, problems = cs.measure_lizard(tmp, ["firmware/main/a.c", "firmware/main/b.c"], self._fake(tmp, LIZARD, 0))
            self.assertEqual(problems, ["lizard returned no result for firmware/main/b.c"])
            _, problems = cs.measure_lizard(tmp, ["firmware/main/a.c"], ("/nonexistent/lizard",))
            self.assertTrue(problems and "cannot run lizard" in problems[0])

    def test_h3_no_su_files_is_a_problem(self):
        with tempfile.TemporaryDirectory() as tmp:
            m, problems = cs.measure_stack(tmp, [tmp])
            self.assertEqual(m, {})
            self.assertEqual(len(problems), 1)

    def test_h4_missing_image_is_a_problem(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertIn("not built", cs.gather_size(tmp, tmp)[3][0])
            self.assertIn("not built", cs.gather_c6_image(tmp)[2][0])

    def test_h5_untracked_files_are_measured(self):
        with tempfile.TemporaryDirectory() as tmp:
            for args in (["init", "-q"], ["config", "user.email", "t@t"], ["config", "user.name", "t"]):
                subprocess.run(["git", "-C", tmp] + args, check=True)
            os.makedirs(os.path.join(tmp, "firmware", "components", "x"))
            os.makedirs(os.path.join(tmp, "firmware", "components", "thirdparty"))
            for rel in ("firmware/components/x/tracked.c", "firmware/components/x/new.c", "firmware/components/thirdparty/v.c"):
                with open(os.path.join(tmp, rel), "w") as f:
                    f.write("int f(void){return 0;}\n")
            subprocess.run(["git", "-C", tmp, "add", "firmware/components/x/tracked.c"], check=True)
            self.assertEqual(cs.tracked_sources(tmp), ["firmware/components/x/new.c", "firmware/components/x/tracked.c"])


class Shrinking(unittest.TestCase):
    key = ("ccn", "a.c:f")

    def test_h6_new_or_raised_entries_fail_against_the_merge_base(self):
        base = {self.key: 30}
        self.assertEqual(cs.check_shrink_only({self.key: 30}, base), [])
        self.assertEqual(cs.check_shrink_only({self.key: 28}, base), [])
        self.assertEqual(len(cs.check_shrink_only({self.key: 31}, base)), 1)
        self.assertEqual(len(cs.check_shrink_only({self.key: 30, ("ccn", "a.c:g"): 26}, base)), 1)
        self.assertEqual(cs.check_shrink_only({("size", "l.a"): 99}, base), [])  # size entries are not ratcheted

    def test_m3_rename_keeps_the_value_and_passes_the_check(self):
        base = {self.key: 30, ("nloc", "a.c"): 1600}
        moved, moves = cs.apply_rename(base, "a.c", "b.c")
        self.assertEqual(len(moves), 2)
        self.assertEqual(moved[("ccn", "b.c:f")], 30)
        self.assertNotIn(self.key, moved)
        self.assertEqual(cs.check_shrink_only(moved, base), [])
        # a rename record does not launder a raised value
        moved[("ccn", "b.c:f")] = 31
        self.assertEqual(len(cs.check_shrink_only(moved, base)), 1)
        # without the record the same entry is new
        plain = {k: v for k, v in moved.items() if k[0] != cs.MOVED}
        plain[("ccn", "b.c:f")] = 30
        self.assertEqual(len(cs.check_shrink_only(plain, base)), 2)  # the ccn and the nloc entry
        with self.assertRaises(cs.GateError):
            cs.apply_rename(base, "nothere.c", "c.c")

    def test_settled_move_records_are_dropped(self):
        base = {self.key: 30}
        moved, _ = cs.apply_rename(base, "a.c", "b.c")
        merged_base = {("ccn", "b.c:f"): 30}
        self.assertFalse(any(k[0] == cs.MOVED for k in cs.drop_settled_moves(moved, merged_base)))
        self.assertTrue(any(k[0] == cs.MOVED for k in cs.drop_settled_moves(moved, base)))

    def test_h7_h6_run_check_fails_on_stale_and_incomplete(self):
        meas = {self.key: 20}
        fails, _, _ = cs.run_check("", {self.key: 30}, {self.key: 30}, meas, {}, {"p4": (0, 0), "c6": (0, 0)}, [], set())
        self.assertTrue(any(f.startswith("STALE") for f in fails))
        fails, _, _ = cs.run_check("", {}, {}, {}, {}, {"p4": (0, 0), "c6": (0, 0)}, ["no .su files"], set())
        self.assertEqual(fails, ["INCOMPLETE no .su files"])
        fails, _, notes = cs.run_check("", {}, None, {}, {}, {"p4": (0, 0), "c6": (0, 0)}, [], set())
        self.assertEqual(fails, [])
        self.assertTrue(any("introduces" in n for n in notes))

    def test_m1_stack_entries_merge_with_max(self):
        a = cs.parse_stack_usage("../../shared/lora/p.c:1:1:f\t100\tstatic\n", "/r", "/r/firmware/build")
        b = cs.parse_stack_usage("../../shared/lora/p.c:1:1:f\t900\tstatic\n", "/r", "/r/firmware/build")
        dst = {}
        cs.merge_max(dst, b)
        cs.merge_max(dst, a)
        self.assertEqual(dst[("stack", "shared/lora/p.c:f")], 900)

    def test_m2_duplicate_function_names_get_distinct_keys(self):
        text = (
            "      9      1     42      0       9     0 helper@10-18@firmware/main/a.c\n"
            "    120     30    900      9     160     7 helper@30-190@firmware/main/a.c\n"
        )
        m = cs.parse_lizard(text + text.splitlines()[1] + "\n")  # the warnings table lists a function again
        self.assertEqual(m[("ccn", "firmware/main/a.c:helper")], 1)
        self.assertNotIn(("ccn", "firmware/main/a.c:helper#3"), m)
        self.assertEqual(m[("ccn", "firmware/main/a.c:helper#2")], 30)

    def test_size_update_diff(self):
        lines = cs.diff_sizes({("size", "a.a"): 10, ("size", "b.a"): 5}, {("size", "a.a"): 15, ("size", "c.a"): 7})
        self.assertEqual(lines, ["a.a: 10 -> 15 (+5)", "b.a: 5 -> gone", "c.a: new -> 7"])

    def test_atomic_write(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = os.path.join(tmp, "b.txt")
            cs.atomic_write(p, "one\n")
            cs.atomic_write(p, "two\n")
            with open(p) as f:
                self.assertEqual(f.read(), "two\n")
            self.assertEqual(os.listdir(tmp), ["b.txt"])


if __name__ == "__main__":
    unittest.main()
