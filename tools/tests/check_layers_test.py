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
        # lichen_notify left both peer lines (#283): its app header lives in lichen_api since #245
        self.assertEqual(conf.peers["mesh"], {"lichen_clock", "lichen_random", "lora", "fw_console"})  # #353: no psram_guard
        self.assertEqual(conf.peers["lichen_apps"], {"lichen_clock", "lora", "fw_console"})
        self.assertEqual(conf.allowed_layers("L6"), {"L4", "L6"})
        for h in ("stdio.h", "sys/*.h", "freertos/*.h", "esp_log.h", "esp_timer.h", "esp_attr.h",
                  "mbedtls/*.h", "psa/*.h", "lvgl.h", "stdatomic.h", "new"):
            self.assertIn(h, conf.headers["L6"])
        self.assertNotIn("esp_heap_caps.h", conf.headers["L6"])  # apps allocate through lichen_mem.h (#283)
        for f in ("esp_ota_*", "esp_wifi_*", "esp_efuse_*", "gpio_*", "esp_partition_*", "nvs_flash_init*", "nvs_flash_erase*",
                  "ledc_*", "i2c_*", "esp_restart", "lora_start", "lora_ident", "lora_ident_live", "lora_stop", "lora_c6_*",
                  "lora_set_send_hook", "lora_set_enabled", "lora_enabled", "lora_set_switch_owner", "xTaskCreate*"):
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
        # notify_os.h and notify_bind.h are the OS's side (#245): every one of their functions is forbidden to apps,
        # and none of lichen_notify.h's
        for f in ("lichen_notify_start", "lichen_notify_save_now", "lichen_notify_set_clock", "lichen_notify_generation",
                  "lichen_notify_ids", "lichen_notify_get", "lichen_notify_mark_read_id", "lichen_notify_mark_read_upto",
                  "lichen_notify_mark_all_read", "lichen_notify_remove", "lichen_notify_clear_all", "lichen_notify_set_dnd",
                  "lichen_notify_set_source_enabled", "lichen_notify_next_toast", "lichen_notify_set_feedback",
                  "lichen_notify_feedback", "lichen_notify_stats", "lichen_notify_bind_store",
                  "lichen_notify_set_state_hooks"):
            self.assertIn(f, conf.forbid["L6"])
        for f in ("lichen_notify", "lichen_notify_to", "lichen_notify_unread", "lichen_notify_unread_top",
                  "lichen_notify_mark_read", "lichen_notify_source_enabled", "lichen_notify_dnd"):
            self.assertFalse(any(cl.fnmatch.fnmatchcase(f, g) for g in conf.forbid["L6"]), f)
        # the console's LoRa switch saves "off" (#222): lora alone may call it, never a peer app
        self.assertIn(("lora_switch_request", ["lora"]), conf.owner_only)

    def test_real_conf_closes_the_283_gaps(self):
        """#283: the peers' internal headers and symbols, plain heap calls and flash mmap are closed, the shim is scanned."""
        conf = cl.parse_conf(cl.read(os.path.join(os.path.dirname(cl.__file__), "layers.conf")))
        for app in ("mesh", "lichen_apps"):
            for inc in ("lora_internal.h", "lora_os.h", "lora_link.h", "lora_tx.h", "lora_owner.h", "lora_switch.h",
                        "console_cmd.h", "fw_store.h", "key_raw.h", "nvs_erase_os.h", "psram_guard.h", "clock_check.h"):
                for dep in conf.peers[app]:
                    self.assertFalse(conf.header_ok(app, dep, inc), (app, dep, inc))
            self.assertTrue(conf.header_ok(app, "lora", "lora_app.h") and conf.header_ok(app, "fw_console", "fw_console.h"))
        self.assertNotIn("psram_guard", conf.peers["mesh"])  # #353: the trial's hold was its only use
        for sym in ("lora_internal_send", "lora_sweep_answer", "lora_tx_begin", "lora_owner_claim", "lora_c6_link_init",
                    "psram_speed_choice", "psram_speed_cmd", "psram_speed_gate_result", "psram_speed_no_rearm",
                    "malloc", "calloc", "realloc", "free", "strdup"):
            self.assertTrue(any(cl.fnmatch.fnmatchcase(sym, g) for g in conf.forbid["L6"]), sym)
        for sym in ("lora_sweep", "lora_set_tx_power", "lora_claim", "lichen_alloc", "lichen_free"):
            self.assertFalse(any(cl.fnmatch.fnmatchcase(sym, g) for g in conf.forbid["L6"]), sym)
        self.assertEqual(conf.forbid_except, [("mesh", "MyMesh.cpp.obj", ["malloc", "free"])])
        for g in ("esp_partition_mmap", "spi_flash_mmap*", "esp_mmu_map*"):
            self.assertIn((g, []), conf.owner_only)
        self.assertIn("../shared/arduino_shim", conf.own_dirs["mesh"])
        self.assertNotIn("../shared/arduino_shim", conf.vendored["mesh"])

    def test_real_conf_lets_apps_read_the_registry_and_set_nothing(self):
        """#313: lichen_reg.h's lichen_reg_get is the apps' one way to a registry key, and it is a read. The gate
        declares no registry setter, and L6 may call none of the OS's: the service, the setters, the transport."""
        tools = os.path.dirname(cl.__file__)
        conf = cl.parse_conf(cl.read(os.path.join(tools, "layers.conf")))
        gate = {f: h for f, h, _ in cl.gate_functions(os.path.join(tools, ".."))}
        self.assertEqual(gate.get("lichen_reg_get"), "lichen_reg.h")
        self.assertEqual([f for f, h in gate.items() if h == "lichen_reg.h"], ["lichen_reg_get"])
        self.assertFalse(any(cl.fnmatch.fnmatchcase("lichen_reg_get", g) for g in conf.forbid["L6"]))
        for sym in ("lichen_reg_cached_text", "lichen_reg_start", "reg_set", "reg_cmd_set", "reg_p4_bl_set",
                    "reg_p4_wifi_ps_set", "reg_p4_find_name", "reg_proxy_answer", "lora_reg_set", "lora_reg_get"):
            self.assertTrue(any(cl.fnmatch.fnmatchcase(sym, g) for g in conf.forbid["L6"]), sym)

    def test_real_conf_lets_apps_read_the_radios_and_switch_none(self):
        """#360: apps read the radio state and listen; the setters are the OS's (lichen_radio_set lifts a thermal hold).
        The setters are gate functions, so without the forbid an L6 call would pass the layer rule."""
        tools = os.path.dirname(cl.__file__)
        conf = cl.parse_conf(cl.read(os.path.join(tools, "layers.conf")))
        gate = {f: h for f, h, _ in cl.gate_functions(os.path.join(tools, ".."))}
        def forbidden(sym):
            return any(cl.fnmatch.fnmatchcase(sym, g) for g in conf.forbid["L6"])
        for sym in ("lichen_radio_set", "lichen_radio_switch_set", "lichen_airplane_set"):
            self.assertEqual(gate.get(sym), "lichen_radio.h", sym)
            self.assertTrue(forbidden(sym), sym)
        for sym in ("lichen_radio_get", "lichen_radio_switch_get", "lichen_airplane_get", "lichen_radio_listen",
                    "lichen_radio_name", "lichen_radio_lora_pause", "lichen_radio_lora_resume"):
            self.assertFalse(forbidden(sym), sym)

    def test_app_api_doc_matches_the_conf(self):
        """docs/app-api.md's peer table and "Exceptions still open" list say what layers.conf grants apps (#283)."""
        tools = os.path.dirname(cl.__file__)
        conf = cl.parse_conf(cl.read(os.path.join(tools, "layers.conf")))
        doc = cl.read(os.path.join(tools, "..", "docs", "app-api.md"))
        apps = [c for c, layer in conf.comp_layer.items() if layer in conf.closed]
        table = doc.split("## What an app may use", 1)[1].split("###", 1)[0]
        rows = {}
        for line in table.splitlines():
            cells = [c.strip() for c in line.strip().strip("|").split("|")]
            m = cl.re.fullmatch(r"`(\w+)`", cells[0]) if len(cells) == 3 else None
            if m and m.group(1) in conf.comp_layer and m.group(1) != cl.GATE:
                rows[m.group(1)] = set(cl.re.findall(r"`([^`]+)`", cells[1]))
        peers = set().union(*(conf.peers[a] for a in apps))
        self.assertEqual(set(rows), peers)
        for dep in peers:
            granted = set().union(*(set(conf.peer_headers.get((a, dep), [])) for a in apps))
            self.assertEqual(rows[dep], granted, dep)
        section = doc.split("## Exceptions still open", 1)[1].split("\n## ", 1)[0]
        listed = set(cl.re.findall(r"^- `((?:comp-headers|forbid-except) [^`]+)`", section, cl.re.M))
        want = set("comp-headers %s %s" % (c, " ".join(h)) for c, h in conf.comp_headers.items() if c in apps)
        want |= set("forbid-except %s %s %s" % (c, o, " ".join(sy)) for c, o, sy in conf.forbid_except if c in apps)
        self.assertEqual(listed, want)
        self.assertNotIn("No component or header exception is open", doc)

    def test_real_conf_symbol_globs_name_real_symbols(self):
        """Every forbid and owner-only glob matches a symbol that exists (#283): a typo such as lora_intrnal_* would
        match nothing and guard nothing. Known symbols: the identifiers of the repo's sources plus the IDF functions
        that no repo source names (they are banned outright), checked against the IDF headers when IDF_PATH is set."""
        tools = os.path.dirname(cl.__file__)
        root = os.path.join(tools, "..")
        conf = cl.parse_conf(cl.read(os.path.join(tools, "layers.conf")))
        external = {"strndup": "", "esp_partition_mmap": "esp_partition", "spi_flash_mmap": "spi_flash",
                    "esp_mmu_map": "esp_mm"}
        known = set(external)
        for base in ("firmware/components", "firmware/main", "shared"):
            for path in cl.source_files(os.path.join(root, base)):
                with open(path, encoding="utf-8", errors="replace") as f:
                    known |= set(cl.re.findall(r"\b[A-Za-z_]\w*\b", f.read()))
        globs = [g for gs in conf.forbid.values() for g in gs] + [g for g, _ in conf.owner_only]
        dead = [g for g in globs if not any(cl.fnmatch.fnmatchcase(k, g) for k in known)]
        self.assertEqual(dead, [])
        idf = os.environ.get("IDF_PATH", "")
        for sym, comp in external.items():
            if comp and os.path.isdir(os.path.join(idf, "components", comp)):
                text = ""
                for f in cl.source_files(os.path.join(idf, "components", comp)):
                    if f.endswith(".h"):
                        with open(f, encoding="utf-8", errors="replace") as h:
                            text += h.read()
                self.assertRegex(text, r"\b%s\s*\(" % sym)

    def test_real_gate_requires_no_hardware_layer(self):
        """The gate must not require L1/L2 (#220), nor lichen_notify since its headers moved into the gate (#245)."""
        tools = os.path.dirname(cl.__file__)
        conf = cl.parse_conf(cl.read(os.path.join(tools, "layers.conf")))
        self.assertEqual(conf.comp_layer["lichen_api"], "L4")
        self.assertEqual(conf.allowed_layers("L4"), {"L0", "L4"})
        self.assertEqual(conf.comp_layer["lichen_os"], "L3")
        cm = cl.read(os.path.join(tools, "..", "firmware", "components", "lichen_api", "CMakeLists.txt"))
        bad = [d for _, d in cl.parse_requires(cm) if conf.comp_layer.get(d) in ("L1", "L2", "L3")]
        self.assertEqual(bad, [])


