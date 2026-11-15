#!/usr/bin/env python3
"""Host test of the use-soak plan and driver (#476) against a fake badge and a fake clock: no network, no sleeping.
Pinned: the 24 h schedule, the send budget and target, the home and unlock lines after a failed block, the stop rule
(boot, crash, slot, C6 restart: no further line), the scope read-back, the caps, the end-of-session check, the private
config. The scope names here are made up. Run with python3 -I."""
import contextlib
import csv
import io
import os
import re
import signal
import sys
import tempfile
import threading
import time
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
        self.wall_jump = 0.0

    def now(self):
        return self.t + self.wall_jump

    def mono(self):
        return self.t

    def sleep(self, s):
        self.t += s


class FakeBadge:
    """Answers like the badge's console. `hook(badge, line)` runs before each line (to inject faults)."""

    def __init__(self, clock, hook=None, line_s=0.0):
        self.clock, self.hook, self.line_s = clock, hook, line_s
        self.address = "badge.example.lan"
        self.boot, self.crash, self.slot, self.c6rs, self.mtx = 54, 0, 0, 0, 0
        self.scope, self.lock, self.stack, self.focus = "none", False, "0", "Home"
        self.right_stuck = False
        self.ignore_scope_set, self.stuck_stack = False, False
        self.log, self.sent_at = [], []

    def soak(self):
        up = int(self.clock.t) - T0 + 1000
        common = "v=1 boot=%d up=%d" % (self.boot, up)
        return "\n".join(["soak1 %s crash=%d slot=%d dma=151091" % (common, self.crash, self.slot),
                          "soak2 %s c6rs=%d hb=5" % (common, self.c6rs),
                          "soak3 %s mtx=%d mrx=0" % (common, self.mtx),
                          "soak4 %s jpend=0" % common])

    def run(self, line):
        self.log.append(line)
        self.clock.t += self.line_s
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
                self.stack = {"home": "0", "meshcore": "0 > 2"}.get(w[2], "0 > 3")
                self.focus = {"home": "Home", "meshcore": "Chat"}.get(w[2], "x")
            return 0, "ui open: %s\n" % w[2]
        if line == "ui":
            return 0, "ui stack: %s | focus: %s\nui lock: %s\n" % (self.stack, self.focus,
                                                                  "on, 100 s left" if self.lock else "off")
        if line == "keys send needlock right":
            if self.focus == "Chat" and not self.right_stuck:
                self.focus = "Contacts"
            return 0, "ok\n"
        if line == sp.CONTACTS_ENTER:
            if self.focus != "Contacts":
                return 0, "keys send: guard 1 failed: focus '%s'; 0 of 3 sent\n" % self.focus
            self.stack, self.focus = "0 > 2 > 5", "row"
            return 0, "ok\n"
        if line.startswith("mesh chat open"):
            return 0, "chat opened: #badgetest\n"
        return 0, "ok\n"


