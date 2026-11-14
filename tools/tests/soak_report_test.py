#!/usr/bin/env python3
"""Host test of the soak parser and the expected-against-measured report (#424): recorded fw-575798b9 lines (as quoted
on #409, #416, #419; the quotes cut the long lines, so only the keys shown exist), a figure over, a figure under, a
missing key, a reboot inside the run, -1 values, and the shipped budget file. Run with python3 -I."""
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

REPLY = """\
soak1 v=1 boot=30 up=91751 rst=sw rstcrash=0 crash=0 slot=0 slotok=1 int=179147 intmin=172563 intbig=69620 intwin=172563 intwx=1 dma=152991 dmamin=146407 dmabig=69620 dmawin=146407 dmawx=1 ps=26034464
soak2 v=1 boot=30 up=91751 lock=0 canary=0 c6=up c6rs=0 hb=2 hbmiss=0 hbdrop=0 pooltx=0 poolrx=0 lora=rx lpkt=12 lcrc=5 c6up=60 lage=27 lrx=17 ltx=0
soak3 v=1 boot=30 up=91751 mrx=17 mtx=0 mfail=0 mtmo=0 mref=0 mdrop=0 airlog=17 airdrop=0 wdbm=-61 wup=1 therm=ok store=2961408 sd=mounted sdok=1 sdfree=31088705536 sderr=0 bat=absent batmv=-1 kbd=1
soak4 v=1 boot=30 up=91751 mslow=0 mslowms=0 mslowfile=0 stlast=3058 stmax=3058 nslow=1 nslowms=3061 uiref=0 uiwait=0 wqfull=0 c6qfull=0 toastdrop=0 notdrop=0 jpend=0 jdrop=0 c6busy=0
"""

BUDGETS = """\
# test budgets
dma_free key dma >=89981 - B #234
dma_min key dmamin >=80650 - B #234
crashes key crash <=0 - count #407
nslow key nslowms <=500 - ms code
mem key ps 26000000 2% B #1 two-sided
stor key stmax - - ms none
need key nosuchkey <=0 - count #1
probe1 probe first-frame-ms <=3000 - ms #205
loop log took\\s(\\d+)\\sms <200 - ms code
"""


def put(path, text):
    with open(path, "w") as f:
        f.write(text)


def sample(**kw):
    s = {k: v for k, v in sp.parse_reply(REPLY)[0].items() if isinstance(v, int)}
    s.update(kw)
    return s


def run(samples, budgets=BUDGETS, log=None):
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, "b.txt")
        put(p, budgets)
        budgets = sr.load_budgets(p)
        rows, failed = sr.run(budgets, samples, None if log is None else sr.scan_log(log.splitlines(), budgets))
    return {r[0]: r for r in rows}, failed


class ParseTest(unittest.TestCase):
    def test_reply_merges_four_lines(self):
        (s,) = sp.parse_reply(REPLY)
        self.assertEqual(s["boot"], 30)
        self.assertEqual(s["dmamin"], 146407)
        self.assertEqual(s["rst"], "sw")
        self.assertEqual(s["therm"], "ok")
        self.assertEqual(s["stmax"], 3058)
        self.assertEqual(s["wdbm"], -61)

    def test_two_replies_are_two_samples(self):
        self.assertEqual(len(sp.parse_reply(REPLY + "console noise\n" + REPLY)), 2)

    def test_minus_one_is_unknown(self):
        (s,) = sp.parse_reply(REPLY)
        self.assertEqual(s["batmv"], -1)
        self.assertIsNone(sp.value(s, "batmv"))
        self.assertIsNone(sp.value(s, "absent"))
        self.assertIsNone(sp.value(s, "rst"))
        self.assertEqual(sp.value(s, "hb"), 2)

    def test_not_a_soak_line(self):
        self.assertIsNone(sp.parse_line("I (12) sys: up=5"))
        self.assertIsNone(sp.parse_line("soakx v=1"))

    def test_prefixed_and_crlf_lines(self):
        lines = REPLY.splitlines()
        text = "\r\n".join(["fw: " + lines[0], "12:00:01 " + lines[1], ">> " + lines[2], lines[3]]) + "\r\n"
        (s,) = sp.parse_reply(text)
        self.assertNotIn("incomplete", s)
        self.assertEqual(s["stmax"], 3058)
        self.assertEqual(s["therm"], "ok")

    def test_duplicate_key_keeps_first(self):
        self.assertEqual(sp.parse_line("soak1 v=1 boot=3 up=4 boot=9")[1]["boot"], 3)

    def test_lost_line_does_not_merge(self):
        lines = REPLY.splitlines()
        text = REPLY + "\n".join(lines[1:]) + "\n" + REPLY      # the second reply lost its soak1
        a, b, c = sp.parse_reply(text)
        self.assertNotIn("incomplete", a)
        self.assertEqual(b["incomplete"], 1)
        self.assertNotIn("dmamin", b)
        self.assertNotIn("incomplete", c)
        self.assertEqual(len(sp.usable([a, b, c])), 2)

    def test_mismatched_boot_is_incomplete(self):
        lines = REPLY.splitlines()
        (s,) = sp.parse_reply("\n".join(lines[:3] + [lines[3].replace("boot=30", "boot=31")]))
        self.assertEqual(s["incomplete"], 1)

    def test_reboots_track_last_known(self):
        self.assertEqual(sp.reboots([{"boot": 5, "up": 9}, {"boot": -1, "up": -1}, {"boot": 4, "up": 20}]), [2])

    def test_reboots(self):
        a = {"boot": 30, "up": 100}
        self.assertEqual(sp.reboots([a, {"boot": 30, "up": 700}]), [])
        self.assertEqual(sp.reboots([a, {"boot": 31, "up": 60}]), [1])      # boot higher
        self.assertEqual(sp.reboots([a, {"boot": 1, "up": 700}]), [1])      # NVS restarted: boot lower
        self.assertEqual(sp.reboots([a, {"boot": 30, "up": 50}]), [1])      # up lower
        self.assertEqual(sp.reboots([a, {"boot": -1, "up": -1}]), [])       # unknown is no change

    def test_csv_round_trip(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "s.csv")
            sp.write_csv(p, sp.parse_reply(REPLY))
            (s,) = sp.read_csv(p)
        self.assertEqual(s["int"], 179147)
        self.assertEqual(s["batmv"], -1)
        self.assertEqual(s["bat"], "absent")


