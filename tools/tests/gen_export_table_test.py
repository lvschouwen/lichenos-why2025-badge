#!/usr/bin/env python3
"""Host test of the export table generator (#519): the allow-list syntax, the forbid and wrap rules, the version-bump
rule and the app-symbol check. Run with python3 -I."""
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import gen_export_table as g  # noqa: E402


class AllowTest(unittest.TestCase):
    def test_syntax(self):
        self.assertEqual(g.parse_allow("memcpy # c\n\nmalloc = lichen_exp_malloc\n"), [("memcpy", None), ("malloc", "lichen_exp_malloc")])

    def test_bad_line(self):
        for bad in ("a b", "a = b = c", "1x", "!a"):
            with self.assertRaises(g.ExportError):
                g.parse_allow(bad)


class HashTest(unittest.TestCase):
    h = staticmethod(g.prototype_hash)

    def test_param_names_do_not_matter(self):
        self.assertEqual(self.h("bool f(lichen_ctx_t ctx, char const *fmt, size_t n)"), self.h("bool f(lichen_ctx_t c, char const *format, size_t len)"))
        self.assertEqual(self.h("int f(uint8_t buf[16], struct foo *p)"), self.h("int f(uint8_t b[16], struct foo *q)"))

    def test_signature_changes_do(self):
        base = self.h("bool f(lichen_ctx_t ctx, int n)")
        for changed in ("bool f(lichen_ctx_t ctx, long n)", "int f(lichen_ctx_t ctx, int n)", "bool f(lichen_ctx_t ctx)",
                        "bool f(lichen_ctx_t ctx, int n, int m)", "bool f(lichen_ctx_t ctx, unsigned n)"):
            self.assertNotEqual(base, self.h(changed), changed)

    def test_unnamed_and_void(self):
        self.assertEqual(self.h("void f(void)"), self.h("void f( void )"))
        self.assertNotEqual(self.h("void f(int)"), self.h("void f(long)"))
        self.assertEqual(self.h("void f(unsigned)"), self.h("void f(unsigned)"))
        self.assertNotEqual(self.h("void f(struct a x)"), self.h("void f(struct b x)"))


class Conf:
    forbid = {"L6": ["lichen_sys_*"]}


class ApiTest(unittest.TestCase):
    protos = {"a_one": ("a.h", "1"), "a_two": ("a.h", "2"), "b_one": ("b.h", "3"), "lichen_sys_x": ("a.h", "4")}

    def sel(self, text):
        with tempfile.TemporaryDirectory() as root:
            os.makedirs(os.path.join(root, "tools"))
            with open(os.path.join(root, g.API_REL), "w", encoding="utf-8") as f:
                f.write(text)
            return g.api_selection(root, Conf(), self.protos)

    def test_header_func_deny(self):
        got = self.sel("header a.h\ndeny a_two\nfunc b_one\n")
        self.assertEqual(sorted(got), ["a_one", "b_one"])  # lichen_sys_x is forbidden, so not taken

    def test_unlisted_is_not_exported(self):
        self.assertEqual(sorted(self.sel("func b_one\n")), ["b_one"])

    def test_matches_nothing_fails(self):
        for bad in ("header c.h\n", "header a.h\ndeny b_one\n", "func nope\n", "func lichen_sys_x\n",
                    "header a.h\nfunc a_one\n", "header a.h\ndeny lichen_sys_x\n"):
            with self.assertRaises(g.ExportError, msg=bad):
                self.sel(bad)

    def test_syntax(self):
        with self.assertRaises(g.ExportError):
            g.parse_api("bogus x\n")


class WrapTest(unittest.TestCase):
    def test_commented_wrap_ignored(self):
        with tempfile.TemporaryDirectory() as root:
            d = os.path.join(root, "firmware", "components", "c")
            os.makedirs(d)
            with open(os.path.join(d, "CMakeLists.txt"), "w", encoding="utf-8") as f:
                f.write('target_link_libraries(x INTERFACE "-Wl,--wrap=real_one")\n# "-Wl,--wrap=old_one"\n'
                        'foo() # "-Wl,--wrap=trailing"\n')
            self.assertEqual(g.find_wraps(root), ["real_one"])


class TableTest(unittest.TestCase):
    def test_c_text_is_sorted_and_terminated(self):
        c = g.c_text([("a", "a", "x.h", "1"), ("b", "__wrap_b", "x.h", "2")])
        self.assertIn('{"a", (void *)a},\n    {"b", (void *)__wrap_b},\n    {NULL, NULL}', c)
        self.assertIn("extern void __wrap_b(void);", c)

    def test_parse_round_trip(self):
        t = g.table_text([("a", "a", "x.h", "h1")], ["esp_restart"], (1, 2))
        self.assertEqual(g.parse_table(t), ((1, 2), {"a": "h1"}, ["esp_restart"]))


class VersionTest(unittest.TestCase):

    def test_added_needs_minor(self):
        self.assertTrue(g.bump_problems((1, 0), {"a": "a"}, (1, 0), {"a": "a", "b": "b"}))
        self.assertFalse(g.bump_problems((1, 0), {"a": "a"}, (1, 1), {"a": "a", "b": "b"}))

    def test_removed_or_changed_needs_major(self):
        self.assertTrue(g.bump_problems((1, 3), {"a": "a", "b": "b"}, (1, 4), {"a": "a"}))
        self.assertTrue(g.bump_problems((1, 3), {"a": "a"}, (1, 4), {"a": "other"}))
        self.assertFalse(g.bump_problems((1, 3), {"a": "a", "b": "b"}, (2, 0), {"a": "a"}))

    def test_hash_change_is_major_wrap_is_not(self):
        self.assertTrue(g.bump_problems((1, 0), {"a": "h1"}, (1, 1), {"a": "h2"}))
        self.assertFalse(g.bump_problems((1, 0), {"a": "h1"}, (2, 0), {"a": "h2"}))

    def test_unchanged_needs_nothing(self):
        self.assertFalse(g.bump_problems((1, 0), {"a": "a"}, (1, 0), {"a": "a"}))


class RealTreeTest(unittest.TestCase):
    root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))

    def test_committed_table_matches_headers(self):
        exports, table = g.generate(self.root)
        with open(os.path.join(self.root, g.TABLE_REL), encoding="utf-8") as f:
            self.assertEqual(f.read(), table)
        names = [e[0] for e in exports]
        self.assertEqual(names, sorted(set(names)))

    def test_forbidden_never_exported(self):
        names = set(e[0] for e in g.generate(self.root)[0])
        for bad in ("lichen_sys_ui_lock", "esp_restart", "esp_rom_printf", "printf", "console_register_dev", "console_exec",
                    "app_settings_erase_all", "settings_erase", "clock_bind_store", "clock_set_manual", "lora_set_link_callback",
                    "lora_set_rx_formatter", "lichen_crashlog_delete", "mem_book_set_bulk_quota", "test_card_set_hooks",
                    "app_ctx_add", "bench_scope_bind", "system_restart_set_hook"):
            self.assertNotIn(bad, names)


if __name__ == "__main__":
    unittest.main()
