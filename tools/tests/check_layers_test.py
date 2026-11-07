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

    def run(self, *args, git=False):
        extra = [] if git else ["--no-merge-base"]  # fixtures are no git repos unless a test says so
        r = subprocess.run([sys.executable, "-I", os.path.join(os.path.dirname(cl.__file__), "check_layers.py"),
                            "--root", self.root, "--nm", "nm"] + extra + list(args), capture_output=True, text=True)
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
        self.assertEqual(keys(fx), {("A!", "app -> esp_wifi")})

    def test_rule_b_upward_include(self):
        fx = Fixture(self, overrides={"drv": ("idf_component_register(SRCS \"d.c\" REQUIRES base)", {"d.c": "#include \"gate.h\"\nint drv_fn(void){return 2;}\n"})})
        self.assertEqual(keys(fx), {("B", "drv -> gate")})

    def test_rule_b_closed_layer_system_header_and_own_header(self):
        src = "#include <stdint.h>\n#include <stdio.h>\n#include \"mine.h\"\nint app_fn(void){return 4;}\n"
        fx = Fixture(self, overrides={"app": (BASE_FILES["app"][0], {"a.c": src, "mine.h": ""})})
        self.assertEqual(keys(fx), {("B!", "app -> stdio.h")})

    def test_comp_headers_allow_one_component_only(self):
        src = "#include <stdio.h>\nint %s(void){return 4;}\n"
        two = dict(BASE_FILES, app2=("idf_component_register(SRCS \"a2.c\" REQUIRES gate freertos)", {"a2.c": src % "app2_fn"}))
        conf = CONF.replace("component app L3", "component app app2 L3") + "comp-headers app stdio.h\n"
        fx = Fixture(self, conf=conf, overrides={"app": (BASE_FILES["app"][0], {"a.c": src % "app_fn"}), "app2": two["app2"]})
        self.assertEqual(keys(fx), {("B!", "app2 -> stdio.h")})  # app may, its sibling on the same layer may not

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
        self.assertEqual(keys(fx), {("C!", "app:esp_restart")})

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


