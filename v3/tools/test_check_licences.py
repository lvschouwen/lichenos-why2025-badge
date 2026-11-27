"""Host tests for check_licences.py: SPDX evaluation."""
import unittest

import check_licences as cl


class SpdxTests(unittest.TestCase):
    def test_simple(self):
        self.assertTrue(cl.allowed("MIT"))
        self.assertTrue(cl.allowed("Apache-2.0"))
        self.assertFalse(cl.allowed("GPL-3.0-only"))
        self.assertFalse(cl.allowed(""))

    def test_or_needs_one_and_needs_all(self):
        self.assertTrue(cl.allowed("MIT OR Apache-2.0"))
        self.assertTrue(cl.allowed("MIT OR GPL-3.0-only"))
        self.assertFalse(cl.allowed("GPL-2.0-only OR GPL-3.0-only"))
        self.assertTrue(cl.allowed("MIT AND Apache-2.0"))
        self.assertFalse(cl.allowed("MIT AND Unicode-3.0"))
        self.assertTrue(cl.allowed("(MIT OR Apache-2.0) AND (MIT OR BSD-3-Clause)"))
        self.assertFalse(cl.allowed("(MIT OR Apache-2.0) AND Unicode-3.0"))

    def test_legacy_slash_and_with(self):
        self.assertTrue(cl.allowed("MIT/Apache-2.0"))
        self.assertTrue(cl.allowed("Apache-2.0 WITH LLVM-exception"))
        self.assertFalse(cl.allowed("GPL-2.0-only WITH Classpath-exception-2.0"))

    def test_garbage_raises(self):
        with self.assertRaises((ValueError, IndexError)):
            cl.allowed("(MIT OR")

    def test_findings_list_missing_and_bad(self):
        meta = {"packages": [
            {"name": "a", "version": "1", "license": "MIT"},
            {"name": "b", "version": "1", "license": None},
            {"name": "c", "version": "2", "license": "GPL-3.0-only"},
        ]}
        self.assertEqual(cl.findings(meta), ["b 1: no license field", "c 2: GPL-3.0-only"])


if __name__ == "__main__":
    unittest.main()
