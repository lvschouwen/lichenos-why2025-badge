"""Host tests for tools/v3_bench_report.py against a log fixture."""
import os
import subprocess
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.normpath(os.path.join(HERE, "..", "..", "tools", "v3_bench_report.py"))
sys.path.insert(0, os.path.dirname(TOOL))
import v3_bench_report as rep  # noqa: E402

FIXTURE = os.path.join(HERE, "fixtures", "run-ok.log")


def load() -> str:
    with open(FIXTURE, encoding="utf-8") as f:
        return f.read()


class ParseTests(unittest.TestCase):
    def test_fixture_is_clean(self):
        r = rep.parse(load())
        self.assertEqual((r.build_id, r.platform, r.end), ("151b0540", "esp32p4", "ok"))
        self.assertEqual(len(r.checks), 8)
        self.assertEqual(rep.verdict(r, rep.DEFAULT_REQUIRED), [])

    def test_first_end_wins_over_later_noise(self):
        self.assertEqual(rep.parse(load()).end, "ok")

    def test_crlf_and_ansi_are_tolerated(self):
        text = load().replace("\n", "\r\n").replace("CHECK c6.en", "\x1b[0;32mCHECK c6.en")
        r = rep.parse(text)
        self.assertIn("c6.en", [c[0] for c in r.checks])

    def test_missing_end_is_a_failure_naming_the_last_begin(self):
        text = load().split("CHECK l2.cache")[0]
        problems = rep.verdict(rep.parse(text), rep.DEFAULT_REQUIRED)
        self.assertTrue(any("no END" in p and "l2" in p for p in problems), problems)

    def test_a_fail_line_fails_the_run(self):
        text = load().replace("CHECK c6.en PASS", "CHECK c6.en FAIL")
        problems = rep.verdict(rep.parse(text), rep.DEFAULT_REQUIRED)
        self.assertTrue(any(p.startswith("FAIL c6.en") for p in problems))

    def test_missing_required_id(self):
        text = load().replace("CHECK l2.fit", "CHECK l2.other")
        self.assertIn("missing l2.fit", rep.verdict(rep.parse(text), rep.DEFAULT_REQUIRED))

    def test_end_fatal_and_no_banner(self):
        problems = rep.verdict(rep.parse("FATAL trap mcause=0x2\nEND fatal\n"), [])
        self.assertEqual(problems, ["no banner", "END fatal"])


class CliTests(unittest.TestCase):
    def run_cli(self, *args):
        return subprocess.run([sys.executable, "-I", TOOL, *args], capture_output=True, text=True)

    def test_exit_codes(self):
        ok = self.run_cli(FIXTURE)
        self.assertEqual(ok.returncode, 0, ok.stdout)
        self.assertIn("RESULT: PASS", ok.stdout)
        self.assertEqual(self.run_cli(FIXTURE, "--require", "nope").returncode, 1)
        self.assertEqual(self.run_cli("/nonexistent/log").returncode, 2)
        self.assertEqual(self.run_cli().returncode, 2)


if __name__ == "__main__":
    unittest.main()
