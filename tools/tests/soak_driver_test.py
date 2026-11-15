#!/usr/bin/env python3
"""Host test of the use-soak plan and driver (#476) against a fake badge and a fake clock: no network, no sleeping.
Pinned: the 24 h schedule, the send budget and target, the home and unlock lines after a failed block, the stop rule
(boot, crash, slot, C6 restart: no further line), the scope read-back, the caps, the end-of-session check, the private
config. The scope names here are made up. Run with python3 -I."""
import contextlib
import csv
import io
import os
import sys
import tempfile
import unittest
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
import soak_driver as sd  # noqa: E402
import soak_plan as sp  # noqa: E402

ZERO, REGION = "zz-zero", "zz-region"
T0 = 1_800_000_000


class FakeClock:
    def __init__(self):
        self.t = float(T0)

    def now(self):
        return self.t

    def sleep(self, s):
        self.t += s


class FakeBadge:
    """Answers like the badge's console. `hook(badge, line)` runs before each line (to inject faults)."""

    def __init__(self, clock, hook=None):
        self.clock, self.hook = clock, hook
        self.boot, self.crash, self.slot, self.c6rs, self.mtx = 54, 0, 0, 0, 0
        self.scope, self.lock, self.stack = "none", False, "0"
        self.ignore_scope_set, self.stuck_stack = False, False
        self.log, self.sent_at = [], []

    def soak(self):
        up = int(self.clock.now()) - T0 + 1000
        common = "v=1 boot=%d up=%d" % (self.boot, up)
        return "\n".join(["soak1 %s crash=%d slot=%d dma=151091" % (common, self.crash, self.slot),
                          "soak2 %s c6rs=%d hb=5" % (common, self.c6rs),
                          "soak3 %s mtx=%d mrx=0" % (common, self.mtx),
                          "soak4 %s jpend=0" % common])

    def run(self, line):
        self.log.append(line)
        if self.hook:
            self.hook(self, line)
        w = line.split()
        if line == "soak":
            return 0, self.soak() + "\n"
        if line == "mesh scope":
            return 0, "default None | console %s\n" % self.scope
        if line.startswith("mesh scope console "):
            if not self.ignore_scope_set:
                self.scope = w[3]
            return 0, "console scope set\n"
        if line.startswith("mesh send "):
            self.mtx += 1
            self.sent_at.append(self.clock.now())
            return 0, "queued #%d on %s\n" % (self.mtx, w[2])
        if line.startswith("ui lock on"):
            self.lock = True
            return 0, "ui lock: on, 120 s after the last command\n"
        if line == "ui lock off":
            self.lock = False
            return 0, "ui lock: off\n"
        if line.startswith("ui open "):
            if not self.stuck_stack or w[2] != "home":
                self.stack = "0" if w[2] == "home" else "0 > 3"
            return 0, "ui open: %s\n" % w[2]
        if line == "ui":
            return 0, "ui stack: %s | focus: x\nui lock: %s\n" % (self.stack, "on, 100 s left" if self.lock else "off")
        if line.startswith("mesh chat open"):
            return 0, "chat opened: #badgetest\n"
        return 0, "ok\n"


class Env:
    def __init__(self, test, profile=sp.FULL, hook=None, notify=None):
        self.dir = tempfile.mkdtemp()
        test.addCleanup(lambda: __import__("shutil").rmtree(self.dir, ignore_errors=True))
        self.clock = FakeClock()
        self.badge = FakeBadge(self.clock, hook)
        self.notes = []
        self.driver = sd.Driver(profile, sd.Config(ZERO, REGION), self.badge, self.clock, self.dir,
                                self.notes.append if notify is None else notify)

    def start(self):
        self.driver.start = self.clock.now()
        self.driver.run_id = "abcd"
        self.driver.read_state()

    def ledger(self):
        with open(os.path.join(self.dir, "ledger.csv"), newline="") as f:
            return list(csv.DictReader(f))

    def read(self, name):
        with open(os.path.join(self.dir, name)) as f:
            return f.read()