class HardAndConf(unittest.TestCase):
    def test_hard_failures_ignore_the_baseline(self):
        src = "void esp_restart(void);\nint app_fn(void){esp_restart();return 4;}\n"
        fx = Fixture(self, overrides={"app": ("idf_component_register(SRCS \"a.c\" REQUIRES gate esp_wifi ${NOPE})", {"a.c": "#include <stdio.h>\n" + src})})
        fx.write("tools/layers-baseline.txt", "A\tapp -> esp_wifi\t1\nB\tapp -> stdio.h\t1\nC\tapp:esp_restart\t1\n")
        code, out = fx.run()
        self.assertEqual(code, 1)
        for want in ("HARD  A app -> esp_wifi", "HARD  B app -> stdio.h", "HARD  C app:esp_restart",
                     "app -> ${NOPE} (unresolved variable)"):
            self.assertIn(want, out)
        self.assertEqual(out.count("STALE"), 3)  # the baselined copies are not accepted as the entry either
        code, out = fx.run("--init")
        self.assertEqual(code, 1)
        self.assertIn("hard violations", out)

    def test_duplicate_component_and_unknown_peer_are_rejected(self):
        with self.assertRaises(cl.LayerError):
            cl.parse_conf("layer L0 x\ncomponent c L0\ncomponent c L0\n")
        with self.assertRaises(cl.LayerError):
            cl.parse_conf("layer L0 x\ncomponent c L0\npeer c ghost\n")
        with self.assertRaises(cl.LayerError):
            cl.parse_conf("layer L0 x\ncomponent c L0\nowner-only nvs_open ghost\n")

    def test_peer_allows_an_upward_edge_and_include(self):
        drv = ("idf_component_register(SRCS \"d.c\" PRIV_REQUIRES gate)", {"d.c": "#include \"gate.h\"\nint drv_fn(void){return 2;}\n"})
        self.assertEqual(keys(Fixture(self, overrides={"drv": drv})), {("A", "drv -> gate"), ("B", "drv -> gate")})
        self.assertEqual(keys(Fixture(self, conf=CONF + "peer drv gate\n", overrides={"drv": drv})), set())

    def test_owner_only_first_match_wins(self):
        src = "int heap_caps_walk(void);\nint %s(void){return heap_caps_walk();}\n"
        conf = CONF + "owner-only heap_caps_walk gate\nowner-only heap_caps_* drv\n"
        fx = Fixture(self, conf=conf, overrides={"drv": (BASE_FILES["drv"][0], {"d.c": src % "drv_fn"}),
                                                 "gate": (BASE_FILES["gate"][0], {"g.c": src % "gate_fn", "gate.h": ""})})
        self.assertEqual(keys(fx), {("C", "drv:heap_caps_walk")})  # the narrow line before the glob decides
        conf2 = CONF + "owner-only heap_caps_* drv\nowner-only heap_caps_walk gate\n"
        fx2 = Fixture(self, conf=conf2, overrides={"drv": (BASE_FILES["drv"][0], {"d.c": src % "drv_fn"}),
                                                   "gate": (BASE_FILES["gate"][0], {"g.c": src % "gate_fn", "gate.h": ""})})
        self.assertEqual(keys(fx2), {("C", "gate:heap_caps_walk")})  # glob first: the narrow line never applies

    def test_shrink_and_init_refuse_only_layer(self):
        fx = Fixture(self)
        fx.write("tools/layers-baseline.txt", "")
        for flag in ("--shrink", "--init"):
            code, out = fx.run(flag, "--only-layer", "L3")
            self.assertEqual(code, 1)
            self.assertIn("not with --only-layer", out)

    def test_real_conf_keeps_the_app_boundary_lists(self):
        """The L6 profile must keep what tools/app_boundary.conf (#178) said before #218 absorbed it."""
        conf = cl.parse_conf(cl.read(os.path.join(os.path.dirname(cl.__file__), "layers.conf")))
        self.assertEqual(conf.comp_layer["mesh"], "L6")
        self.assertEqual(conf.closed["L6"], {"freertos", "esp_timer", "heap", "mbedtls", "lvgl", "lvgl__lvgl"})
        self.assertEqual(conf.peers["mesh"], {"lichen_notify", "lichen_clock", "lichen_random", "lora", "fw_console", "psram_guard"})
        self.assertEqual(conf.allowed_layers("L6"), {"L4", "L6"})
        for h in ("stdio.h", "sys/*.h", "freertos/*.h", "esp_log.h", "esp_timer.h", "esp_heap_caps.h", "esp_attr.h",
                  "mbedtls/*.h", "psa/*.h", "lvgl.h", "stdatomic.h", "new"):
            self.assertIn(h, conf.headers["L6"])
        for f in ("esp_ota_*", "esp_wifi_*", "esp_efuse_*", "gpio_*", "esp_partition_*", "nvs_flash_init*", "nvs_flash_erase*",
                  "ledc_*", "i2c_*", "esp_restart", "lora_start", "lora_ident", "lora_ident_live", "lora_stop", "lora_c6_*",
                  "lora_set_send_hook", "lora_set_enabled", "lora_enabled", "xTaskCreate*"):
            self.assertIn(f, conf.forbid["L6"])
        self.assertIn("components/mesh/companion", conf.own_dirs["mesh"])
        self.assertIn("../meshcore/src", conf.own_dirs["mesh"])
        self.assertIn("../third_party/base64/src", conf.own_dirs["mesh"])
        # owner-only symbols the issue names, with the narrow heap lines before their glob
        globs = [g for g, _ in conf.owner_only]
        for g in ("lvgl_port_lock", "nvs_open*", "xTaskCreate*", "heap_caps_*", "lv_group_set_default"):
            self.assertIn(g, globs)
        self.assertLess(globs.index("heap_caps_walk"), globs.index("heap_caps_*"))
        self.assertLess(globs.index("heap_caps_get_*"), globs.index("heap_caps_*"))


