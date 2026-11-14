#!/usr/bin/env python3
"""Host test of tools/check_console_rules.py (#464): the row check fires on an open transmitting row and on every table
shape it cannot read, and the registration tie fires on a wrong table, a plain register and a double register; each
has a compliant twin that stays quiet. Run with python3 -I."""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import check_console_rules as ccr  # noqa: E402

ROW_OK = '{"x", CON_MATCH_EXACT, CON_GATE_OPEN, CON_ORIGIN_UART, CON_F_TX}'


def table(decl, rows):
    return "%s = {\n    %s,\n    CONSOLE_SUB_END,\n};\n" % (decl, ",\n    ".join(rows))


PIN = """
static rules_of_t const RULES[] = {
    {"sd", SD_RULES},
    {"mesh", MESH_SUBS},
};
"""

MESH = """
mesh_con_t const MESH_CONSOLE_CMDS[MESH_CON_COUNT] = {
    [MESH_CON_MESH] = {"mesh", "mesh | mesh start", MESH_SUBS},
    [MESH_CON_MSGS] = {"msgs", "msgs [n]", NULL},
};
"""

LOOP = """
void mesh_console_init(void) {
    for (mesh_con_run_t const &c : MESH_CON_RUN) {
        console_register_app_rules("meshcore", MESH_CONSOLE_CMDS[c.id].name, MESH_CONSOLE_CMDS[c.id].usage, c.run,
                                   MESH_CONSOLE_CMDS[c.id].rules);
    }
}
"""


class Rows(unittest.TestCase):
    def problems(self, text, allow=frozenset()):
        return ccr.check_rows([("f.h", text)], set(allow))[0]

    def test_open_tx_row_fails_unless_allowed(self):
        text = table("static console_sub_t const T[]", ['{"go", CON_MATCH_EXACT, CON_GATE_OPEN, CON_ORIGIN_ANY, CON_F_TX}'])
        self.assertEqual(len(self.problems(text)), 1)
        self.assertEqual(self.problems(text, {("T", "go")}), [])

    def test_a_ruled_row_passes(self):
        self.assertEqual(self.problems(table("static console_sub_t const T[]", [ROW_OK])), [])

    def test_both_qualifier_orders_are_read(self):
        row = '{NULL, CON_MATCH_REST, CON_GATE_OPEN, CON_ORIGIN_ANY, CON_F_RESTART}'
        for decl in ("static const console_sub_t T[]", "static console_sub_t const T[]", "const console_sub_t T[2]",
                     "mesh_sub_t const T[N + 1]"):
            self.assertEqual(len(self.problems(table(decl, [row]))), 1, decl)  # read, so the REST row is seen

    def test_unread_declaration_fails(self):
        text = "static console_sub_t const *const PTRS[] = {T_RULES};\n"  # an array of the type the parser does not read
        self.assertIn("cannot read", self.problems(text)[0])

    def test_macro_built_row_fails(self):
        text = "#define ROW(w) {w, CON_MATCH_EXACT, CON_GATE_OPEN, CON_ORIGIN_ANY, 0}\n" + table(
            "static console_sub_t const T[]", ['ROW_TX("x")'])
        self.assertIn("cannot read", self.problems(text)[0])

    def test_flag_from_another_header_fails(self):
        text = table("static console_sub_t const T[]", ['{"x", CON_MATCH_EXACT, CON_GATE_OPEN, CON_ORIGIN_ANY, MY_TX}'])
        self.assertIn("flag", self.problems(text)[0])

    def test_unknown_origin_fails(self):
        text = table("static console_sub_t const T[]", ['{"x", CON_MATCH_EXACT, CON_GATE_OPEN, ORIGIN_X, 0}'])
        self.assertIn("origin", self.problems(text)[0])

    def test_missing_end_fails(self):
        text = 'static console_sub_t const T[] = {{"x", CON_MATCH_EXACT, CON_GATE_OPEN, CON_ORIGIN_ANY, 0}};\n'
        self.assertIn(ccr.END, self.problems(text)[0])

    def test_local_defines_and_dev_word_are_expanded(self):
        text = "#define OPEN CON_GATE_OPEN, CON_ORIGIN_ANY\n" + table(
            "static console_sub_t const T[]", ['{CON_DEV_WORD("w"), CON_MATCH_EXACT, OPEN, CON_F_TX}'])
        self.assertEqual(self.problems(text, {("T", "w")}), [])

    def test_stale_allow_entry_fails(self):
        self.assertIn("matches no", self.problems(table("static console_sub_t const T[]", [ROW_OK]), {("T", "x")})[0])


class Tie(unittest.TestCase):
    def tie(self, register):
        return ccr.check_tie([("sd.c", register), ("mesh_cmd_table.c", MESH), ("mesh_cmd.cpp", LOOP)], PIN)

    def test_matching_registration_passes(self):
        self.assertEqual(self.tie('console_register_rules("sd", "sd", cmd_sd, SD_RULES);\n'), [])

    def test_wrong_table_fails(self):
        problems = self.tie('console_register_rules("sd", "sd", cmd_sd, CRASH_RULES);\n')
        self.assertTrue(any("CRASH_RULES" in p for p in problems), problems)
        self.assertTrue(any("pin tests \"sd\" with SD_RULES" in p for p in problems), problems)

    def test_plain_register_fails(self):
        problems = self.tie('console_register("sd", "sd", cmd_sd);\n')
        self.assertEqual(len(problems), 1, problems)
        self.assertIn("without a table", problems[0])

    def test_registered_both_ways_fails(self):
        problems = self.tie('console_register_rules("sd", "sd", cmd_sd, SD_RULES);\nconsole_register("sd", "u", f);\n')
        self.assertTrue(any("both" in p for p in problems), problems)

    def test_unresolvable_table_fails(self):
        problems = self.tie('console_register_rules("sd", "sd", cmd_sd, pick_rules());\n')
        self.assertTrue(any("cannot resolve" in p for p in problems), problems)

    def test_definitions_are_not_calls(self):
        text = ('void console_register_rules(char const *name, char const *usage, console_fn_t fn, '
                'console_sub_t const *rules) {\n}\nconsole_register_rules("sd", "sd", cmd_sd, SD_RULES);\n')
        self.assertEqual(self.tie(text), [])


class Firmware(unittest.TestCase):
    def test_the_tree_passes(self):
        srcs = list(ccr.sources())
        self.assertEqual(ccr.check_rows(srcs, ccr.ALLOW)[0], [])
        self.assertEqual(ccr.check_tie(srcs, ccr.PIN.read_text()), [])


if __name__ == "__main__":
    unittest.main()