class Gaps283(unittest.TestCase):
    """The #283 rules on fixtures: each one catches its violation."""
    PEER = CONF + "peer app drv\npeer-headers app drv drv.h\n"

    def app(self, src, extra=None):
        return {"app": (BASE_FILES["app"][0], dict({"a.c": src}, **(extra or {})))}

    def test_peer_headers_narrow_a_closed_peer(self):
        drv = (BASE_FILES["drv"][0], {"d.c": "int drv_fn(void){return 2;}\n", "drv.h": "", "drv_internal.h": ""})
        # rule B reads the include lines, the fixture's gcc has no -I for drv: hence #if 0
        ok = "#if 0\n#include \"drv.h\"\n#endif\nint app_fn(void){return 4;}\n"
        bad = "#if 0\n#include \"drv_internal.h\"\n#endif\nint app_fn(void){return 4;}\n"
        self.assertEqual(keys(Fixture(self, conf=self.PEER, overrides=dict(self.app(ok), drv=drv))), set())
        self.assertEqual(keys(Fixture(self, conf=self.PEER, overrides=dict(self.app(bad), drv=drv))),
                         {("B!", "app -> drv:drv_internal.h")})
        # without the line the peer edge allows every header: the gap #283 closes
        conf = self.PEER.replace("peer-headers app drv drv.h\n", "")
        with self.assertRaises(cl.LayerError):  # a closed layer's peer must name its headers
            cl.parse_conf(conf)

    def test_peer_headers_need_a_peer_line(self):
        with self.assertRaises(cl.LayerError):
            cl.parse_conf(CONF + "peer-headers app drv drv.h\n")
        with self.assertRaises(cl.LayerError):
            cl.parse_conf(CONF + "peer app drv\npeer-headers app drv\n")

    def test_own_dir_outside_the_component_is_scanned_unless_vendored(self):
        conf = CONF + "own-dir app ../shared/shim ../third/lib\n"
        fx = Fixture(self, conf=conf + "vendored app ../third/lib\n")
        fx.write("shared/shim/shim.c", "#include <dirent.h>\n#include \"shim.h\"\n")
        fx.write("shared/shim/shim.h", "")
        fx.write("third/lib/up.c", "#include <fcntl.h>\n")
        self.assertEqual(keys(fx), {("B!", "app -> dirent.h")})  # the shim is the app's code, the vendored lib is not
        fx.write("tools/layers.conf", conf + "vendored app ../third/lib\ncomp-headers app dirent.h\n")
        self.assertEqual(keys(fx), set())
        fx.write("tools/layers.conf", conf)  # third/lib not vendored: scanned as well
        self.assertEqual(keys(fx), {("B!", "app -> dirent.h"), ("B!", "app -> fcntl.h")})
        with self.assertRaises(cl.LayerError):
            cl.parse_conf(CONF + "vendored app ../nowhere\n")

    def test_missing_own_dir_root_fails(self):
        fx = Fixture(self, conf=CONF + "own-dir app ../gone\n")
        code, out = fx.run("--no-symbols", "--report")
        self.assertEqual(code, 1)
        self.assertIn("does not exist", out)

    def test_forbid_plain_heap_and_forbid_except(self):
        conf = CONF.replace("forbid L3 esp_restart", "forbid L3 esp_restart malloc free")
        src = "void *malloc(__SIZE_TYPE__);\nvoid free(void *);\nint app_fn(void){free(malloc(4));return 4;}\n"
        fx = Fixture(self, conf=conf, overrides=self.app(src, {"up.c": src.replace("app_fn", "up_fn")}))
        self.assertEqual(keys(fx), {("C!", "app:malloc"), ("C!", "app:free")})
        fx.write("tools/layers.conf", conf + "forbid-except app up.c.obj malloc free\n")
        conf2 = cl.parse_conf(cl.read(os.path.join(fx.root, "tools/layers.conf")))
        found = cl.measure(conf2, fx.root, os.path.join(fx.root, "firmware/build"), "nm", True)
        self.assertEqual(set(found), {("C!", "app:malloc"), ("C!", "app:free")})
        self.assertTrue(all("a.c.obj" in w for ws in found.values() for w in ws))  # up.c.obj is excepted, a.c.obj not
        fx.write("firmware/components/app/a.c", "int app_fn(void){return 4;}\n")
        fx.build()
        self.assertEqual(keys(fx), set())
        # keyed to the object's name: renamed upstream (or fixed), the line matches no use and fails instead of passing
        os.rename(os.path.join(fx.root, "firmware/components/app/up.c"), os.path.join(fx.root, "firmware/components/app/up2.c"))
        shutil.rmtree(os.path.join(fx.root, "firmware/build/esp-idf/app"))
        fx.build()
        self.assertEqual(keys(fx), {("C!", "app:malloc"), ("C!", "app:free"),
                                    ("C!", "app:forbid-except up.c.obj malloc free (no such use in the archive)")})

    def test_owner_only_without_owner_bans_everyone(self):
        src = "int esp_partition_mmap(void);\nint %s(void){return esp_partition_mmap();}\n"
        fx = Fixture(self, conf=CONF + "owner-only esp_partition_mmap\n",
                     overrides={"base": (BASE_FILES["base"][0], {"b.c": src % "base_fn", "base.h": ""}),
                                "drv": (BASE_FILES["drv"][0], {"d.c": src % "drv_fn"})})
        # hard, like forbid: a baseline entry cannot absorb it (#283 review)
        self.assertEqual(keys(fx), {("C!", "base:esp_partition_mmap"), ("C!", "drv:esp_partition_mmap")})
        fx.write("tools/layers-baseline.txt", "C\tbase:esp_partition_mmap\t1\nC\tdrv:esp_partition_mmap\t1\n")
        code, out = fx.run()
        self.assertEqual(code, 1)
        self.assertIn("HARD  C drv:esp_partition_mmap", out)


