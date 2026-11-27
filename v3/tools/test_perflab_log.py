"""The #648 perf lab log (tools/fixtures/perflab-smoke.log, a trimmed real smoke run): both report tools must read it."""
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.normpath(os.path.join(HERE, "..", "..", "tools")))
import perf_report as pr  # noqa: E402
import v3_bench_report as rep  # noqa: E402

FIXTURE = os.path.join(HERE, "fixtures", "perflab-smoke.log")


def text() -> str:
    with open(FIXTURE, encoding="utf-8") as f:
        return f.read()


class PerfLines(unittest.TestCase):
    def test_every_perf_line_is_in_the_643_grammar(self):
        metrics, warnings = pr.parse_log(text())
        self.assertEqual(warnings, [])
        self.assertEqual(metrics["lab.mem.psram.seq_read"], {"mb_s": 111.30})
        self.assertEqual(metrics["lab.config"]["l2_line_b"], 64.0)
        self.assertEqual(metrics["lab.dma.sram_to_sram"]["setup_cycles"], 992.0)

    def test_the_kernel_and_core_metrics_parse(self):
        metrics, _ = pr.parse_log(text())
        self.assertEqual(metrics["lab.irq.idle"]["jitter_max_ns"], 133.0)
        self.assertEqual(metrics["lab.timer.accuracy"]["mtime_khz"], 360000.0)  # the CLINT counter runs at the CPU clock
        self.assertEqual(metrics["lab.dual.read_read"]["c1_passes"], 2.0)
        self.assertAlmostEqual(metrics["lab.cpu.fetch.psram512k"]["cycles_per_instr"], 12.37)

    def test_a_variant_is_in_the_name_so_a_later_line_cannot_replace_it(self):
        metrics, _ = pr.parse_log(text())
        names = [m for m in metrics if m.startswith("lab.integrity.")]
        self.assertEqual(len(names), 3)  # cached, dma_write, dma_read of the same pattern keep separate metrics
        self.assertEqual(metrics["lab.integrity.cached.addr"]["bad"], 0.0)

    def test_lab_metrics_do_not_disturb_the_ratchet(self):
        metrics, _ = pr.parse_log(text())
        targets = pr.load_toml(os.path.join(HERE, "..", "perf", "targets.toml"))
        self.assertEqual(set(metrics) & set(targets), set())  # lab metrics are not v3 targets (nothing is ratcheted by them)
        rows, regress = pr.evaluate(targets, {}, metrics, "t")
        self.assertEqual(regress, [])


class CheckLines(unittest.TestCase):
    def test_the_bench_report_reads_banner_checks_and_end(self):
        r = rep.parse(text())
        self.assertEqual((r.build_id, r.platform, r.end), ("02761299+-cpu360-ps120-l2k256l64-dma32-ldocal", "esp32p4", "ok"))
        ids = {c[0] for c in r.checks}
        self.assertTrue({"648.config", "648.psram", "648.integrity", "648.park", "648.run"} <= ids)
        required = ["648.cpu", "648.l2", "648.psram", "648.map", "648.config", "648.integrity", "648.park", "648.cpu.crc"]
        self.assertEqual(rep.verdict(r, required), [])

    def test_a_missing_end_or_a_fail_is_a_failed_run(self):
        no_end = "\n".join(text().splitlines()[:-1])
        self.assertNotEqual(rep.verdict(rep.parse(no_end), ["648.config"]), [])
        failed = text().replace("CHECK 648.park PASS", "CHECK 648.park FAIL").replace("END ok", "END fail")
        self.assertNotEqual(rep.verdict(rep.parse(failed), ["648.park"]), [])


if __name__ == "__main__":
    unittest.main()