class PlanTest(unittest.TestCase):
    def test_24h_schedule(self):
        s = sp.plan_sessions(sp.FULL)
        self.assertEqual(len(s), 48)
        self.assertEqual({b: sum(1 for x in s if x.block == b) for b in sp.BLOCKS}, {b: 8 for b in sp.BLOCKS})
        self.assertTrue(all(x.start_s < 16 * 3600 and x.segment == "awake" for x in s))
        self.assertEqual([x.start_s for x in s][:3], [0, 1200, 2400])
        self.assertEqual(sp.segment_at(sp.FULL, 16 * 3600), "night")

    def test_send_budget(self):
        t = sp.totals(sp.FULL)
        self.assertEqual((t["sends"], t["regional"], t["zero_hop"]), (96, 4, 92))
        self.assertLessEqual(t["max_per_hour"], 6)
        reg = [x for x, k in sp.send_schedule(sp.plan_sessions(sp.FULL)) if k == sp.REGIONAL]
        self.assertEqual(len(reg), 4)
        self.assertTrue(all(b - a >= 4 * 3600 for a, b in zip(reg, reg[1:])))
        self.assertEqual(sp.max_in_window([0, 1, 2, 3, 4, 5, 6]), 7)  # the window function itself counts
        self.assertEqual(sp.max_in_window([0, 3600]), 1)

    def test_rehearsal(self):
        s = sp.plan_sessions(sp.REHEARSAL)
        self.assertEqual([x.block for x in s], list(sp.BLOCKS))
        t = sp.totals(sp.REHEARSAL)
        self.assertLessEqual(t["sends"], 6)
        self.assertEqual(t["regional"], 1)
        self.assertEqual(sp.REHEARSAL.duration_s, 3600)

    def test_every_block_ends_home_and_never_presses_enter(self):
        for b in sp.BLOCKS:
            steps = sp.session_steps(sp.Session(1, b, 0, "awake", (sp.ZERO_HOP,)))
            self.assertEqual([x.text for x in steps[-2:]], ["ui open home", "ui lock off"])
            self.assertEqual(steps[0].text, "idle test 0")
            for x in steps:
                self.assertNotRegex(x.text, r"\benter\b|\bdm\b|advert|remote|trace|discover|telemetry|history|"
                                             r"contacts import|channel|radio|name")

    def test_target_check(self):
        self.assertTrue(sp.target_is_test_channel(sp.send_line("ab12", 3)[0]))
        for bad in ("mesh send #general hi", "mesh send #badgetest2 hi", "mesh send #badgetest", "mesh dm abc hi",
                    "mesh send badgetest hi"):
            self.assertFalse(sp.target_is_test_channel(bad), bad)

    def test_dry_run_totals(self):
        out = io.StringIO()
        sd.dry_run(False, out)
        text = out.getvalue()
        self.assertIn("48 sessions (0 in the night)", text)
        self.assertIn("sends: 96 (92 zero_hop, 4 regional), at most 6 in any hour", text)
        out = io.StringIO()
        sd.dry_run(True, out)
        self.assertIn("6 sessions", out.getvalue())


class RunTest(unittest.TestCase):
    def test_full_day_run(self):
        e = Env(self)
        self.assertEqual(e.driver.run(), 0)
        sends = [x for x in e.badge.log if x.startswith("mesh send ")]
        self.assertEqual(len(sends), 96)
        self.assertTrue(all(sp.target_is_test_channel(x) for x in sends))
        times = e.badge.sent_at
        self.assertTrue(all(sum(1 for u in times if t <= u < t + 3600) <= 6 for t in times))
        rows = e.ledger()
        self.assertEqual(sum(1 for r in rows if r["scope"] == "regional"), 4)
        self.assertTrue(all(r["result"] == "sent" and int(r["tx_after"]) == int(r["tx_before"]) + 1 for r in rows))
        tr = e.read("transcript.txt")
        self.assertEqual(tr.count("## session"), 96)
        self.assertIn("## segment night %d" % (T0 + 16 * 3600), tr)
        self.assertEqual(tr.count("## sample "), 145)
        self.assertFalse(os.path.exists(os.path.join(e.dir, "BLOCKER")))
        # no session line after the night begins
        night = [i for i, x in enumerate(tr.splitlines()) if x.startswith("## segment night")][0]
        self.assertNotIn("## session", "\n".join(tr.splitlines()[night:]))

    def test_stop_file_ends_cleanly(self):
        e = Env(self, hook=lambda b, line: open(os.path.join(e.dir, "STOP"), "w").close()
                if len(b.log) == 30 else None)
        self.assertEqual(e.driver.run(), 0)
        self.assertLess(len(e.badge.log), 60)
        self.assertFalse(os.path.exists(os.path.join(e.dir, "BLOCKER")))

    def test_transcript_is_redacted(self):
        e = Env(self)
        orig = e.badge.run
        e.badge.run = lambda line: (0, "from 192.0.2.7 aa:bb:cc:dd:ee:ff scope #%s\n" % ZERO) if line == "mem" else orig(line)
        e.start()
        e.driver.line("mem")
        tr = e.read("transcript.txt")
        self.assertNotIn("192.0.2.7", tr)
        self.assertNotIn("aa:bb", tr)
        self.assertNotIn(ZERO, tr)
        self.assertIn("<ip>", tr)


