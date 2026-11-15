#!/usr/bin/env python3
"""Host test of the report split by segment (#476): a transcript with the use-soak driver's markers and a sampler CSV
give the right rows per segment, per block, and the unchanged report without the option. Run with python3 -I."""
import contextlib
import io
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
import soak_parse as sp  # noqa: E402
import soak_report as sr  # noqa: E402
import soak_segment as seg  # noqa: E402

E0 = 1_800_000_000
# samples every 600 s for 3 h; awake for the first 2 h (sessions every 20 min, blocks chat, contacts, tour), night after
TRANSCRIPT = "".join(
    ["## segment awake %d\n" % E0] +
    ["## session %d %s start %d\nnoise\n## session %d %s end %d\n" % (
        k + 1, ("chat", "contacts", "tour")[k % 3], E0 + k * 1200, k + 1, ("chat", "contacts", "tour")[k % 3], E0 + k * 1200 + 60)
     for k in range(6)] +
    ["## segment night %d\n" % (E0 + 7200)] +
    ["## sample %d 2026-11-16T00:00:00Z epoch=%d\nsoak1 v=1\n" % (i + 1, E0 + 600 * i) for i in range(19)]
)
ROWS = [{"t": 600 * i, "dma": 1000 + i} for i in range(19)]
BUDGETS = "dma_free key dma >=0 - B test\n"


def put(path, text):
    with open(path, "w") as f:
        f.write(text)


def files(test):
    d = tempfile.mkdtemp()
    test.addCleanup(lambda: __import__("shutil").rmtree(d, ignore_errors=True))
    tr, cs, bd = (os.path.join(d, n) for n in ("t.txt", "s.csv", "b.txt"))
    put(tr, TRANSCRIPT)
    put(bd, BUDGETS)
    sp.write_csv(cs, ROWS)
    return tr, cs, bd


def report(args):
    out = io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
        rc = sr.main(["soak_report.py"] + args)
    return rc, out.getvalue()


class SegmentTest(unittest.TestCase):
    def markers(self):
        return seg.parse_markers(TRANSCRIPT.splitlines())

    def test_awake_and_night_rows(self):
        m = self.markers()
        awake = seg.select(ROWS, m, "awake")
        night = seg.select(ROWS, m, "night")
        self.assertEqual([r["t"] for r in awake], [600 * i for i in range(12)])
        self.assertEqual([r["t"] for r in night], [600 * i for i in range(12, 19)])
        self.assertEqual(len(awake) + len(night), len(ROWS))

    def test_block_rows_run_to_the_next_session(self):
        m = self.markers()
        # chat sessions start at 0 and 3600 s: samples 0..5 and 6..11 minus the contacts/tour spans
        self.assertEqual([r["t"] for r in seg.select(ROWS, m, "chat")], [0, 600, 3600, 4200])
        self.assertEqual([r["t"] for r in seg.select(ROWS, m, "contacts")], [1200, 1800, 4800, 5400])
        # the last session (tour, started at 6000 s) holds every later sample, the night too
        self.assertEqual([r["t"] for r in seg.select(ROWS, m, "tour")][-1], 600 * 18)

    def test_epoch_column_wins_over_t(self):
        m = self.markers()
        rows = [{"epoch": E0 + 7300, "t": 0}]
        self.assertEqual(seg.select(rows, m, "night"), rows)

    def test_unknown_segment_or_missing_time_is_an_error(self):
        m = self.markers()
        with self.assertRaises(ValueError):
            seg.select(ROWS, m, "files")
        with self.assertRaises(ValueError):
            seg.select([{"dma": 1}], m, "awake")
        with self.assertRaises(ValueError):
            seg.select(ROWS, seg.parse_markers([]), "night")


class ReportTest(unittest.TestCase):
    def test_expected_split(self):
        tr, cs, bd = files(self)
        rc, out = report(["expected", cs, "--budgets", bd, "--segment", "night", "--transcript", tr])
        self.assertEqual(rc, 0)
        self.assertIn("samples: 7,", out)
        rc, out = report(["expected", cs, "--budgets", bd, "--segment", "awake", "--transcript", tr])
        self.assertIn("samples: 12,", out)
        rc, out = report(["expected", cs, "--budgets", bd, "--segment", "contacts", "--transcript", tr])
        self.assertIn("samples: 4,", out)

    def test_without_the_option_nothing_changes(self):
        tr, cs, bd = files(self)
        rc, out = report(["expected", cs, "--budgets", bd])
        self.assertIn("samples: 19,", out)

    def test_options_go_together_and_unknown_is_a_tool_error(self):
        tr, cs, bd = files(self)
        self.assertEqual(report(["expected", cs, "--budgets", bd, "--segment", "night"])[0], 2)
        self.assertEqual(report(["expected", cs, "--budgets", bd, "--transcript", tr])[0], 2)
        self.assertEqual(report(["expected", cs, "--budgets", bd, "--segment", "files", "--transcript", tr])[0], 2)

    def test_trend_split(self):
        tr, cs, bd = files(self)
        allow = os.path.join(os.path.dirname(cs), "allow.txt")
        put(allow, "")
        rc, out = report(["trend", cs, "--allow", allow, "--segment", "night", "--transcript", tr])
        self.assertEqual(rc, 0)
        self.assertIn("7 samples", out)
        rc, out = report(["trend", cs, "--allow", allow])
        self.assertIn("19 samples", out)


if __name__ == "__main__":
    unittest.main()