class GitBase(unittest.TestCase):
    def git(self, fx, *args):
        subprocess.run(["git", "-C", fx.root, "-c", "user.name=t", "-c", "user.email=t@t", "-c", "commit.gpgsign=false"] + list(args),
                       check=True, capture_output=True)

    def repo(self, base_text):
        """A repo whose main has `base_text` as the baseline (None: no baseline yet) and a branch from it."""
        fx = Fixture(self)
        self.git(fx, "init", "-q", "-b", "main")
        self.git(fx, "config", "core.excludesFile", "/dev/null")
        with open(os.path.join(fx.root, ".gitignore"), "w") as f:
            f.write("firmware/build/\n")
        if base_text is not None:
            fx.write("tools/layers-baseline.txt", base_text)
        self.git(fx, "add", "-A")
        self.git(fx, "commit", "-q", "-m", "base")
        self.git(fx, "checkout", "-q", "-b", "work")
        return fx

    def test_baseline_introduced_on_the_branch_passes(self):
        fx = self.repo(None)
        fx.write("tools/layers-baseline.txt", "")
        self.assertEqual(fx.run(git=True)[0], 0)

    def test_grew_new_and_raised_entries_fail_shrunk_passes(self):
        fx = self.repo("A\tdrv -> gate\t2\n")
        fx.write("firmware/components/drv/CMakeLists.txt", "idf_component_register(SRCS \"d.c\" PRIV_REQUIRES gate)")
        fx.write("tools/layers-baseline.txt", "A\tdrv -> gate\t2\n")
        self.assertEqual(fx.run("--report", git=True)[0], 0)
        fx.write("tools/layers-baseline.txt", "A\tdrv -> gate\t1\n")
        self.assertEqual(fx.run(git=True)[0], 0)
        fx.write("tools/layers-baseline.txt", "A\tdrv -> gate\t3\n")
        code, out = fx.run(git=True)
        self.assertEqual(code, 1)
        self.assertIn("raised 2 -> 3", out)
        fx.write("tools/layers-baseline.txt", "A\tdrv -> gate\t1\nB\tdrv -> gate\t1\n")
        self.assertIn("not in the merge-base", fx.run(git=True)[1])

    def test_no_merge_base_fails_loudly(self):
        fx = self.repo(None)
        self.git(fx, "branch", "-m", "work", "other")
        self.git(fx, "branch", "-m", "main", "trunk")
        fx.write("tools/layers-baseline.txt", "")
        code, out = fx.run(git=True)
        self.assertEqual(code, 1)
        self.assertIn("no merge base", out)

    def test_local_main_beats_a_stale_origin_main(self):
        fx = self.repo("A\tdrv -> gate\t1\n")
        old = subprocess.run(["git", "-C", fx.root, "rev-parse", "main"], capture_output=True, text=True).stdout.strip()
        self.git(fx, "checkout", "-q", "main")
        fx.write("tools/layers-baseline.txt", "")  # main shrank it since origin/main
        self.git(fx, "commit", "-q", "-am", "shrink")
        self.git(fx, "update-ref", "refs/remotes/origin/main", old)
        self.git(fx, "checkout", "-q", "-b", "newer")
        fx.write("firmware/components/drv/CMakeLists.txt", "idf_component_register(SRCS \"d.c\" PRIV_REQUIRES gate)")
        fx.write("tools/layers-baseline.txt", "A\tdrv -> gate\t1\n")  # raised again against the newer main
        code, out = fx.run(git=True)
        self.assertEqual(code, 1)
        self.assertIn("GREW", out)


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

    def test_allow_grow_only_for_marked_new_entries(self):
        mark = "# moved from shell, #221 removes"
        text = "C\tapp:fn\t1\t" + mark + "\nC\tapp:other\t1\n"
        cur, marks = cl.parse_baseline(text), cl.baseline_marks(text)
        self.assertEqual(cur, {("C", "app:fn"): 1, ("C", "app:other"): 1})  # the mark is not part of the count
        self.assertEqual(len(cl.check_grew(cur, {}, marks, None)), 2)       # no escape: both are GREW
        self.assertEqual(len(cl.check_grew(cur, {}, marks, "300")), 2)      # another issue's number: no help
        out = cl.check_grew(cur, {}, marks, "221")
        self.assertEqual(len(out), 1)                                        # only the unmarked one still fails
        self.assertIn("app:other", out[0])
        raised = {("C", "app:fn"): 2}
        self.assertIn("raised", cl.check_grew(raised, {("C", "app:fn"): 1}, {("C", "app:fn"): mark}, "221")[0])

    def test_shrink_keeps_marks(self):
        text = "C\tapp:fn\t1\t# moved from shell, #221 removes\n"
        out = cl.format_baseline("# h", cl.parse_baseline(text), cl.baseline_marks(text))
        self.assertIn("C\tapp:fn\t1\t# moved from shell, #221 removes", out)

    def test_allow_grow_matches_the_whole_issue_number(self):
        mark = "# moved from shell, #221 removes"
        cur, marks = {("C", "app:fn"): 1}, {("C", "app:fn"): mark}
        for bad in ("22", "2", "1", "221x", "#221", "", "abc"):
            self.assertEqual(len(cl.check_grew(cur, {}, marks, bad)), 1, bad)
        self.assertEqual(cl.check_grew(cur, {}, marks, "221"), [])

    def test_allow_grow_is_for_rule_c_only(self):
        mark = "# moved from shell, #221 removes"
        cur, marks = {("A", "x -> y"): 1}, {("A", "x -> y"): mark}
        self.assertEqual(len(cl.check_grew(cur, {}, marks, "221")), 1)

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