class SessionTest(unittest.TestCase):
    # block -> (the line that fails, a later line of the block that must then not run)
    FAILS = {"contacts": ("ui open meshcore", "ui perf"), "tour": ("ui open clock", "ui open timer"),
             "files": ("ui open files", "ui perf"), "web": ("ui open weather", sp.keys("wait", "wait"))}

    def test_failed_block_still_goes_home_and_unlocks(self):
        for block, (fail, later) in self.FAILS.items():
            with self.subTest(block):
                e = Env(self)
                orig = e.badge.run

                def run(line, orig=orig, fail=fail):
                    rc, out = orig(line)
                    return (rc, "ui open: not now (a dialog is open)\n") if line == fail else (rc, out)

                e.badge.run = run
                e.start()
                e.driver.session(sp.Session(1, block, 0, "awake", ()))
                log = e.badge.log
                self.assertIn(fail, log)
                self.assertNotIn(later, log)
                self.assertEqual(log[-4:-2], ["ui open home", "ui lock off"])
                self.assertIn("error", e.read("transcript.txt"))

    def test_exception_in_a_line_still_tails(self):
        e = Env(self)
        orig = e.badge.run
        e.badge.run = lambda line: (1, "boom") if line.startswith("keys send") else orig(line)
        e.start()
        e.driver.session(sp.Session(1, "contacts", 0, "awake", ()))
        self.assertEqual(e.badge.log[-4:-2], ["ui open home", "ui lock off"])

    def test_session_not_ending_home_is_a_blocker(self):
        for fault in ("stack", "lock"):
            with self.subTest(fault):
                e = Env(self)
                e.start()
                if fault == "stack":
                    e.badge.stuck_stack, e.badge.stack = True, "0 > 3"
                else:
                    orig = e.badge.run
                    e.badge.run = lambda line, orig=orig: (0, "ui lock: on, 5 s left\n") if line == "ui lock off" else orig(line)
                with self.assertRaises(sd.Blocker):
                    e.driver.session(sp.Session(1, "idle", 0, "awake", ()))

    def test_ordering_of_a_chat_session(self):
        e = Env(self)
        e.start()
        e.driver.session(sp.Session(1, "chat", 0, "awake", (sp.REGIONAL, sp.ZERO_HOP)))
        lines = [x for x in e.badge.log if x not in ("soak", "mesh scope", "ui")]
        self.assertEqual(lines[0], "idle test 0")
        self.assertEqual(lines[1], "ui lock on 120")
        self.assertEqual(lines[2], "mesh chat open #badgetest")
        self.assertEqual(lines[-2:], ["ui open home", "ui lock off"])
        self.assertEqual(sum(1 for x in lines if x.startswith("mesh send ")), 2)


class StopRuleTest(unittest.TestCase):
    def test_changed_counter_stops_and_no_further_line(self):
        for field, value in (("boot", 55), ("crash", 1), ("slot", 1), ("c6rs", 1)):
            with self.subTest(field):
                state = {}

                def hook(b, line, field=field, value=value, state=state):
                    if line == "mesh scope" and not state:  # first send of the run, right before its state read
                        setattr(b, field, value)
                        state["at"] = len(b.log)

                e = Env(self, hook=hook)
                self.assertEqual(e.driver.run(), 1)
                after = e.badge.log[state["at"]:]
                self.assertEqual(after[-1], "soak")  # the state read that saw it was the last line sent
                self.assertEqual(after.count("soak"), 1)
                self.assertTrue(all(x.startswith("mesh scope") for x in after[:-1]))
                self.assertFalse(any(x.startswith("mesh send ") for x in e.badge.log))
                self.assertIn("BLOCKER", os.listdir(e.dir))
                self.assertEqual(len(e.notes), 1)
                self.assertNotRegex(e.notes[0], r"\d+\.\d+\.\d+\.\d+")

    def test_restart_during_the_sampler(self):
        e = Env(self, hook=lambda b, line: setattr(b, "boot", 99) if line == "soak" and b.log.count("soak") == 9 else None)
        self.assertEqual(e.driver.run(), 1)
        self.assertEqual(e.badge.log[-1], "soak")

    def test_no_answer_is_a_blocker(self):
        e = Env(self)
        e.start()
        e.badge.run = lambda line: (0, "")
        with self.assertRaises(sd.Blocker):
            e.driver.read_state()

    def test_start_refuses_when_blocker_present(self):
        e = Env(self)
        open(os.path.join(e.dir, "BLOCKER"), "w").close()
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(e.driver.run(), 2)
        self.assertEqual(e.badge.log, [])


