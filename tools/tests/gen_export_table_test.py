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

    def test_spelling_pairs(self):
        self.assertEqual(self.h("char *f(char const *p)"), self.h("char* f(char const* p)"))
        self.assertEqual(self.h("void f(const char* p)"), self.h("void f(char const* p)"))
        self.assertEqual(self.h("void f(unsigned a)"), self.h("void f(unsigned int b)"))
        self.assertEqual(self.h("void f(long a)"), self.h("void f(long int b)"))
        self.assertEqual(self.h("void f(void (*cb)(int))"), self.h("void f(void (*fn)(int))"))
        self.assertEqual(self.h("void f(void (*cb)(int a, char const *s))"), self.h("void f(void (*fn)(int, const char *))"))
        self.assertEqual(self.h("void f(char **argv)"), self.h("void f(char * * v)"))

    def test_real_differences(self):
        self.assertNotEqual(self.h("void f(char const *p)"), self.h("void f(char *p)"))
        self.assertNotEqual(self.h("void f(unsigned a)"), self.h("void f(signed a)"))
        self.assertNotEqual(self.h("void f(void (*cb)(int))"), self.h("void f(void (*cb)(long))"))
        self.assertNotEqual(self.h("void f(char *const p)"), self.h("void f(char const *p)"))
        self.assertNotEqual(self.h("void f(unsigned a)"), self.h("void f(unsigned long a)"))

    def test_unnamed_and_void(self):
        self.assertEqual(self.h("void f(void)"), self.h("void f( void )"))
        self.assertNotEqual(self.h("void f(int)"), self.h("void f(long)"))
        self.assertEqual(self.h("void f(unsigned)"), self.h("void f(unsigned)"))
        self.assertNotEqual(self.h("void f(struct a x)"), self.h("void f(struct b x)"))


