"""App selection changes the flashed contents, while preserving boot components."""
from pathlib import Path
import json
import struct
import sys
import tempfile
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'installer'))
from app_selection import archive, entries, customize_logical, install_packages, select

CATALOG = [
    {'id': 'builtin:files', 'name': 'Files', 'kind': 'builtin', 'program': 'files', 'required': True, 'default': True},
    {'id': 'builtin:calculator', 'name': 'Calculator', 'kind': 'builtin', 'program': 'calculator', 'default': True},
    {'id': 'package:hello', 'name': 'Hello', 'kind': 'package', 'program': 'hello', 'default': False},
]


class AppSelectionTest(unittest.TestCase):
    def test_defaults_and_required_apps(self):
        self.assertEqual(select(CATALOG), ['builtin:calculator', 'builtin:files'])
        self.assertEqual(select(CATALOG, []), ['builtin:files'])
        with self.assertRaisesRegex(ValueError, 'Unknown'):
            select(CATALOG, ['package:unknown'])

    def logical(self):
        logical = bytearray(b'\xff' * (16 * 1024 * 1024))
        blob = archive({'bin/files': b'files', 'bin/calculator': b'calculator',
                        'bin/fs': b'fs', 'bin/springboard': b'springboard',
                        'lib/libc.so.1': b'library', 'services.bin': b'services'})
        logical[0x40000:0x40000 + len(blob)] = blob
        struct.pack_into('<I', logical, 4 * 16 + 12, len(blob) // 512)
        return logical

    def test_deselection_removes_executable_preserves_services_and_libraries(self):
        logical = self.logical()
        result = customize_logical(logical, CATALOG, select(CATALOG, []))
        size = struct.unpack_from('<I', result, 4 * 16 + 12)[0] * 512
        files = entries(result[0x40000:0x40000 + size], b'GZRF')
        self.assertNotIn('bin/calculator', files)
        for required in ('bin/files', 'bin/fs', 'bin/springboard', 'lib/libc.so.1', 'services.bin'):
            self.assertIn(required, files)
        self.assertEqual(result[0x500000:], logical[0x500000:])
        self.assertEqual(result[0x14000:0x40000], logical[0x14000:0x40000])
        self.assertTrue(all(offset % 4096 == 0 for _, offset, _ in
                            [struct.unpack_from('<48sII', result, 0x40008 + i * 56) for i in range(len(files))]))

    def package(self, names):
        # GZPK uses the same table with four-byte alignment; the test archive
        # remains valid at the stricter 4096-byte alignment too.
        data = bytearray(archive(names))
        data[:4] = b'GZPK'
        return data

    def test_selected_package_is_an_installed_bundle(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            (tmp / 'resources/packages').mkdir(parents=True)
            blob = self.package({'Info': b'id=hello\nname=Hello\nexec=app\n', 'app': b'ELF', 'res/icon.gzi': b'icon'})
            (tmp / 'resources/packages/hello.gzp').write_bytes(blob)
            tree = tmp / 'userdata'
            tree.mkdir()
            install_packages(tmp / 'resources', CATALOG, select(CATALOG), tree)
            self.assertFalse((tree / 'Applications').exists())
            install_packages(tmp / 'resources', CATALOG, select(CATALOG, ['package:hello']), tree)
            bundle = tree / 'Applications/hello.app'
            self.assertEqual((bundle / 'app').read_bytes(), b'ELF')
            self.assertEqual((bundle / 'res/icon.gzi').read_bytes(), b'icon')
            self.assertEqual((bundle / '.install-id').read_text(), '1')

    def test_malformed_archives_are_rejected(self):
        for name in ('../escape', '/absolute', 'res/../../escape', 'C:/escape', 'res\\escape', 'res//file'):
            with self.subTest(name=name), self.assertRaises(ValueError):
                entries(self.package({name: b'bad'}), b'GZPK')
        with self.assertRaises(ValueError):
            entries(b'GZPK' + struct.pack('<I', 257), b'GZPK')
        blob = self.package({'app': b'ELF'})
        struct.pack_into('<I', blob, 8 + 48, 0)
        with self.assertRaises(ValueError):
            entries(blob, b'GZPK')

    def test_all_bundled_packages_fit_and_have_metadata(self):
        resources = Path(__file__).resolve().parents[1] / 'out/installer/resources'
        if not (resources / 'apps.json').exists():
            self.skipTest('requires staged resources')
        catalog = json.loads((resources / 'apps.json').read_text())
        with tempfile.TemporaryDirectory() as tmp:
            install_packages(resources, catalog, select(catalog, [a['id'] for a in catalog]), tmp)
            expected = sum(a['kind'] == 'package' for a in catalog)
            self.assertEqual(len(list((Path(tmp) / 'Applications').glob('*.app'))), expected)
            self.assertLess(sum(p.stat().st_size for p in Path(tmp).rglob('*') if p.is_file()), 4 * 1024 * 1024)


if __name__ == '__main__':
    unittest.main()
