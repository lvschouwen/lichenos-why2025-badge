import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import perf_report as pr  # noqa: E402

REAL = Path(__file__).resolve().parent.parent / "perf"
TARGETS = """
["syscall.local"]
key = "p95_us"
direction = "lower"
v2 = "todo"
target = 10
stretch = 2
ratchet = true
tolerance_pct = 10

["mem.dma_free"]
key = "bytes"
direction = "higher"
v2 = 100
target = 125
ratchet = true

["boot.first_frame"]
key = "ms"
direction = "lower"
v2 = 1000
target_pct = -50
ratchet = false
"""


class Parse(unittest.TestCase):
    def test_lines(self):
        log = "noise\n[12] PERF syscall.local median_us=3.1 p95_us=4.8\nPERF a.b x=1 y=-2\nPERF bad x=1ms\nPERF a.b x=7\n"
        got, warn = pr.parse_log(log)
        self.assertEqual(got["syscall.local"], {"median_us": 3.1, "p95_us": 4.8})
        self.assertEqual(got["a.b"], {"x": 7.0})  # the later line wins
        self.assertNotIn("bad", got)
        self.assertEqual(len(warn), 1)

    def test_not_perf(self):
        got, warn = pr.parse_log("PERFORMANCE fine\nsomething PERF x a=1\n")
        self.assertEqual(got, {})
        self.assertEqual(len(warn), 1)


class Evaluate(unittest.TestCase):
    def setUp(self):
        import tomllib
        self.t = tomllib.loads(TARGETS)

    def rows(self, measured, best=None):
        rows, reg = pr.evaluate(self.t, best or {}, measured, "b1")
        return {r["metric"]: r for r in rows}, reg

    def test_unmeasured_skipped(self):
        rows, reg = self.rows({}, {"syscall.local": {"value": 1.0}})
        self.assertEqual(reg, [])
        self.assertEqual(rows["syscall.local"]["status"], "-")

    def test_status_and_delta(self):
        rows, _ = self.rows({"boot.first_frame": {"ms": 400}, "syscall.local": {"p95_us": 1.5}})
        self.assertEqual(rows["boot.first_frame"]["status"], "ok")  # target 500 from v2 1000
        self.assertAlmostEqual(rows["boot.first_frame"]["delta_pct"], -60.0)
        self.assertEqual(rows["syscall.local"]["status"], "stretch")
        self.assertIsNone(rows["syscall.local"]["delta_pct"])  # v2 todo
        rows, _ = self.rows({"mem.dma_free": {"bytes": 110}})
        self.assertEqual(rows["mem.dma_free"]["status"], "miss")

    def test_ratchet_lower(self):
        best = {"syscall.local": {"value": 4.0}}
        _, reg = self.rows({"syscall.local": {"p95_us": 4.4}}, best)  # exactly +10 %: inside tolerance
        self.assertEqual(reg, [])
        rows, reg = self.rows({"syscall.local": {"p95_us": 4.5}}, best)
        self.assertEqual(len(reg), 1)
        self.assertEqual(rows["syscall.local"]["status"], "REGRESS")

    def test_ratchet_higher_default_tolerance(self):
        best = {"mem.dma_free": {"value": 200}}
        _, reg = self.rows({"mem.dma_free": {"bytes": 190}}, best)  # 5 %
        self.assertEqual(reg, [])
        _, reg = self.rows({"mem.dma_free": {"bytes": 189}}, best)
        self.assertEqual(len(reg), 1)

    def test_not_ratcheted_never_fails(self):
        _, reg = self.rows({"boot.first_frame": {"ms": 9999}}, {"boot.first_frame": {"value": 1}})
        self.assertEqual(reg, [])

    def test_best_only_improves(self):
        best = {"syscall.local": {"value": 4.0, "build": "a"}}
        same = pr.improve_best(self.t, best, {"syscall.local": {"p95_us": 5.0}}, "b")
        self.assertEqual(same["syscall.local"]["value"], 4.0)
        better = pr.improve_best(self.t, best, {"syscall.local": {"p95_us": 3.0}, "boot.first_frame": {"ms": 1}}, "b")
        self.assertEqual(better["syscall.local"], {"value": 3.0, "build": "b"})
        self.assertNotIn("boot.first_frame", better)  # not ratcheted


class Cli(unittest.TestCase):
    def run_cli(self, d, log, *extra):
        (d / "log.txt").write_text(log)
        return pr.main(["--log", str(d / "log.txt"), "--build", "t1", "--perf-dir", str(d / "perf"), *extra])

    def test_end_to_end(self):
        with tempfile.TemporaryDirectory() as td:
            d = Path(td)
            (d / "perf").mkdir()
            (d / "perf" / "targets.toml").write_text(TARGETS)
            (d / "perf" / "best.toml").write_text("")
            self.assertEqual(self.run_cli(d, "PERF syscall.local p95_us=4.0\n", "--update-best"), 0)
            res = json.loads((d / "perf" / "results" / "t1.json").read_text())
            self.assertEqual(res["metrics"]["syscall.local"]["p95_us"], 4.0)
            import tomllib
            best = tomllib.loads((d / "perf" / "best.toml").read_text())["best"]
            self.assertEqual(best["syscall.local"]["value"], 4.0)
            self.assertEqual(self.run_cli(d, "PERF syscall.local p95_us=6.0\n", "--update-best"), 1)
            best = tomllib.loads((d / "perf" / "best.toml").read_text())["best"]
            self.assertEqual(best["syscall.local"]["value"], 4.0)  # a regression never moves best
            self.assertEqual(self.run_cli(d, "nothing measured\n"), 0)

    def test_bad_build_id(self):
        with tempfile.TemporaryDirectory() as td:
            self.assertEqual(pr.main(["--log", "x", "--build", "../x", "--perf-dir", td]), 2)


class RealFiles(unittest.TestCase):
    def test_targets_well_formed(self):
        t = pr.load_toml(REAL / "targets.toml")
        pr.load_toml(REAL / "best.toml")
        for name, m in t.items():
            self.assertIn(m["direction"], ("lower", "higher"), name)
            self.assertTrue(m["key"], name)
            self.assertIn("v2", m, name)
            self.assertTrue("target" in m or "target_pct" in m, name)
            if isinstance(m["v2"], str):
                self.assertEqual(m["v2"], "todo", name)

    def test_dma_goals(self):
        t = pr.load_toml(REAL / "targets.toml")
        self.assertEqual(t["mem.dma_free"]["target"], 171474)
        self.assertEqual(t["mem.dma_min"]["target"], 160384)


if __name__ == "__main__":
    unittest.main()
