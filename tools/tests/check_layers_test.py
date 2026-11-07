#!/usr/bin/env python3
"""Host test of the layer checker (#218): config, rules A/B/C on fixture components, every ratchet state.
Rule C uses the host gcc/ar/nm on tiny fixture archives. Run with python3 -I."""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import check_layers as cl  # noqa: E402

CONF = """\
layer L0 platform
layer L1 drivers
layer L2 gate
layer L3 apps
component base L0
component drv L1
component gate L2
component app L3
layer-may L3 L2 L3
closed L3 freertos
headers L3 stdint.h string.h
forbid L3 esp_restart
owner-only nvs_open gate
owner-only xTaskCreate <=L1
"""

BASE_FILES = {
    "base": ("idf_component_register(SRCS \"b.c\")", {"b.c": "int base_fn(void){return 1;}\n", "base.h": ""}),
    "drv": ("idf_component_register(SRCS \"d.c\" REQUIRES base)", {"d.c": "int drv_fn(void){return 2;}\n", "drv.h": ""}),
    "gate": ("idf_component_register(SRCS \"g.c\" REQUIRES base)", {"g.c": "int gate_fn(void){return 3;}\n", "gate.h": ""}),
    "app": ("idf_component_register(SRCS \"a.c\" REQUIRES gate freertos)", {"a.c": "#include <stdint.h>\n#include \"gate.h\"\nint app_fn(void){return 4;}\n"}),
}