# Gate attribution (#220): lichen_os implements the gate's headers. Layers as the real tree, fewer of them.
GATE_CONF = """\
layer L0 platform
layer L1 drivers
layer L2 services
layer L3 gate
layer L4 system
layer L5 apps
component base L0
component drv L1
component lichen_os lichen_notify L2
component gate lichen_api L3
component shell L4
component app L5
layer-may L3 L0 L3
layer-may L4 L0 L3 L4
layer-may L5 L3 L5
peer lichen_os lichen_api
peer lichen_notify lichen_api
owner-only os_lock lichen_os
"""
GATE_HEADER = "int os_fn(void);\nint shell_fn(void);\nint os_lock(void);\nint notify_fn(void);\n"


class GateAttribution(unittest.TestCase):
    def fixture(self, app_calls, manifest=True):
        decl = "".join("int %s(void);\n" % f for f in app_calls)
        body = "+".join("%s()" % f for f in app_calls) or "0"
        files = {
            "lichen_os": ("idf_component_register(SRCS \"o.c\" PRIV_REQUIRES lichen_api drv)",
                          {"o.c": "int os_fn(void){return 1;}\nint os_private(void){return 2;}\nint os_lock(void){return 6;}\n"}),
            "lichen_notify": ("idf_component_register(SRCS \"n.c\" REQUIRES lichen_api)",
                              {"n.c": "int notify_fn(void){return 7;}\nint notify_private(void){return 8;}\n"}),
            "lichen_api": ("idf_component_register(SRCS \"x.c\" INCLUDE_DIRS \".\" REQUIRES base)",
                           {"x.c": "int api_fn(void){return 5;}\n", "lichen_x.h": GATE_HEADER}),
            "shell": ("idf_component_register(SRCS \"s.c\" REQUIRES lichen_api)", {"s.c": "int shell_fn(void){return 3;}\n"}),
            "app": ("idf_component_register(SRCS \"a.c\" REQUIRES lichen_api)",
                    {"a.c": decl + "int app_fn(void){return %s;}\n" % body}),
        }
        fx = Fixture(self, conf=GATE_CONF, overrides=files)
        if manifest:
            self.assertEqual(fx.run("--gen-manifest")[0], 0)
        return fx

    def test_app_call_to_a_gate_function_lichen_os_defines_passes(self):
        self.assertEqual(keys(self.fixture(["os_fn"])), set())

    def test_app_call_to_a_gate_function_lichen_notify_defines_passes(self):
        self.assertEqual(keys(self.fixture(["notify_fn"])), set())  # the second implementer (#245)

    def test_shell_call_to_a_gate_function_lichen_notify_defines_passes(self):
        fx = self.fixture([])
        fx.write("firmware/components/shell/s.c", "int notify_fn(void);\nint shell_fn(void){return notify_fn();}\n")
        fx.build()
        self.assertEqual(keys(fx), set())

    def test_app_call_to_a_lichen_notify_function_no_header_declares_fails(self):
        self.assertEqual(keys(self.fixture(["notify_private"])), {("C", "app:notify_private")})

    def test_the_gate_calling_into_lichen_notify_stays_a_violation(self):
        fx = self.fixture([])
        fx.write("firmware/components/lichen_api/x.c", "int notify_fn(void);\nint api_fn(void){return notify_fn();}\n")
        fx.build()
        self.assertEqual(keys(fx), {("C", "lichen_api:notify_fn")})  # an implementer is still below the gate

    def test_app_call_to_a_lichen_os_function_no_header_declares_fails(self):
        self.assertEqual(keys(self.fixture(["os_private"])), {("C", "app:os_private")})

    def test_gate_function_the_shell_defines_stays_an_upward_edge(self):
        self.assertEqual(keys(self.fixture(["shell_fn"])), {("C", "app:shell_fn")})

    def test_the_gate_calling_into_lichen_os_stays_a_violation(self):
        fx = self.fixture([])
        fx.write("firmware/components/lichen_api/x.c", "int os_fn(void);\nint api_fn(void){return os_fn();}\n")
        fx.build()
        self.assertEqual(keys(fx), {("C", "lichen_api:os_fn")})  # L4 may not use L3, header or not

    def test_a_lower_layer_calling_a_gate_function_lichen_os_defines_stays_flagged(self):
        fx = self.fixture([])
        fx.write("firmware/components/drv/d.c", "int os_fn(void);\nint drv_fn(void){return os_fn();}\n")
        fx.build()
        self.assertEqual(keys(fx), {("C", "drv:os_fn")})  # L1 may reach neither lichen_os (L2) nor the gate (L3)

    def test_a_header_declared_owner_only_symbol_stays_restricted(self):
        self.assertEqual(keys(self.fixture(["os_lock"])), {("C", "app:os_lock")})

    def test_a_manifest_line_alone_frees_nothing(self):
        fx = self.fixture(["os_private"])
        mf = os.path.join(fx.root, "tools/gate_manifest.txt")
        fx.write("tools/gate_manifest.txt", cl.read(mf) + "os_private\t-\tany\tlichen_x.h\n")
        self.assertEqual(keys(fx), {("C", "app:os_private")})  # rule C reads the headers, not the manifest
        fx.write("tools/layers-baseline.txt", "C\tapp:os_private\t1\n")
        code, out = fx.run()
        self.assertEqual(code, 1)
        self.assertIn("os_private is in tools/gate_manifest.txt but no header declares it", out)

    def test_manifest_must_match_the_headers(self):
        fx = self.fixture(["os_fn"], manifest=False)
        fx.write("tools/layers-baseline.txt", "")
        code, out = fx.run()
        self.assertEqual(code, 1)
        self.assertIn("gate_manifest.txt is missing", out)
        self.assertEqual(fx.run("--gen-manifest")[0], 0)
        self.assertEqual(fx.run()[0], 0, fx.run()[1])
        fx.write("firmware/components/lichen_api/lichen_x.h", GATE_HEADER + "int new_fn(void);\n")
        code, out = fx.run()
        self.assertEqual(code, 1)
        self.assertIn("new_fn is declared in a lichen_api header but not in tools/gate_manifest.txt", out)

    def test_app_reads_a_registry_key_through_the_gate_and_sets_none(self):
        """#313 with the real lichen_reg.h and the real L6 forbid lines: an app's lichen_reg_get (lichen_os defines it)
        passes; lichen_os's own cached-text call and any setter are hard failures."""
        tools = os.path.dirname(cl.__file__)
        header = cl.read(os.path.join(tools, "..", "firmware", "components", "lichen_api", "lichen_reg.h"))
        real = cl.parse_conf(cl.read(os.path.join(tools, "layers.conf")))
        # the fixture's apps are L5
        forbid = "".join("forbid L5 %s\n" % g for g in real.forbid["L6"] if "reg" in g or g == "lora_get_requested")
        os_src = ("int lichen_reg_cached_text(void){return 1;}\nint lichen_reg_get(void){return 1;}\n"
                  "int reg_set(void){return 2;}\nint reg_cmd_set(void){return 3;}\n")
        files = {
            "lichen_os": ("idf_component_register(SRCS \"o.c\" PRIV_REQUIRES lichen_api drv)", {"o.c": os_src}),
            "lichen_notify": ("idf_component_register(SRCS \"n.c\" REQUIRES lichen_api)", {"n.c": ""}),
            "lichen_api": ("idf_component_register(SRCS \"x.c\" INCLUDE_DIRS \".\" REQUIRES base)",
                           {"x.c": "int api_fn(void){return 5;}\n", "lichen_reg.h": header}),
            "shell": ("idf_component_register(SRCS \"s.c\" REQUIRES lichen_api)", {"s.c": ""}),
            "app": ("idf_component_register(SRCS \"a.c\" REQUIRES lichen_api)",
                    {"a.c": "int lichen_reg_get(void);\nint app_fn(void){return lichen_reg_get();}\n"}),
        }
        fx = Fixture(self, conf=GATE_CONF + forbid, overrides=files)
        self.assertEqual(fx.run("--gen-manifest")[0], 0)
        self.assertEqual(keys(fx), set())
        for sym in ("lichen_reg_cached_text", "reg_set", "reg_cmd_set"):
            fx.write("firmware/components/app/a.c", "int %s(void);\nint app_fn(void){return %s();}\n" % (sym, sym))
            fx.build()
            self.assertEqual(keys(fx), {("C!", "app:" + sym)})

    def test_gate_must_not_require_l1_or_l2(self):
        fx = self.fixture([])
        fx.write("firmware/components/lichen_api/CMakeLists.txt",
                 "idf_component_register(SRCS \"x.c\" REQUIRES base PRIV_REQUIRES drv lichen_os)")
        self.assertEqual(keys(fx), {("A", "lichen_api -> drv"), ("A", "lichen_api -> lichen_os")})


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

    def test_prototype_after_static_inline_body_is_seen(self):
        # #247: the closing brace of an inline body used to glue itself to the next prototype
        h = """// Any task.
static inline int helper(int a)
{
    if (a) {
        return 1;
    }
    return 0;
}
// LVGL task only.
int after_multi(void);
static inline int one_liner(void) { return 0; }
// Not on the LVGL task.
int after_one_liner(void);
"""
        self.assertEqual(cl.header_functions(h), [("after_multi", "lvgl"), ("after_one_liner", "not-lvgl")])

    def test_attribute_prefix_and_suffix(self):
        h = """// Any task.
__attribute__((warn_unused_result)) int first(void);
// LVGL task only.
int second(int a) __attribute__((nonnull(1), format(printf, 1, 2)));
"""
        self.assertEqual(cl.header_functions(h), [("first", "any"), ("second", "lvgl")])

    def test_two_prototypes_on_one_line(self):
        h = "// Pure, any task.\nint a(void); int b(int x);\n// LVGL task only.\nvoid c(void);\n"
        self.assertEqual(cl.header_functions(h), [("a", "any"), ("b", "any"), ("c", "lvgl")])

    def test_never_on_the_lvgl_task_is_not_lvgl(self):
        # notify_os.h's lichen_notify_save_now says "Blocks: never on the LVGL task" (#245 review)
        self.assertEqual(cl.thread_rule("// Restart / power off: waits. Blocks: never on the LVGL task."), "not-lvgl")
        self.assertEqual(cl.thread_rule("// Never from the LVGL task"), "not-lvgl")
        self.assertEqual(cl.thread_rule("// not on the LVGL task"), "not-lvgl")
        self.assertEqual(cl.thread_rule("// on the LVGL task only"), "lvgl")

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