class ReportTest(unittest.TestCase):
    def test_recorded_sample_flags(self):
        rows, failed = run([sample()])
        self.assertEqual(rows["dma_free"][5], "ok")
        self.assertEqual(rows["crashes"][5], "ok")
        self.assertEqual(rows["nslow"][5], "breach")      # 3061 ms against 500: the known off-plan figure
        self.assertEqual(rows["stor"][5], "unplanned")
        self.assertEqual(rows["probe1"][5], "no data")
        self.assertEqual(rows["loop"][5], "no data")      # no --log
        self.assertTrue(failed)

    def test_over_and_under(self):
        rows, _ = run([sample(dmamin=70000, crash=1, ps=30000000)])
        self.assertEqual(rows["dma_min"][5], "breach")     # a floor that was undercut
        self.assertEqual(rows["dma_min"][4], "70000")      # worst of a floor is the minimum
        self.assertEqual(rows["crashes"][5], "breach")
        self.assertEqual(rows["mem"][5], "over")
        rows, _ = run([sample(ps=1000000)])
        self.assertEqual(rows["mem"][5], "under")          # much cheaper than expected

    def test_within_tolerance(self):
        rows, _ = run([sample(ps=26100000)])
        self.assertEqual(rows["mem"][5], "ok")

    def test_missing_key_fails(self):
        rows, failed = run([sample(nslowms=10)])
        self.assertEqual(rows["need"][5], "no data")
        self.assertTrue(failed)                            # a planned key the CSV lacks
        _, failed = run([sample(nslowms=10)], BUDGETS.replace("need key nosuchkey <=0 - count #1\n", ""))
        self.assertFalse(failed)                           # without it the run is clean: probes and logs only show
        ok_only = "dma_free key dma >=89981 - B #234\nprobe1 probe p <=1 - ms #1\n"
        self.assertFalse(run([sample()], ok_only)[1])

    def test_unknown_values_are_not_measured(self):
        rows, _ = run([sample(dmamin=-1)])
        self.assertEqual(rows["dma_min"][5], "no data")
        rows, _ = run([sample(dmamin=-1), sample(dmamin=146407)])
        self.assertEqual(rows["dma_min"][3], "146407")
        self.assertEqual(rows["dma_min"][5], "ok")

    def test_reboot_inside_run(self):
        a = sample(boot=30, up=100000, crash=0, hb=500)
        b = sample(boot=31, up=40, crash=1, hb=2, dmamin=75000)
        rows, failed = run([a, b])
        self.assertEqual(rows["crashes"][4], "1")          # the run's worst, whichever boot it came from
        self.assertEqual(rows["dma_min"][4], "75000")
        self.assertTrue(failed)
        self.assertIn("reboots inside the run: 1 (at samples 1)", sr.render([], [a, b]))

    def test_log_numbers(self):
        log = "I (1) x: fine\nW (2) mesh: MeshCore loop took 650 ms\nW (3) mesh: MeshCore loop took 250 ms\n"
        rows, _ = run([sample()], log=log)
        self.assertEqual(rows["loop"][5], "breach")
        self.assertEqual(rows["loop"][4], "650")
        self.assertEqual(rows["loop"][3], "450")
        self.assertEqual(run([sample()], log="x\nMeshCore loop took 200 ms\n")[0]["loop"][5], "breach")  # < not <=

    def test_bad_budget_files(self):
        base = "a key dma >=1 - B x\n"
        bad = {
            "only three columns": "only three columns\n",
            "expected": "a key dma <=abc - B x\n",
            "tolerance": "a key dma 5 abc B x\n",
            "one-sided": "a key dma <=5 2 B x\n",
            "unplanned": "a unplanned dma 5 - B x\n",
            "duplicate": base + base,
            "compile": "a log (unclosed <5 - ms x\n",
            "exactly one group": "a log two(\\d)(\\d) <5 - ms x\n",
            "no group": "a log took <5 - ms x\n",
        }
        for why, text in bad.items():
            with tempfile.TemporaryDirectory() as d:
                p = os.path.join(d, "b.txt")
                put(p, "# header\n" + text)
                with self.assertRaises(sr.BudgetError, msg=why) as cm:
                    sr.load_budgets(p)
                self.assertRegex(str(cm.exception), r"b\.txt:\d+: ", why)

    def test_absolute_and_percent_tolerance(self):
        b = "a key ps 100 5 B x\nb key ps 100 5% B x\n"
        rows, _ = run([sample(ps=106)], b)
        self.assertEqual((rows["a"][5], rows["b"][5]), ("over", "over"))
        rows, _ = run([sample(ps=105)], b)
        self.assertEqual((rows["a"][5], rows["b"][5]), ("ok", "ok"))
        rows, _ = run([sample(ps=94)], b)
        self.assertEqual(rows["a"][5], "under")

    def test_two_sided_median_ok_but_spike_not(self):
        b = "a key ps 100 5 B x\n"
        rows, _ = run([sample(ps=100), sample(ps=101), sample(ps=100), sample(ps=130)], b)
        self.assertEqual(rows["a"][3], "100.50")
        self.assertEqual(rows["a"][5], "over")
        self.assertEqual(rows["a"][4], "130")

    def test_ceiling_worst_over_many_samples(self):
        b = "a key nslowms <=500 - ms x\n"
        rows, _ = run([sample(nslowms=10), sample(nslowms=600), sample(nslowms=20)], b)
        self.assertEqual((rows["a"][4], rows["a"][5]), ("600", "breach"))
        rows, _ = run([sample(nslowms=10), sample(nslowms=20)], b)
        self.assertEqual((rows["a"][4], rows["a"][5]), ("20", "ok"))
        f = "a key dmamin >=100 - B x\n"
        rows, _ = run([sample(dmamin=500), sample(dmamin=50), sample(dmamin=300)], f)
        self.assertEqual((rows["a"][4], rows["a"][5]), ("50", "breach"))

    def test_counter_reset_at_reboot_still_counts(self):
        a = sample(boot=30, up=100000, crash=1)
        b = sample(boot=31, up=40, crash=0)
        rows, failed = run([a, b], "c key crash <=0 - count x\n")
        self.assertEqual((rows["c"][4], rows["c"][5], failed), ("1", "breach", True))

    def test_incomplete_samples_are_left_out(self):
        bad = sample(dmamin=1, incomplete=1)
        rows, _ = run([sample(), bad])
        self.assertEqual(rows["dma_min"][5], "ok")
        self.assertIn("incomplete (left out): 1", sr.render([], [sample(), bad]))

    def test_no_log_is_said(self):
        self.assertIn("no --log given", sr.render([], [sample()], log_given=False))
        self.assertNotIn("no --log", sr.render([], [sample()], log_given=True))

    def test_exit_2_on_tool_errors(self):
        with tempfile.TemporaryDirectory() as d:
            c = os.path.join(d, "s.csv")
            sp.write_csv(c, [sample()])
            b = os.path.join(d, "b.txt")
            put(b, "# x\na key dma <=abc - B x\n")
            err = io.StringIO()
            with contextlib.redirect_stderr(err):
                self.assertEqual(sr.main(["r", "expected", c, "--budgets", b]), 2)
                self.assertEqual(sr.main(["r", "expected", os.path.join(d, "none.csv")]), 2)
                self.assertEqual(sr.main(["r", "expected", c, "--log", os.path.join(d, "none.log")]), 2)
            self.assertIn("b.txt:2:", err.getvalue())

    def test_help(self):
        with contextlib.redirect_stdout(io.StringIO()) as out:
            self.assertEqual(sr.main(["r", "--help"]), 0)
        self.assertIn("breach", out.getvalue())


class ShippedBudgetsTest(unittest.TestCase):
    def test_file_parses_and_names_known_keys(self):
        budgets = sr.load_budgets(sr.DEFAULT_BUDGETS)
        self.assertGreater(len(budgets), 30)
        keys = set(sp.parse_reply(REPLY)[0])
        for b in budgets:
            if b.kind == "key":
                self.assertTrue(b.measure in keys or b.measure in ("crash",), b.measure)

    def test_cli_exit_status(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "s.csv")
            sp.write_csv(p, sp.parse_reply(REPLY))
            with contextlib.redirect_stdout(io.StringIO()) as out:
                self.assertEqual(sr.main(["soak_report.py", "expected", p]), 1)  # nslowms 3061 over 500
            self.assertIn("notice_slow_ms", out.getvalue())


if __name__ == "__main__":
    unittest.main()
