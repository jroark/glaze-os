#!/usr/bin/env python3
"""Service catalog build-time validation; malformed policy must not ship."""
import importlib.util
import json
import struct
import unittest
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('catalog', ROOT / 'tools/mkservices.py')
catalog = importlib.util.module_from_spec(spec)
spec.loader.exec_module(catalog)


class CatalogTest(unittest.TestCase):
    def test_shipped_catalog(self):
        data = catalog.build(json.loads((ROOT / 'config/services.json').read_text()))
        self.assertEqual(struct.unpack_from('<III', data), (0x475a5356, 1, 4))
        self.assertEqual(len(data), 12 + 4 * 76)
        self.assertEqual(struct.unpack_from('<III', data, 12 + 76 + 64), (4, 1, 1))
        self.assertEqual(struct.unpack_from('<III', data, 12 + 3 * 76 + 64), (65, 0, 1))

    def test_bad_catalogs(self):
        cases = [[], [{'name': 'x', 'path': '/bad'}],
                 [{'name': 'x', 'path': 'bin/../../bad'}],
                 [{'name': 'x', 'path': 'bin/x', 'after': ['x']}],
                 [{'name': 'x', 'path': 'bin/x', 'after': ['y']},
                  {'name': 'y', 'path': 'bin/y', 'after': ['x']}],
                 [{'name': 'x', 'path': 'bin/x', 'privileges': ['network']}],
                 [{'name': 'x', 'path': 'bin/x', 'requires': ['unknown']}],
                 [{'name': 'x', 'path': 'bin/x', 'privileges': ['unknown']}],
                 [{'name': 'x', 'path': 'bin/x'}] * 2]
        for entries in cases:
            with self.subTest(entries=entries), self.assertRaises((ValueError, KeyError)):
                catalog.build(entries)


if __name__ == '__main__':
    unittest.main()
