#!/usr/bin/env python3
"""Host test of the SD app import gate (#550): the table and readelf parsing and the miss list. Run with python3 -I."""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import check_sdapp_imports as c  # noqa: E402

TABLE = "# comment\nexport\tmemcpy\tmemcpy\tallow\t1234\nexport\tlv_tick_get\tlv_tick_get\tallow\t5678\ntype\tkey_matrix.h\tX\tabcd\n"
READELF = """Symbol table '.dynsym' contains 4 entries:
   Num:    Value  Size Type    Bind   Vis      Ndx Name
     0: 00000000     0 NOTYPE  LOCAL  DEFAULT  UND 
     1: 00000000     0 NOTYPE  GLOBAL DEFAULT  UND memcpy
     2: 00000000     0 NOTYPE  GLOBAL DEFAULT  UND xTaskGetTickCount
     3: 00001000    20 FUNC    GLOBAL DEFAULT    7 main
"""


class ImportsTest(unittest.TestCase):
    def test_table_names(self):
        self.assertEqual(c.exported_names(TABLE), {"memcpy", "lv_tick_get"})

    def test_undefined_only(self):
        self.assertEqual(c.undefined_symbols(READELF), ["memcpy", "xTaskGetTickCount"])

    def test_missing(self):
        self.assertEqual(c.missing_imports(READELF, TABLE), ["xTaskGetTickCount"])
        self.assertEqual(c.missing_imports(READELF, TABLE + "export\txTaskGetTickCount\tx\tallow\t1\n"), [])


if __name__ == "__main__":
    unittest.main()