class SendCheckTest(unittest.TestCase):
    def test_wrong_scope_readback_skips_the_send(self):
        e = Env(self)
        e.start()
        e.badge.ignore_scope_set = True
        e.driver.send(sp.ZERO_HOP)
        self.assertFalse(any(x.startswith("mesh send ") for x in e.badge.log))
        self.assertTrue(e.ledger()[0]["result"].startswith("skipped: scope read-back"))
        self.assertEqual(e.driver.skips, 1)

    def test_three_failed_checks_in_a_row_are_a_blocker(self):
        e = Env(self)
        e.start()
        e.badge.ignore_scope_set = True
        e.driver.send(sp.ZERO_HOP)
        e.driver.send(sp.ZERO_HOP)
        with self.assertRaises(sd.Blocker):
            e.driver.send(sp.ZERO_HOP)

    def test_a_good_send_resets_the_count(self):
        e = Env(self)
        e.start()
        e.badge.ignore_scope_set = True
        e.driver.send(sp.ZERO_HOP)
        e.driver.send(sp.ZERO_HOP)
        e.badge.ignore_scope_set = False
        e.driver.send(sp.ZERO_HOP)
        self.assertEqual(e.driver.skips, 0)
        e.badge.ignore_scope_set = True
        e.badge.scope = "other"
        e.driver.send(sp.ZERO_HOP)  # a third skip overall, but only the first in a row
        self.assertEqual(e.driver.skips, 1)

    def test_scope_is_set_to_the_plans_scope(self):
        e = Env(self)
        e.start()
        e.driver.send(sp.REGIONAL)
        self.assertEqual(e.badge.scope, REGION)
        e.clock.sleep(60)
        e.driver.send(sp.ZERO_HOP)
        self.assertEqual(e.badge.scope, ZERO)

    def test_wrong_target_is_skipped(self):
        e = Env(self)
        e.start()
        with mock.patch.object(sp, "send_line", lambda run, seq: ("mesh send #general soak", "soak")):
            e.driver.send(sp.ZERO_HOP)
        self.assertFalse(any(x.startswith("mesh send ") for x in e.badge.log))
        self.assertIn("skipped: target", e.ledger()[0]["result"])

    def test_caps(self):
        e = Env(self)
        d = e.driver
        d.attempts = [(1000.0 + i, sp.ZERO_HOP) for i in range(6)]
        self.assertEqual(d.cap_problem(sp.ZERO_HOP, 1100.0), "hour cap")
        self.assertEqual(d.cap_problem(sp.ZERO_HOP, 1000.0 + 3600), "")
        d.attempts = [(0.0, sp.ZERO_HOP)] * 100
        self.assertEqual(d.cap_problem(sp.ZERO_HOP, 99999.0), "total cap")
        d.attempts = [(0.0, sp.REGIONAL)]
        self.assertEqual(d.cap_problem(sp.REGIONAL, 3 * 3600.0), "regional interval")
        self.assertEqual(d.cap_problem(sp.REGIONAL, 4 * 3600.0), "")

    def test_cap_holds_in_a_run_that_asks_for_more(self):
        many = sp.Profile("x", 3600, 0, 60, 2, 4 * 3600, 100, 6, 600)  # 60 sessions an hour, 2 sends each
        e = Env(self, profile=many)
        e.driver.run()
        self.assertEqual(sum(1 for x in e.badge.log if x.startswith("mesh send ")), 6)


class ConfigTest(unittest.TestCase):
    def write(self, text):
        d = tempfile.mkdtemp()
        self.addCleanup(lambda: __import__("shutil").rmtree(d, ignore_errors=True))
        p = os.path.join(d, "c.conf")
        with open(p, "w") as f:
            f.write(text)
        return p

    def test_loads_and_normalises(self):
        c = sd.load_config(self.write("# x\nscope_zero_hop=#ZZ-Zero\nscope_regional = zz-region # r\n"))
        self.assertEqual(c.scopes, {sp.ZERO_HOP: "zz-zero", sp.REGIONAL: "zz-region"})

    def test_refuses_without_file_or_keys(self):
        for text in (None, "", "scope_zero_hop=zz-a\n", "scope_zero_hop=zz-a\nscope_regional=zz-a\n",
                     "scope_zero_hop=zz-a\nscope_regional=*\n"):
            with self.assertRaises(ValueError):
                sd.load_config("/nonexistent/x.conf" if text is None else self.write(text))

    def test_main_refuses_to_start_without_config(self):
        d = tempfile.mkdtemp()
        self.addCleanup(lambda: __import__("shutil").rmtree(d, ignore_errors=True))
        ipf = os.path.join(d, "ip")
        with open(ipf, "w") as f:
            f.write("x")
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            rc = sd.main(["soak_driver.py", "--badge-ip-file", ipf, "--run-dir", d, "--config", os.path.join(d, "none")])
        self.assertEqual(rc, 2)
        self.assertNotIn(d + "/ip", err.getvalue().replace(os.path.join(d, "none"), ""))


if __name__ == "__main__":
    unittest.main()