class DeclTest(unittest.TestCase):
    d = staticmethod(g.declarations)
    BASE = ("typedef uint32_t c_t;\n#define N 24\ntypedef struct { int a; char b; } s_t;\n"
            "typedef enum { A, B = 5, C } e_t;\nextern int v;\ntypedef void (*cb_t)(int);\n")

    def test_per_declaration_names(self):
        got = self.d(self.BASE)
        self.assertEqual(sorted(got), ["A", "B", "C", "N", "c_t", "cb_t", "e_t", "s_t", "v"])

    def test_functions_comments_and_formatting_do_not_count(self):
        more = ("// c\ntypedef  uint32_t   c_t; /* x */\n#define N   24\ntypedef struct {int a;char b;} s_t;\n"
                "typedef enum { A,\n B=5, C } e_t;\nextern int v;\ntypedef void (*cb_t)(int);\nint g(void);\n"
                "static inline int h(void) { return 1; }\n")
        self.assertEqual(self.d(self.BASE), self.d(more))

    def test_cplusplus_guard_is_ignored(self):
        self.assertEqual(self.d('#ifdef __cplusplus\nextern "C" {\n#endif\ntypedef int t;\n#ifdef __cplusplus\n}\n#endif\n'),
                         self.d("typedef int t;\n"))

    def test_changes_change_the_named_hash(self):
        base = self.d(self.BASE)
        for old, new, name in (("uint32_t c_t", "uint64_t c_t", "c_t"), ("N 24", "N 32", "N"), ("char b;", "short b;", "s_t"),
                               ("B = 5", "B = 6", "B"), ("B = 5, C", "B = 5, C = 9", "C"), ("extern int v", "extern long v", "v"),
                               ("(*cb_t)(int)", "(*cb_t)(long)", "cb_t")):
            got = self.d(self.BASE.replace(old, new))
            self.assertNotEqual(base[name], got[name], new)
            self.assertEqual({k: v for k, v in base.items() if k != name and not (name == "B" and k == "C")},
                             {k: v for k, v in got.items() if k != name and not (name == "B" and k == "C")}, new)

    def test_enum_values_are_their_own_names(self):
        base = self.d(self.BASE)
        more = self.d(self.BASE.replace("C }", "C, D }"))
        self.assertEqual(base["e_t"], more["e_t"])  # the enum type does not hash its values
        self.assertEqual(sorted(set(more) - set(base)), ["D"])
        moved = self.d(self.BASE.replace("{ A, B = 5, C }", "{ Z, A, B = 5, C }"))
        self.assertNotEqual(base["A"], moved["A"])  # an inserted value moves the implicit ones after it

    def test_tagged_struct_and_removed_name(self):
        got = self.d("struct foo { int a; };\nstruct foo *make(void);\n")
        self.assertEqual(sorted(got), ["struct foo"])


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
        t = g.table_text([("a", "a", "x.h", "h1")], ["esp_restart"], (1, 2), {"N": ("x.h", "t1")})
        self.assertEqual(g.parse_table(t), ((1, 2), {"a": "h1"}, ["esp_restart"], {"N": "t1"}))


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

    def test_adding_a_type_or_macro_is_minor(self):
        self.assertTrue(g.bump_problems((1, 0), {"a": "h"}, (1, 0), {"a": "h"}, {"N": "1"}, {"N": "1", "M": "2"}))
        self.assertFalse(g.bump_problems((1, 0), {"a": "h"}, (1, 1), {"a": "h"}, {"N": "1"}, {"N": "1", "M": "2"}))

    def test_changed_or_removed_type_is_major(self):
        for new_t in ({"N": "2"}, {}):
            self.assertTrue(g.bump_problems((1, 0), {"a": "h"}, (1, 1), {"a": "h"}, {"N": "1"}, new_t))
            self.assertFalse(g.bump_problems((1, 0), {"a": "h"}, (2, 0), {"a": "h"}, {"N": "1"}, new_t))

    def test_the_capability_count_is_no_api(self):
        # #562: a new capability moves CAP_COUNT, which no app uses (it names caps in its manifest)
        self.assertFalse(g.bump_problems((2, 2), {"a": "h"}, (2, 3), {"a": "h"}, {"CAP_COUNT": "1", "X": "1"}, {"X": "1"}))
        self.assertFalse(g.bump_problems((2, 2), {"a": "h"}, (2, 2), {"a": "h"}, {"CAP_COUNT": "1"}, {"CAP_COUNT": "2"}))

    def test_unchanged_needs_nothing(self):
        self.assertFalse(g.bump_problems((1, 0), {"a": "a"}, (1, 0), {"a": "a"}))


class ElfTest(unittest.TestCase):
    def test_missing_elf_for_an_app_dir(self):
        with tempfile.TemporaryDirectory() as src:
            for d in ("hello", "doom"):
                os.makedirs(os.path.join(src, d))
            old = g.undefined_symbols
            g.undefined_symbols = lambda p: ["lichen_notify_dnd"]
            try:
                got = g.elf_problems(["/x/hello/app.elf"], [("lichen_notify_dnd", "", "", "")], src)
                self.assertEqual(got, ["sdapps/doom has no built app.elf: nothing checked its symbols"])
                self.assertIn("lichen_notify_dnd", g.elf_problems(["/x/hello/app.elf"], [], None)[0])
            finally:
                g.undefined_symbols = old

    def test_missing_nm_is_a_clear_error(self):
        old = os.environ.get("NM")
        os.environ["NM"] = "/nonexistent/nm"
        try:
            with self.assertRaises(g.ExportError):
                g.undefined_symbols("x.elf")
        finally:
            os.environ.pop("NM") if old is None else os.environ.__setitem__("NM", old)


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
        for bad in ("lichen_register_app", "lichen_sys_ui_lock", "esp_restart", "esp_rom_printf", "printf", "console_register_dev", "console_exec",
                    "app_settings_erase_all", "settings_erase", "clock_bind_store", "clock_set_manual", "lora_set_link_callback",
                    "lora_set_rx_formatter", "lichen_crashlog_delete", "mem_book_set_bulk_quota", "test_card_set_hooks",
                    "app_ctx_add", "bench_scope_bind", "system_restart_set_hook"):
            self.assertNotIn(bad, names)


if __name__ == "__main__":
    unittest.main()
