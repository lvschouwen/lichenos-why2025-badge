#!/usr/bin/env python3
"""Host test of the soak log census and the trend test (#430): a faked leak, a faked reboot, a gap in the samples, a
line kind that appears late, a timed line that grows, a counter from the allow file that rises (not flagged), and a
fixture holding a name and addresses that must not reach the output. All values are invented (documentation address
ranges, made-up names). Run with python3 -I."""
import contextlib
import io
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
import soak_census as sc  # noqa: E402
import soak_report as sr  # noqa: E402
import soak_trend as st  # noqa: E402

H = 3600


def stamp(t):
    return "%02d:%02d:%02d" % ((t // 3600) % 24, (t // 60) % 60, t % 60)


def log_lines(hours, every, extra=None, start=10 * H):
    """A boot-relative log, one `tick` line every `every` s for `hours`; extra(t_rel) -> more lines at that time."""
    out = []
    for t in range(0, int(hours * H), every):
        ms = (t + 5) * 1000
        out.append("%s I (%d) app: tick ok" % (stamp(start + t), ms))
        for text in (extra(t) if extra else []):
            out.append("%s %s" % (stamp(start + t), text.replace("@MS@", str(ms))))
    return out


def census(lines, warmup_h=2, window_h=2):
    return sc.read_census(iter(lines), warmup_h * H, window_h * H, 1800)


def report(lines, **kw):
    return "\n".join(sc.census_report(census(lines, **kw)))


class SplitTest(unittest.TestCase):
    def test_stamps(self):
        self.assertEqual(sc.split_line("12:30:05 W (4567) mesh: slow loop 812 ms"), (45005, "W", 4567, "mesh", "slow loop 812 ms"))
        self.assertEqual(sc.split_line("\x1b[0;32mI (12) a: b\x1b[0m\r\n"), (None, "I", 12, "a", "b"))
        self.assertEqual(sc.split_line("12:00:01 >> fw soak")[1:], ("-", None, "", ">> fw soak"))

    def test_midnight_wraps_and_small_step_back_is_held(self):
        c = sc.Clock()
        t1 = c.feed(1, 23 * H + 59 * 60, None)
        t2 = c.feed(2, 120, None)
        self.assertEqual(t2 - t1, 180)
        self.assertEqual(c.feed(3, 100, None), t2)       # the clock stepped back 20 s: held, not a new day

    def test_ms_stamp_without_wall_clock_chains_boots(self):
        c = sc.Clock()
        c.feed(1, None, 1000)
        t = c.feed(2, None, 9000)
        self.assertEqual(t, 9.0)
        c.feed(3, None, 500)                             # the stamp dropped: a reboot
        self.assertEqual(c.segments, [(3, 9.0)])
        self.assertEqual(c.feed(4, None, 2500), 11.5)


class CensusTest(unittest.TestCase):
    def test_late_kind(self):
        lines = log_lines(8, 60, lambda t: ["W (@MS@) fs: new thing happened"] if t == 5 * H else [])
        out = report(lines)
        self.assertIn("kinds first seen after the warm-up: 1", out)
        self.assertIn("W fs: new thing happened  (first at 5.0 h, 1 lines)", out)
        self.assertNotIn("app: tick", out.split("first seen after the warm-up")[1].split("kinds whose")[0])

    def test_kind_in_warmup_is_not_late(self):
        lines = log_lines(8, 60, lambda t: ["W (@MS@) fs: seen early"] if t in (60, 5 * H) else [])
        self.assertIn("kinds first seen after the warm-up: 0", report(lines))

    def test_growing_timed_line(self):
        lines = log_lines(12, 60, lambda t: ["W (@MS@) fs: count took %d ms" % (200 + t // 60)] if t % 600 == 0 else [])
        out = report(lines)
        self.assertIn("numbers that rise", out)
        self.assertRegex(out, r"W fs: count took # ms  number 1 median \d+ -> \d+")
        self.assertRegex(out, r"number 1 max \d+ -> \d+")

    def test_steady_timed_line_is_quiet(self):
        lines = log_lines(12, 60, lambda t: ["W (@MS@) fs: count took %d ms" % (200 + t % 7)] if t % 600 == 0 else [])
        self.assertIn("numbers that rise (median or max, first window after warm-up -> last): 0", report(lines))

    def test_negative_numbers_do_not_count_as_a_rise(self):
        lines = log_lines(12, 60, lambda t: ["W (@MS@) rf: level %d dBm" % (-80 + t // 3600 % 3)] if t % 600 == 0 else [])
        self.assertIn("numbers that rise (median or max, first window after warm-up -> last): 0", report(lines))

    def test_rising_rate(self):
        def extra(t):
            return ["W (@MS@) wifi: retry"] * (1 + t // (3 * H)) if t % 600 == 0 else []
        lines = log_lines(12, 60, extra)
        out = report(lines)
        self.assertIn("kinds whose rate rises: 1", out)
        self.assertIn("W wifi: retry", out)

    def test_flat_rate_is_quiet(self):
        lines = log_lines(12, 60, lambda t: ["W (@MS@) wifi: retry"] if t % 600 == 0 else [])
        self.assertIn("kinds whose rate rises: 0", report(lines))

    def test_reboot_is_reported_as_segment(self):
        a = ["%s I (%d) app: tick" % (stamp(10 * H + i * 60), (i + 1) * 60000) for i in range(100)]
        b = ["%s I (%d) app: tick" % (stamp(10 * H + (100 + i) * 60), (i + 1) * 1000) for i in range(100)]
        out = report(a + b)
        self.assertIn("boot restarts (the boot-relative stamp dropped): 1; line 101", out)

    def test_log_gap_is_reported(self):
        lines = log_lines(1, 60) + log_lines(1, 60, start=10 * H + 3 * H)
        self.assertIn("log gaps over 30 min: 1", report(lines))

    def test_short_log_makes_no_trend_claim(self):
        self.assertIn("shorter than the warm-up", report(log_lines(1, 60)))

    def test_reads_a_stream_not_a_list(self):
        def gen():
            yield from log_lines(6, 30)
        c = sc.read_census(gen(), 2 * H, 2 * H, 1800)
        self.assertEqual(c.lines, 6 * H // 30)
        self.assertEqual(len(c.kinds), 1)

    def test_kind_count_is_bounded(self):
        c = census(["I (%d) a: word%s here" % (i, "x" * (i % 50)) for i in range(5)])
        self.assertLessEqual(len(c.kinds), sc.MAX_KINDS)


class ScrubTest(unittest.TestCase):
    SECRETS = ["Wren", "192.0.2.77", "2001:db8::1f", "aa:bb:cc:dd:ee:01", "deadbeefcafe0123", "Lichenville", "hello mesh",
               "/sdcard/wren/notes.txt", "0x3ffb1234", "tok3nAbCdEfGhIjKlMnOpQrStUvWxYz"]
    LINES = [
        'I (100) mesh: rx from Wren: hello mesh at -61 dBm',
        'W (101) wifi: connected to "Lichenville" ip 192.0.2.77 gw 192.0.2.1',
        'I (102) wifi: ssid=Lichenville bssid aa:bb:cc:dd:ee:01 v6 2001:db8::1f',
        'I (103) mesh: contact name=Wren key deadbeefcafe0123 sent',
        'I (104) fs: open /sdcard/wren/notes.txt failed 0x3ffb1234 err 5',
        'I (105) net: token tok3nAbCdEfGhIjKlMnOpQrStUvWxYz accepted',
        'I (106) mesh: channel Lichenville joined, 3 members',
        "console echo for Wren 192.0.2.77 and more words",
    ]

    def test_nothing_private_reaches_the_report(self):
        c = census(self.LINES * 3, warmup_h=0)
        out = "\n".join(sc.census_report(c)) + "\n".join(c.kinds)
        for secret in self.SECRETS:
            self.assertNotIn(secret, out)
        self.assertNotIn("Wren", out)

    def test_names_never_reach_a_late_kind_line(self):
        lines = log_lines(7, 60, lambda t: ['W (@MS@) mesh: ping to node name=Wren at 192.0.2.77'] if t == 4 * H else [])
        out = report(lines)
        self.assertIn("kinds first seen after the warm-up: 1", out)
        for secret in ("Wren", "192.0.2.77"):
            self.assertNotIn(secret, out)

    def test_kinds_hold_no_digits(self):
        c = census(self.LINES, warmup_h=0)
        for name in c.kinds:
            self.assertNotRegex(name, r"\d", name)

    def test_numbers_are_taken_out_and_kept(self):
        s = sc.Scrubber()
        self.assertEqual(s.text("took 812 ms, 3 dropped, -61 dBm, 1.5 s"), ("took # ms, # dropped, # dBm, # s", [812, 3, -61, 1.5]))
        self.assertEqual(s.text("took 9 ms, 4 dropped, -7 dBm, 2.25 s")[1], [9, 4, -7, 2.25])   # cached shape

    def test_console_text_keeps_only_a_command_shaped_first_word(self):
        c = census(["12:00:01 >> mesh send Wren hello", "12:00:02 soak1 v=1 up=5", "12:00:03 Wren says hi"], warmup_h=0)
        self.assertEqual(sorted(c.kinds), ["-: <text>", "-: >>", "-: soak#"])

    def test_unsafe_kind_is_withheld(self):
        self.assertEqual(sc.safe_kind("W a: x7"), "<withheld kind>")
        self.assertEqual(sc.safe_kind("W a: x #"), "W a: x #")

    def test_hex_id_with_long_digit_runs(self):
        s = sc.Scrubber()
        self.assertEqual(s.text("id 0000ce1a x")[0], "id <hex> x")
        self.assertEqual(s.text("id 3fcb2a10 x")[0], "id <hex> x")
        self.assertEqual(s.text("id 000000cf x")[0], "id <hex> x")
        self.assertEqual(s.text("on c6 and esp32 now")[0], "on c# and esp# now")

    def test_decimal_and_ip_differ(self):
        s = sc.Scrubber()
        self.assertEqual(s.text("a 1.5 b")[0], "a # b")
        self.assertEqual(s.text("a 1.5.6.7 b")[0], "a <addr> b")


def csv_samples(n, **cols):
    """n samples ten minutes apart: up counts, every other column from a function of the index."""
    out = []
    for i in range(n):
        s = {"v": 1, "boot": 7, "up": 600 * (i + 1)}
        s.update({k: f(i) for k, f in cols.items()})
        out.append(s)
    return out


def trend(samples, allow=None, warmup_h=2.0):
    return "\n".join(st.trend_report(samples, allow if allow is not None else {"up": ""}, warmup_h))


class TrendTest(unittest.TestCase):
    def test_leak_is_reported_with_quarter_medians(self):
        out = trend(csv_samples(220, int=lambda i: 180000 - 120 * i + (i * 37 % 11), steady=lambda i: 5000 + i % 3))
        self.assertIn("moving (not on the allow list): 1", out)
        self.assertRegex(out, r"int +falls +first quarter median 1[0-9]+ -> last 1[0-9]+")
        self.assertNotIn("steady ", out)

    def test_noise_alone_is_quiet(self):
        out = trend(csv_samples(220, jitter=lambda i: 1000 + (i * 7919 % 13) - 6))
        self.assertIn("moving (not on the allow list): 0", out)

    def test_allowed_counter_that_rises_is_not_flagged(self):
        out = trend(csv_samples(220, mrx=lambda i: 3 * i), allow={"up": "", "mrx": "packets"})
        self.assertIn("moving (not on the allow list): 0", out)
        self.assertIn("rising as allowed: 2 (mrx, up)", out)

    def test_same_counter_unlisted_is_flagged(self):
        out = trend(csv_samples(220, mrx=lambda i: 3 * i))
        self.assertRegex(out, r"mrx +rises")

    def test_allowed_counter_that_falls_is_flagged(self):
        out = trend(csv_samples(220, mrx=lambda i: 900 - 3 * i), allow={"up": "", "mrx": "packets"})
        self.assertRegex(out, r"mrx +falls")

    def test_reboot_is_reported_and_not_fitted_through(self):
        a = csv_samples(150, ctr=lambda i: 50 * i)
        b = csv_samples(40, ctr=lambda i: 50 * i)          # the counter restarted with the boot
        out = trend(a + b, allow={"up": ""})
        self.assertIn("reboot before sample 150", out)
        self.assertIn("boot segments: 2; fitted: samples 0-149", out)
        self.assertRegex(out, r"ctr +rises +first quarter median \d+ -> last \d+")
        self.assertNotIn("last 0", out)

    def test_gap_is_reported(self):
        rows = csv_samples(100, f=lambda i: 1)
        for s in rows[60:]:
            s["up"] += 4 * H
        out = trend(rows)
        self.assertRegex(out, r"gap of 250\.\d min before sample 60")

    def test_clock_column_gives_the_time(self):
        rows = csv_samples(100, f=lambda i: 1)
        for i, s in enumerate(rows):
            s["t"] = 1000 + i * 300 + (3 * H if i >= 50 else 0)
        out = trend(rows)
        self.assertIn("time from: t", out)
        self.assertRegex(out, r"gap of 185\.\d min before sample 50")

    def test_short_run_claims_nothing(self):
        out = trend(csv_samples(10, a=lambda i: i * 100))
        self.assertIn("no trend claimed", out)

    def test_incomplete_samples_are_left_out(self):
        rows = csv_samples(220, int=lambda i: 1000 - 4 * i)
        rows[5]["incomplete"] = 1
        self.assertIn("1 incomplete", trend(rows))

    def test_allow_file(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "a.txt")
            with open(p, "w") as f:
                f.write("# note\nup uptime\nmrx  packets #5 # trailing\n\n")
            self.assertEqual(st.load_allow(p), {"up": "uptime", "mrx": "packets #5"})

    def test_shipped_allow_file_loads(self):
        allow = st.load_allow(st.DEFAULT_ALLOW)
        self.assertIn("up", allow)
        self.assertNotIn("dma", allow)       # a memory figure must never be allowed to fall or rise unseen


class CliTest(unittest.TestCase):
    def run_cli(self, *args):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = sr.main(["soak_report.py", *args])
        return code, out.getvalue(), err.getvalue()

    def test_census_and_trend_exit_zero(self):
        with tempfile.TemporaryDirectory() as d:
            log, csv = os.path.join(d, "s.log"), os.path.join(d, "s.csv")
            with open(log, "w") as f:
                f.write("\n".join(log_lines(6, 60, lambda t: ["W (@MS@) fs: new"] if t == 4 * H else [])) + "\n")
            import soak_parse
            soak_parse.write_csv(csv, csv_samples(120, leak=lambda i: 5000 - 20 * i))
            code, out, _ = self.run_cli("census", log)
            self.assertEqual(code, 0)
            self.assertIn("W fs: new", out)
            code, out, _ = self.run_cli("trend", csv)
            self.assertEqual(code, 0)
            self.assertRegex(out, r"leak +falls")

    def test_unreadable_input_exits_two(self):
        self.assertEqual(self.run_cli("census", "/nonexistent/x.log")[0], 2)
        self.assertEqual(self.run_cli("trend", "/nonexistent/x.csv")[0], 2)

    def test_bad_option_exits_two(self):
        self.assertEqual(self.run_cli("census", "/nonexistent/x.log", "--bogus", "1")[0], 2)
        self.assertEqual(self.run_cli("trend", "x.csv", "--warmup-h")[0], 2)


if __name__ == "__main__":
    unittest.main()
