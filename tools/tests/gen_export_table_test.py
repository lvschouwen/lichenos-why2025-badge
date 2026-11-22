#!/usr/bin/env python3
"""Host test of the export table generator (#519): the allow-list syntax, the forbid and wrap rules, the version-bump
rule and the app-symbol check. Run with python3 -I."""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import gen_export_table as g  # noqa: E402


class AllowTest(unittest.TestCase):
    def test_syntax(self):
        self.assertEqual(g.parse_allow("memcpy # c\n\nmalloc = lichen_exp_malloc\n"), [("memcpy", None), ("malloc", "lichen_exp_malloc")])
        self.assertEqual(g.parse_allow("!bench_x_bind"), [("!bench_x_bind", None)])

    def test_bad_line(self):
        for bad in ("a b", "a = b = c", "1x", "!a = b"):
            with self.assertRaises(g.ExportError):
                g.parse_allow(bad)


class TableTest(unittest.TestCase):
    def test_c_text_is_sorted_and_terminated(self):
        c = g.c_text([("a", "a", "x.h"), ("b", "__wrap_b", "x.h")])
        self.assertIn('{"a", (void *)a},\n    {"b", (void *)__wrap_b},\n    {NULL, NULL}', c)
        self.assertIn("extern void __wrap_b(void);", c)

    def test_parse_round_trip(self):
        t = g.table_text(None, [("a", "a", "x.h")], ["esp_restart"], (1, 2))
        self.assertEqual(g.parse_table(t), ((1, 2), {"a": "a"}, ["esp_restart"]))


class VersionTest(unittest.TestCase):

    def test_added_needs_minor(self):
        self.assertTrue(g.bump_problems((1, 0), {"a": "a"}, (1, 0), {"a": "a", "b": "b"}))
        self.assertFalse(g.bump_problems((1, 0), {"a": "a"}, (1, 1), {"a": "a", "b": "b"}))

    def test_removed_or_changed_needs_major(self):
        self.assertTrue(g.bump_problems((1, 3), {"a": "a", "b": "b"}, (1, 4), {"a": "a"}))
        self.assertTrue(g.bump_problems((1, 3), {"a": "a"}, (1, 4), {"a": "other"}))
        self.assertFalse(g.bump_problems((1, 3), {"a": "a", "b": "b"}, (2, 0), {"a": "a"}))

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
        for bad in ("lichen_sys_ui_lock", "esp_restart", "esp_rom_printf", "printf"):
            self.assertNotIn(bad, names)


if __name__ == "__main__":
    unittest.main()