class Env:
    def __init__(self, test, profile=sp.FULL, hook=None, notify=None, line_s=0.0):
        self.dir = tempfile.mkdtemp()
        test.addCleanup(lambda: __import__("shutil").rmtree(self.dir, ignore_errors=True))
        self.clock = FakeClock()
        self.badge = FakeBadge(self.clock, hook, line_s)
        self.notes = []
        self.driver = sd.Driver(profile, sd.Config(ZERO, REGION), self.badge, self.clock, self.dir,
                                self.notes.append if notify is None else notify)

    def start(self):
        self.driver.start = self.clock.now()
        self.driver.run_id = "abcd"
        self.driver.read_state()

    def ledger(self):
        with open(self.driver.ledger_path(), newline="") as f:
            return list(csv.DictReader(f))

    def seed_ledger(self, rows):
        """rows: (seq, age_s, scope, result); the ages are before the run's start."""
        with open(self.driver.ledger_path(), "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["seq", "time", "scope", "id", "tx_before", "tx_after", "result"])
            for seq, age, scope, result in rows:
                w.writerow([seq, int(T0 - age), scope, "soak x %d" % seq, 0, 0, result])

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
                self.assertNotRegex(x.text, r"\bdm\b|advert|remote|trace|discover|telemetry|history|"
                                             r"contacts import|channel|radio|name")
                if re.search(r"\benter\b", x.text):  # the one guarded Enter of the plan, in the contacts block only
                    self.assertEqual((b, x.text), ("contacts", sp.CONTACTS_ENTER))
        self.assertIn("focus:Contacts|", sp.CONTACTS_ENTER)

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
        self.assertEqual(e.driver.bad, 1)

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
        self.assertEqual(e.driver.bad, 0)
        e.badge.ignore_scope_set = True
        e.badge.scope = "other"
        e.driver.send(sp.ZERO_HOP)  # a third skip overall, but only the first in a row
        self.assertEqual(e.driver.bad, 1)

    def test_scope_is_set_to_the_plans_scope(self):
        e = Env(self)
        e.start()
        e.driver.send(sp.REGIONAL)
        self.assertIn("mesh scope console %s YES" % REGION, e.badge.log)  # the regional line went out under it
        self.assertLess(e.badge.log.index("mesh scope console %s YES" % REGION), e.badge.log.index(
            [x for x in e.badge.log if x.startswith("mesh send ")][0]))
        self.assertEqual(e.badge.scope, ZERO)  # and the scope is put back at once
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
        d.attempts = [(1000.0 + i, sp.ZERO_HOP, None) for i in range(6)]
        self.assertEqual(d.cap_problem(sp.ZERO_HOP, 1100.0), "hour cap")
        self.assertEqual(d.cap_problem(sp.ZERO_HOP, 1000.0 + 3600), "")
        d.attempts = [(0.0, sp.ZERO_HOP, None)] * 100
        self.assertEqual(d.cap_problem(sp.ZERO_HOP, 99999.0), "total cap")
        d.attempts = [(0.0, sp.REGIONAL, None)]
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
        for text in (None, "", "scope_zero_hop=zzz-a\n", "scope_zero_hop=zzz-a\nscope_regional=zzz-a\n",
                     "scope_zero_hop=zzz-a\nscope_regional=*\n", "scope_zero_hop=zz\nscope_regional=zzz-b\n",
                     "scope_zero_hop=zzz-a\nscope_regional=zzz b;x\n", "scope_zero_hop=zzz-a\nscope_regional=zzz\\nmesh\n"):
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

class ReloadTest(unittest.TestCase):
    """H1: a rerun in the same run directory continues the count."""

    def test_caps_and_interval_come_back_from_the_ledger(self):
        e = Env(self)
        e.seed_ledger([(i, 100, "zero_hop", "sent") for i in range(1, 7)] + [(7, 100, "regional", "failed"),
                                                                               (8, 90, "zero_hop", "skipped: x")])
        e.start()
        e.driver.load_ledger()
        self.assertEqual(len(e.driver.attempts), 7)  # failed counts, skipped does not
        self.assertEqual(e.driver.seq, 8)
        self.assertEqual(e.driver.cap_problem(sp.ZERO_HOP, e.clock.mono()), "hour cap")
        self.assertEqual(e.driver.cap_problem(sp.REGIONAL, e.clock.mono() + 7200), "regional interval")

    def test_rerun_sends_nothing_past_the_caps(self):
        e = Env(self, profile=sp.REHEARSAL)
        e.seed_ledger([(i, 100, "zero_hop", "sent") for i in range(1, 7)])
        self.assertEqual(e.driver.run(), 1)  # three skipped sends in a row
        self.assertFalse(any(x.startswith("mesh send ") for x in e.badge.log))

    def test_rerun_does_not_repeat_the_regional_line(self):
        e = Env(self, profile=sp.REHEARSAL)
        e.seed_ledger([(1, 600, "regional", "sent")])
        e.driver.run()
        rows = e.ledger()
        self.assertEqual(rows[1]["scope"], "regional")
        self.assertEqual(rows[1]["result"], "skipped: regional interval")
        self.assertEqual([r["seq"] for r in rows][:2], ["1", "2"])  # the numbering goes on

    def test_total_cap_survives_a_restart(self):
        e = Env(self, profile=sp.REHEARSAL)
        e.seed_ledger([(i, 90000, "zero_hop", "sent") for i in range(1, 7)])
        e.start()
        e.driver.load_ledger()
        self.assertEqual(e.driver.cap_problem(sp.ZERO_HOP, e.clock.mono()), "total cap")


class ScopeRestoreTest(unittest.TestCase):
    """H2: the console scope never stays on the regional one."""

    def test_rehearsal_regional_session_ends_on_the_zero_hop_scope(self):
        e = Env(self, profile=sp.REHEARSAL)
        e.start()
        e.driver.session(sp.Session(1, "chat", 0, "awake", (sp.REGIONAL,)))
        self.assertEqual(e.badge.scope, ZERO)

    def test_skipped_second_send_still_restores(self):
        e = Env(self)
        e.start()
        e.driver.attempts = [(e.clock.mono(), sp.ZERO_HOP, None)] * 5  # one more send fits, the second hits the hour cap
        e.driver.session(sp.Session(1, "idle", 0, "awake", (sp.REGIONAL, sp.ZERO_HOP)))
        self.assertEqual(sum(1 for x in e.badge.log if x.startswith("mesh send ")), 1)
        self.assertEqual(e.badge.scope, ZERO)

    def test_a_failed_regional_attempt_restores(self):
        e = Env(self)
        e.start()
        orig = e.badge.run
        e.badge.run = lambda line: (0, "not queued: x\n") if line.startswith("mesh send ") else orig(line)
        e.driver.session(sp.Session(1, "idle", 0, "awake", (sp.REGIONAL,)))
        self.assertEqual(e.badge.scope, ZERO)

    def test_blocker_in_a_send_restores_but_a_restart_sends_nothing(self):
        e = Env(self, hook=lambda b, line: setattr(b, "boot", 99) if line.startswith("mesh send ") else None)
        e.start()
        with self.assertRaises(sd.Blocker):
            e.driver.send(sp.REGIONAL)
        self.assertEqual(e.badge.log[-1], "soak")  # the read that saw the restart: no scope line after it
        self.assertEqual(e.badge.scope, REGION)
        e2 = Env(self)
        e2.start()
        e2.driver.bad = 2
        orig = e2.badge.run
        e2.badge.run = lambda line: (0, "not queued: x\n") if line.startswith("mesh send ") else orig(line)
        with self.assertRaises(sd.Blocker):  # the third bad send in a row, not a restart: the scope goes back first
            e2.driver.send(sp.REGIONAL)
        self.assertEqual(e2.badge.scope, ZERO)


class UnexpectedErrorTest(unittest.TestCase):
    """H3 and the tail."""

    def raising(self, exc):
        done = []

        def hook(b, line):
            if line.startswith("keys send") and not done:
                done.append(1)
                raise exc

        return hook

    def test_oserror_mid_session_goes_home_and_blocks(self):
        e = Env(self, hook=self.raising(OSError("disk full")))
        self.assertEqual(e.driver.run(), 1)
        self.assertEqual(e.badge.log[e.badge.log.index("ui open home") - 0], "ui open home")
        self.assertIn("ui lock off", e.badge.log)
        text = e.read("BLOCKER")
        self.assertIn("OSError", text)
        self.assertNotIn("disk full", text)
        self.assertEqual(len(e.notes), 1)

    def test_ctrl_c_does_the_tail_and_exits_without_a_blocker(self):
        e = Env(self, hook=self.raising(KeyboardInterrupt()))
        self.assertEqual(e.driver.run(), 130)
        self.assertIn("ui lock off", e.badge.log)
        self.assertFalse(os.path.exists(os.path.join(e.dir, "BLOCKER")))

    def test_a_blocker_in_the_body_still_runs_the_tail_unless_the_badge_restarted(self):
        e = Env(self)
        e.start()
        e.badge.ignore_scope_set = True
        e.driver.bad = 2
        with self.assertRaises(sd.Blocker):
            e.driver.session(sp.Session(1, "chat", 0, "awake", (sp.ZERO_HOP,)))
        self.assertEqual(e.badge.log[-2:], ["ui open home", "ui lock off"])

    def test_restart_mid_body_is_named_and_sends_no_tail(self):
        e = Env(self, hook=lambda b, line: setattr(b, "boot", 99) if line == "ui open timer" else None)
        self.assertEqual(e.driver.run(), 1)
        self.assertEqual(e.badge.log[-1], "soak")
        self.assertIn("boot counter", e.read("BLOCKER"))
        self.assertNotIn("ui open home", e.badge.log[e.badge.log.index("ui open timer"):])


class SendAccountingTest(unittest.TestCase):
    def test_a_failing_scope_line_is_a_skip_with_a_ledger_row(self):
        e = Env(self)
        e.start()
        orig = e.badge.run
        e.badge.run = lambda line: (1, "") if line == "mesh scope" else orig(line)
        e.driver.send(sp.ZERO_HOP)
        self.assertEqual(e.ledger()[0]["result"], "skipped: scope read-back")
        self.assertEqual(e.driver.bad, 1)

    def test_repeated_failed_sends_are_a_blocker(self):
        e = Env(self)
        e.start()
        orig = e.badge.run
        e.badge.run = lambda line: (0, "not queued: x\n") if line.startswith("mesh send ") else orig(line)
        e.driver.send(sp.ZERO_HOP)
        e.driver.send(sp.ZERO_HOP)
        with self.assertRaises(sd.Blocker):
            e.driver.send(sp.ZERO_HOP)
        self.assertEqual([r["result"] for r in e.ledger()], ["failed"] * 3)

    def test_timeout_counts_toward_the_caps(self):
        e = Env(self)
        e.start()
        orig = e.badge.run
        e.badge.run = lambda line: (124, "timeout") if line.startswith("mesh send ") else orig(line)
        e.driver.send(sp.ZERO_HOP)
        self.assertEqual(len(e.driver.attempts), 1)
        self.assertEqual(e.ledger()[0]["result"], "failed")

    def test_two_status_lines_are_not_a_read_back(self):
        e = Env(self)
        e.start()
        orig = e.badge.run
        e.badge.run = lambda line: (0, "default None | console %s\ndefault None | console %s\n" % (ZERO, ZERO)) \
            if line == "mesh scope" else orig(line)
        e.driver.send(sp.ZERO_HOP)
        self.assertFalse(any(x.startswith("mesh send ") for x in e.badge.log))

    def test_regional_interval_holds_with_jitter_in_a_full_day(self):
        e = Env(self, line_s=7.0)  # every line takes 7 s, so real send times drift off the plan's slots
        self.assertEqual(e.driver.run(), 0)
        rows = e.ledger()
        self.assertEqual(sum(1 for r in rows if r["scope"] == "regional" and r["result"] == "sent"), 4)
        # a slow badge can push a session's sends into the next hour window: the hour cap then skips a send (by design),
        # but never the regional one for its interval, and never to a blocker
        self.assertFalse(any("regional interval" in r["result"] for r in rows))
        self.assertTrue(all(sum(1 for u in e.badge.sent_at if t <= u < t + 3600) <= 6 for t in e.badge.sent_at))

    def test_wall_clock_jump_does_not_open_the_hour(self):
        e = Env(self)
        e.start()
        e.driver.attempts = [(e.clock.mono(), sp.ZERO_HOP, None)] * 6
        e.clock.wall_jump = 7200.0
        e.driver.send(sp.ZERO_HOP)
        self.assertEqual(e.ledger()[0]["result"], "skipped: hour cap")

    def test_a_session_far_behind_is_skipped_not_caught_up(self):
        e = Env(self, profile=sp.REHEARSAL, hook=lambda b, line: setattr(b.clock, "t", b.clock.t + 5000)
                if len(b.log) == 8 else None)
        e.driver.run()
        self.assertIn("skipped late", e.read("transcript.txt"))


class RedactionTest(unittest.TestCase):
    def test_hostname_address_is_scrubbed(self):
        e = Env(self)
        e.start()
        orig = e.badge.run
        e.badge.run = lambda line: (1, "curl: could not resolve badge.example.lan\n") if line == "mem" else orig(line)
        with self.assertRaises(sd.StepError):
            e.driver.line("mem")
        self.assertNotIn("badge.example.lan", e.read("transcript.txt"))

    def test_scope_scrub_respects_word_boundaries(self):
        d = sd.Driver(sp.FULL, sd.Config("que", "qux"), FakeBadge(FakeClock()), FakeClock(), tempfile.mkdtemp())
        self.assertEqual(d.scrub("queued #5 on #badgetest"), "queued #5 on #badgetest")
        self.assertEqual(d.scrub("default None | console que"), "default None | console <scope>")
        self.assertEqual(d.scrub("console #QUX."), "console <scope>.")


class AllowlistTest(unittest.TestCase):
    def test_lines_outside_the_plan_are_refused(self):
        names = (ZERO, REGION)
        for ok in ("mesh send #badgetest soak ab12 3", "keys send needlock down up", sp.CONTACTS_ENTER, "ui open home",
                   "mesh scope console %s YES" % ZERO, "mesh chat open #badgetest"):
            self.assertTrue(sp.line_allowed(ok, names), ok)
        for bad in ("mesh dm abcdef123456 hi", "mesh advert", "mesh send #general hi", "mesh send #badgetest hi",
                    "mesh scope console other YES", "keys send enter", "keys send needlock enter",
                    "keys send needlock focus:Chat| enter", "mesh chat open #general", "ui open power",
                    "mesh remote x status", "mesh trace", "contacts import x", "keys send needlock right enter"):
            self.assertFalse(sp.line_allowed(bad, names), bad)

    def test_driver_will_not_send_one(self):
        e = Env(self)
        e.start()
        with self.assertRaises(sd.Blocker):
            e.driver.line("mesh dm abcdef123456 hi")
        self.assertNotIn("mesh dm abcdef123456 hi", e.badge.log)

    def test_send_line_is_one_clean_line(self):
        line, ident = sp.send_line("abc123", 7)
        self.assertEqual(line, "mesh send #badgetest soak abc123 7")
        self.assertNotRegex(line, r"[\r\n;|&$`]")


class CommentTest(unittest.TestCase):
    def test_comment_goes_to_the_repos_slug_from_the_repo_dir(self):
        calls = []

        def fake_run(argv, **kw):
            calls.append((argv, kw))
            return mock.Mock(stdout="git@github.com:owner/name.git\n", returncode=0)

        with mock.patch.object(sd.subprocess, "run", fake_run):
            self.assertTrue(sd.post_issue_comment(407, "x"))
        gh = calls[-1]
        self.assertEqual(gh[0][:6], ["gh", "issue", "comment", "407", "-R", "owner/name"])
        self.assertTrue(os.path.isdir(gh[1]["cwd"]))

    def test_failures_are_reported(self):
        with mock.patch.object(sd.subprocess, "run", lambda *a, **k: mock.Mock(stdout="", returncode=1)):
            self.assertFalse(sd.post_issue_comment(407, "x"))
        with mock.patch.object(sd.subprocess, "run", mock.Mock(side_effect=FileNotFoundError)):
            self.assertFalse(sd.post_issue_comment(407, "x"))

    def test_blocker_file_notes_a_failed_comment(self):
        e = Env(self, hook=lambda b, line: setattr(b, "crash", 1) if line == "mesh scope" else None, notify=lambda t: False)
        self.assertEqual(e.driver.run(), 1)
        self.assertIn("could not be posted", e.read("BLOCKER"))


class BadgeShellTest(unittest.TestCase):
    def tree(self, script):
        d = tempfile.mkdtemp()
        self.addCleanup(lambda: __import__("shutil").rmtree(d, ignore_errors=True))
        os.makedirs(os.path.join(d, "tools"))
        path = os.path.join(d, "tools", "badge.sh")
        with open(path, "w") as f:
            f.write(script)
        os.chmod(path, 0o755)
        return d

    def ipfile(self, text):
        d = tempfile.mkdtemp()
        self.addCleanup(lambda: __import__("shutil").rmtree(d, ignore_errors=True))
        path = os.path.join(d, "ip")
        with open(path, "w") as f:
            f.write(text)
        return path

    def test_runs_badge_sh_fw_with_an_argument_list(self):
        repo = self.tree('#!/bin/sh\nprintf "%s|" "$@"\n')
        b = sd.BadgeShell(self.ipfile("192.0.2.9\n"), repo)
        rc, out = b.run("mesh send #badgetest soak a 1; echo pwned")
        self.assertEqual((rc, out), (0, "fw|192.0.2.9|mesh send #badgetest soak a 1; echo pwned|"))

    def child_script(self, pidfile):
        return '#!/bin/sh\nsleep 30 &\necho $! > "%s"\nwait\n' % pidfile

    def gone(self, pidfile):
        with open(pidfile) as f:
            pid = int(f.read())
        for _ in range(60):  # up to 3 s for the kernel to reap it
            try:
                os.kill(pid, 0)
            except ProcessLookupError:
                return True
            time.sleep(0.05)
        os.kill(pid, 9)
        return False

    def test_timeout_kills_the_children(self):
        pidfile = os.path.join(tempfile.mkdtemp(), "pid")
        b = sd.BadgeShell(self.ipfile("192.0.2.9"), self.tree(self.child_script(pidfile)))
        with mock.patch.object(sd, "CALL_TIMEOUT_S", 0.5):
            self.assertEqual(b.run("x"), (124, "timeout"))
        self.assertTrue(self.gone(pidfile))

    def test_interrupt_kills_the_call_under_way(self):
        pidfile = os.path.join(tempfile.mkdtemp(), "pid")
        b = sd.BadgeShell(self.ipfile("192.0.2.9"), self.tree(self.child_script(pidfile)))
        timer = threading.Timer(0.5, os.kill, (os.getpid(), signal.SIGINT))
        timer.start()
        self.addCleanup(timer.cancel)
        with self.assertRaises(KeyboardInterrupt):
            b.run("x")
        self.assertTrue(self.gone(pidfile))

    def test_sigterm_is_an_interrupt(self):
        old = signal.getsignal(signal.SIGTERM)
        self.addCleanup(lambda: signal.signal(signal.SIGTERM, old))
        sd.install_signal_handlers()
        with self.assertRaises(KeyboardInterrupt):
            signal.raise_signal(signal.SIGTERM)

    def test_the_address_file_must_hold_one_ipv4_address(self):
        for bad in ("badge.example.lan", "192.0.2.9 extra", "::1", "", "192.0.2.9\n192.0.2.10"):
            with self.assertRaises(ValueError):
                sd.BadgeShell(self.ipfile(bad), "/")


class TimelineSegmentTest(unittest.TestCase):
    def test_driver_transcript_split_by_block(self):
        import soak_segment as seg
        e = Env(self, profile=sp.REHEARSAL)
        self.assertEqual(e.driver.run(), 0)
        tr = e.read("transcript.txt").splitlines()
        m = seg.parse_markers(tr)
        epochs = [int(x.rsplit("epoch=", 1)[1]) for x in tr if x.startswith("## sample ")]
        rows = [{"epoch": t} for t in epochs]
        starts = {n: t for n, _, t in m.sessions}
        for k, block in enumerate(sp.BLOCKS):
            got = [r["epoch"] for r in seg.select(rows, m, block)]
            lo, hi = starts[k + 1], starts.get(k + 2, float("inf"))
            self.assertEqual(got, [t for t in epochs if lo < t <= hi] if k + 1 < 6 else [t for t in epochs if t > lo], block)
        self.assertEqual([r["epoch"] for r in seg.select(rows, m, "chat")], [T0 + 300, T0 + 600])

class ContactsGateTest(unittest.TestCase):
    def contacts(self, e):
        e.driver.session(sp.Session(1, "contacts", 0, "awake", ()))

    def test_no_enter_unless_the_contacts_tile_is_focused(self):
        for why, fix in (("right missed", lambda b: setattr(b, "focus", "Chat")),
                         ("row containing the word", lambda b: setattr(b, "focus", "Contacts on the card")),
                         ("wrong stack", lambda b: setattr(b, "stack", "0"))):
            with self.subTest(why):
                e = Env(self)
                e.start()
                orig = e.badge.run

                def run(line, orig=orig, fix=fix, e=e):
                    rc, out = orig(line)
                    if line == "keys send needlock right":
                        fix(e.badge)
                        if "wrong stack" in why:
                            e.badge.focus = "Contacts"
                    return rc, out

                e.badge.run = run
                self.contacts(e)
                self.assertNotIn(sp.CONTACTS_ENTER, e.badge.log)
                self.assertEqual(e.badge.log[-4:-2], ["ui open home", "ui lock off"])  # the tail still went out
                self.assertIn("no Enter sent", e.read("transcript.txt") + e.read("state.txt"))

    def test_the_enter_goes_when_the_grid_is_right(self):
        e = Env(self)
        e.start()
        self.contacts(e)
        self.assertEqual(e.badge.log.count(sp.CONTACTS_ENTER), 1)
        self.assertTrue(e.driver.contacts_opened)

    def test_three_blocks_in_a_row_that_could_not_open_are_a_blocker(self):
        e = Env(self)
        e.start()
        e.badge.right_stuck = True
        self.contacts(e)
        self.contacts(e)
        with self.assertRaises(sd.Blocker):
            self.contacts(e)
        self.assertEqual(e.badge.log[-4:-2], ["ui open home", "ui lock off"])

    def test_a_good_block_resets_the_count(self):
        e = Env(self)
        e.start()
        e.badge.right_stuck = True
        self.contacts(e)
        self.contacts(e)
        e.badge.right_stuck = False
        self.contacts(e)
        e.badge.right_stuck = True
        self.contacts(e)
        self.assertEqual(e.driver.contacts_bad, 1)


class LedgerTest(unittest.TestCase):
    def test_pending_row_is_written_before_the_line_and_counts_on_reload(self):
        seen = []

        def hook(b, line):
            if line.startswith("mesh send "):
                with open(env.driver.ledger_path()) as f:
                    seen.append(f.read())
                b.boot = 99  # the badge restarts right after the send

        env = Env(self, hook=hook)
        self.assertEqual(env.driver.run(), 1)
        self.assertIn("pending", seen[0])
        self.assertEqual(env.ledger()[0]["result"], "pending")  # the outcome never came: the row stays pending
        again = sd.Driver(sp.FULL, sd.Config(ZERO, REGION), FakeBadge(env.clock), env.clock, env.dir)
        again.load_ledger()
        self.assertEqual(len(again.attempts), 1)
        self.assertEqual(again.attempts[0][1], sp.REGIONAL)

    def test_the_row_is_updated_in_place_after_the_send(self):
        e = Env(self)
        e.start()
        e.driver.send(sp.ZERO_HOP)
        rows = e.ledger()
        self.assertEqual((len(rows), rows[0]["result"], rows[0]["tx_after"]), (1, "sent", "1"))

    def test_corrupt_rows_are_a_blocker_not_a_crash(self):
        head = "seq,time,scope,id,tx_before,tx_after,result\n"
        for why, body in (("cut result", "1,%d,zero_hop,x,0,0,sen\n" % T0), ("garbage seq", "zz,%d,zero_hop,x,0,0,sent\n" % T0),
                          ("short row", "2,%d\n" % T0), ("unknown scope", "1,%d,mars,x,0,0,sent\n" % T0)):
            with self.subTest(why):
                e = Env(self)
                os.makedirs(e.dir, exist_ok=True)
                with open(e.driver.ledger_path(), "w") as f:
                    f.write(head + body)
                self.assertEqual(e.driver.run(), 1)
                self.assertIn("ledger", e.read("BLOCKER"))
                self.assertFalse(any(x.startswith(("mesh send", "keys", "ui")) for x in e.badge.log))

    def test_ledgers_are_per_profile_and_the_other_one_is_refused(self):
        e = Env(self, profile=sp.REHEARSAL)
        e.seed_ledger([(1, 10, "zero_hop", "sent")])
        full = sd.Driver(sp.FULL, sd.Config(ZERO, REGION), e.badge, e.clock, e.dir)
        with contextlib.redirect_stderr(io.StringIO()) as err:
            self.assertEqual(full.run(), 2)
        self.assertIn("another profile", err.getvalue())
        self.assertEqual(e.badge.log, [])
        self.assertNotEqual(e.driver.ledger_path(), full.ledger_path())


class RestoreFailureTest(unittest.TestCase):
    def test_failed_restore_line_leaves_the_next_send_to_fix_or_skip(self):
        fails = []

        def hook(b, line):
            if line == "mesh scope console %s YES" % ZERO and len(fails) < 1 and any(
                    x.startswith("mesh send ") for x in b.log):
                fails.append(1)
                b.ignore_scope_set = True  # the restore line does not take

        e = Env(self, hook=hook)
        e.start()
        e.driver.send(sp.REGIONAL)
        self.assertTrue(e.driver.scope_dirty)
        self.assertEqual(e.badge.scope, REGION)
        e.badge.ignore_scope_set = False
        e.driver.send(sp.ZERO_HOP)  # the next 0-hop send re-sets the scope first
        self.assertEqual(e.badge.scope, ZERO)
        self.assertFalse(e.driver.scope_dirty)
        self.assertEqual(sum(1 for x in e.badge.log if x.startswith("mesh send ")), 2)

    def test_a_scope_that_will_not_go_back_means_no_send(self):
        e = Env(self)
        e.start()
        e.driver.send(sp.REGIONAL)
        e.badge.scope, e.badge.ignore_scope_set = REGION, True
        e.driver.scope_dirty = True
        e.driver.send(sp.ZERO_HOP)
        self.assertEqual(sum(1 for x in e.badge.log if x.startswith("mesh send ")), 1)  # only the regional one

    def test_restore_failing_does_not_replace_the_original_exception(self):
        e = Env(self, hook=lambda b, line: (_ for _ in ()).throw(KeyboardInterrupt()) if line.startswith("mesh send ") else None)
        e.start()
        e.driver.restore_scope = mock.Mock(side_effect=sd.Blocker("x"))
        with self.assertRaises(KeyboardInterrupt):
            e.driver.send(sp.REGIONAL)

    def test_blocker_text_says_the_scope_may_be_regional(self):
        e = Env(self, hook=lambda b, line: setattr(b, "boot", 99) if line.startswith("mesh send ") else None)
        self.assertEqual(e.driver.run(), 1)
        self.assertIn("may still be the regional one", e.read("BLOCKER"))


class AllowlistLinesTest(unittest.TestCase):
    def test_control_characters_and_odd_spacing_are_refused(self):
        names = (ZERO, REGION)
        for bad in ("keys send needlock up\ndown", "mesh send #badgetest soak ab 3\n", "keys send  needlock up",
                    "mesh send #badgetest soak ab 3 ", "ui open\thome", "ui lock on 601", "ui lock on 0", "ui lock on 99999",
                    "keys send needlock left", "keys send needlock triangle", "mesh send #badgetest soak ab 3\x00"):
            self.assertFalse(sp.line_allowed(bad, names), repr(bad))
        self.assertTrue(sp.line_allowed("ui lock on 600", names))


if __name__ == "__main__":
    unittest.main()