class Fixture:
    def __init__(self, tc, conf=CONF, overrides=None):
        self.root = tempfile.mkdtemp()
        tc.addCleanup(shutil.rmtree, self.root)
        os.makedirs(os.path.join(self.root, "tools"))
        self.write("tools/layers.conf", conf)
        for name, (cmake, files) in {**BASE_FILES, **(overrides or {})}.items():
            self.write("firmware/components/%s/CMakeLists.txt" % name, cmake)
            for fn, text in files.items():
                self.write("firmware/components/%s/%s" % (name, fn), text)
        self.build()

    def write(self, rel, text):
        path = os.path.join(self.root, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as f:
            f.write(text)

    def build(self):
        comps = os.path.join(self.root, "firmware", "components")
        for name in os.listdir(comps):
            out = os.path.join(self.root, "firmware", "build", "esp-idf", name)
            os.makedirs(out, exist_ok=True)
            objs = []
            for fn in os.listdir(os.path.join(comps, name)):
                if fn.endswith(".c"):
                    o = os.path.join(out, fn + ".obj")
                    subprocess.run(["gcc", "-c", "-I", comps + "/gate", "-I", comps + "/base", "-o", o, os.path.join(comps, name, fn)], check=True, capture_output=True)
                    objs.append(o)
            lib = os.path.join(out, "lib%s.a" % name)
            if os.path.exists(lib):
                os.remove(lib)
            subprocess.run(["ar", "rcs", lib] + objs, check=True)

    def run(self, *args):
        r = subprocess.run([sys.executable, "-I", os.path.join(os.path.dirname(cl.__file__), "check_layers.py"),
                            "--root", self.root, "--nm", "nm"] + list(args), capture_output=True, text=True)
        return r.returncode, r.stdout + r.stderr


def keys(fx):
    conf = cl.parse_conf(cl.read(os.path.join(fx.root, "tools/layers.conf")))
    found = cl.measure(conf, fx.root, os.path.join(fx.root, "firmware/build"), "nm", True)
    return set(found)


class Rules(unittest.TestCase):
    def test_clean_tree_has_no_violations(self):
        self.assertEqual(keys(Fixture(self)), set())

    def test_rule_a_upward_requires(self):
        fx = Fixture(self, overrides={"drv": ("idf_component_register(SRCS \"d.c\" PRIV_REQUIRES gate)", {"d.c": "int drv_fn(void){return 2;}\n"})})
        self.assertEqual(keys(fx), {("A", "drv -> gate")})

    def test_rule_a_expands_set_variables(self):
        cm = "set(DEPS gate)\nidf_component_register(SRCS \"d.c\" REQUIRES base ${DEPS})"
        fx = Fixture(self, overrides={"drv": (cm, {"d.c": "int drv_fn(void){return 2;}\n"})})
        self.assertEqual(keys(fx), {("A", "drv -> gate")})

    def test_rule_a_closed_layer_external_library(self):
        fx = Fixture(self, overrides={"app": ("idf_component_register(SRCS \"a.c\" REQUIRES gate esp_wifi)", {"a.c": "int app_fn(void){return 4;}\n"})})
        self.assertEqual(keys(fx), {("A", "app -> esp_wifi")})

    def test_rule_b_upward_include(self):
        fx = Fixture(self, overrides={"drv": ("idf_component_register(SRCS \"d.c\" REQUIRES base)", {"d.c": "#include \"gate.h\"\nint drv_fn(void){return 2;}\n"})})
        self.assertEqual(keys(fx), {("B", "drv -> gate")})

    def test_rule_b_closed_layer_system_header_and_own_header(self):
        src = "#include <stdint.h>\n#include <stdio.h>\n#include \"mine.h\"\nint app_fn(void){return 4;}\n"
        fx = Fixture(self, overrides={"app": (BASE_FILES["app"][0], {"a.c": src, "mine.h": ""})})
        self.assertEqual(keys(fx), {("B", "app -> stdio.h")})

    def test_rule_c_link_time_upward_edge_without_requires(self):
        fx = Fixture(self, overrides={"drv": ("idf_component_register(SRCS \"d.c\" REQUIRES base)", {"d.c": "int gate_fn(void);\nint drv_fn(void){return gate_fn();}\n"})})
        self.assertEqual(keys(fx), {("C", "drv:gate_fn")})

    def test_rule_c_owner_only_symbol(self):
        src = "int nvs_open(void);\nint app_fn(void){return nvs_open();}\n"
        gate = "int nvs_open(void);\nint gate_fn(void){return nvs_open();}\n"
        fx = Fixture(self, overrides={"app": (BASE_FILES["app"][0], {"a.c": src}), "gate": (BASE_FILES["gate"][0], {"g.c": gate})})
        self.assertEqual(keys(fx), {("C", "app:nvs_open")})

    def test_rule_c_owner_only_by_layer_spec(self):
        src = "int xTaskCreate(void);\nint %s(void){return xTaskCreate();}\n"
        fx = Fixture(self, overrides={"drv": (BASE_FILES["drv"][0], {"d.c": src % "drv_fn"}),
                                      "gate": (BASE_FILES["gate"][0], {"g.c": src % "gate_fn", "gate.h": ""})})
        self.assertEqual(keys(fx), {("C", "gate:xTaskCreate")})  # <=L1 owns it: drv yes, gate no

    def test_rule_c_forbidden_symbol(self):
        src = "void esp_restart(void);\nint app_fn(void){esp_restart();return 4;}\n"
        fx = Fixture(self, overrides={"app": (BASE_FILES["app"][0], {"a.c": src})})
        self.assertEqual(keys(fx), {("C", "app:esp_restart")})

    def test_rule_c_missing_archive_fails_instead_of_skipping(self):
        fx = Fixture(self)
        os.remove(os.path.join(fx.root, "firmware/build/esp-idf/gate/libgate.a"))
        code, out = fx.run("--report")
        self.assertEqual(code, 1)
        self.assertIn("libgate.a", out)
        self.assertEqual(fx.run("--report", "--no-symbols")[0], 0)

    def test_unlisted_component_fails(self):
        fx = Fixture(self, overrides={"extra": ("idf_component_register(SRCS \"x.c\")", {"x.c": ""})})
        code, out = fx.run("--no-symbols", "--report")
        self.assertEqual(code, 1)
        self.assertIn("without a layer", out)

    def test_bad_conf_directive(self):
        with self.assertRaises(cl.LayerError):
            cl.parse_conf("layer L0 x\nbogus L0\n")
        with self.assertRaises(cl.LayerError):
            cl.parse_conf("layer L0 x\ncomponent c L9\n")

    def test_only_layer_filters(self):
        fx = Fixture(self, overrides={"drv": ("idf_component_register(SRCS \"d.c\" PRIV_REQUIRES gate)", {"d.c": ""})})
        code, out = fx.run("--only-layer", "L3", "--report")
        self.assertEqual(code, 0)
        self.assertIn("0 violation", out)


class Ratchet(unittest.TestCase):
    def broken(self):
        return Fixture(self, overrides={"drv": ("idf_component_register(SRCS \"d.c\" PRIV_REQUIRES gate)", {"d.c": "int drv_fn(void){return 2;}\n"})})

    def test_init_then_ok_and_init_refuses_overwrite(self):
        fx = self.broken()
        self.assertEqual(fx.run("--init")[0], 0)
        code, out = fx.run()
        self.assertEqual(code, 0, out)
        self.assertEqual(fx.run("--init")[0], 1)

    def test_new_violation_fails_with_place(self):
        fx = self.broken()
        fx.write("tools/layers-baseline.txt", "# empty\n")
        code, out = fx.run()
        self.assertEqual(code, 1)
        self.assertIn("NEW   A drv -> gate", out)
        self.assertIn("firmware/components/drv/CMakeLists.txt:1", out)

    def test_worse_fails(self):
        fx = Fixture(self, overrides={"drv": ("idf_component_register(SRCS \"d.c\" REQUIRES base)",
                                              {"d.c": "int g1(void);\nint drv_fn(void){return g1();}\n", "e.c": "int g1(void);\nint e(void){return g1();}\n"}),
                                      "gate": (BASE_FILES["gate"][0], {"g.c": "int g1(void){return 1;}\n", "gate.h": ""})})
        fx.write("tools/layers-baseline.txt", "C\tdrv:g1\t1\n")
        code, out = fx.run()
        self.assertEqual(code, 1)
        self.assertIn("WORSE C drv:g1 = 2 (baseline 1)", out)

    def test_stale_fails_and_shrink_fixes_it(self):
        fx = Fixture(self)
        fx.write("tools/layers-baseline.txt", "# keep this header\nA\tdrv -> gate\t1\nB\tdrv -> gate\t3\n")
        code, out = fx.run()
        self.assertEqual(code, 1)
        self.assertIn("STALE baseline A drv -> gate: gone", out)
        self.assertEqual(fx.run("--shrink")[0], 0)
        text = cl.read(os.path.join(fx.root, "tools/layers-baseline.txt"))
        self.assertEqual(text.strip(), "# keep this header")
        self.assertEqual(fx.run()[0], 0)

    def test_lowered_entry_is_stale(self):
        fx = self.broken()
        fx.write("tools/layers-baseline.txt", "A\tdrv -> gate\t5\n")
        code, out = fx.run()
        self.assertEqual(code, 1)
        self.assertIn("5 -> 1", out)

    def test_no_symbols_ignores_rule_c_entries(self):
        fx = Fixture(self)
        fx.write("tools/layers-baseline.txt", "C\tdrv:gate_fn\t1\n")
        self.assertEqual(fx.run("--no-symbols")[0], 0)
        self.assertEqual(fx.run()[0], 1)

    def test_grew_against_merge_base(self):
        base = {("A", "x -> y"): 1}
        self.assertEqual(cl.check_grew(base, base), [])
        self.assertEqual(cl.check_grew({}, base), [])
        self.assertIn("raised 1 -> 2", cl.check_grew({("A", "x -> y"): 2}, base)[0])
        self.assertIn("not in the merge-base", cl.check_grew({("A", "n -> m"): 1}, base)[0])

    def test_shrink_never_adds_or_raises(self):
        out = cl.shrink({("A", "a"): 5, ("A", "new"): 1}, {("A", "a"): 3, ("A", "gone"): 1})
        self.assertEqual(out, {("A", "a"): 3})

    def test_baseline_parse_errors(self):
        for bad in ("A\tx\n", "Z\tx\t1\n", "A\tx\tmany\n", "A\tx\t1\nA\tx\t2\n"):
            with self.assertRaises(cl.LayerError):
                cl.parse_baseline(bad)


class Manifest(unittest.TestCase):
    HEADER = """#pragma once
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { int a; } t_t;
// Safe from any task.
int one(int a,
        int b);
// Runs on the LVGL task. Not from the LVGL task is wrong here.
void two(void);
static inline int skipped(void) { return 0; }
#ifdef __cplusplus
}
#endif
"""

    def test_header_functions_and_thread_rules(self):
        self.assertEqual(cl.header_functions(self.HEADER), [("one", "any"), ("two", "not-lvgl")])

    def test_manifest_roundtrip_and_warning(self):
        fx = Fixture(self, overrides={"gate": (BASE_FILES["gate"][0], {"g.c": "", "gate.h": self.HEADER})})
        os.makedirs(os.path.join(fx.root, "firmware/components/lichen_api"))
        os.rename(os.path.join(fx.root, "firmware/components/gate/gate.h"), os.path.join(fx.root, "firmware/components/lichen_api/lichen_x.h"))
        self.assertEqual(fx.run("--gen-manifest")[0], 0)
        text = cl.read(os.path.join(fx.root, "tools/gate_manifest.txt"))
        self.assertIn("one\t-\tany\tlichen_x.h", text)
        self.assertEqual(cl.manifest_warnings(fx.root, text), [])
        warns = " ".join(cl.manifest_warnings(fx.root, text.replace("two\t", "other\t")))
        self.assertIn("two is declared", warns)
        self.assertIn("other is in", warns)


if __name__ == "__main__":
    unittest.main()
