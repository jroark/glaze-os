"""Synthetic package tests for the disposable WinCE emulator harness."""
import struct
import tempfile
import unittest
from pathlib import Path
from run_wince import read_package, read_registry, runtime_failures
from wince_assets import encode_gzce_registry


def archive(entries):
    table, payload = bytearray(), bytearray()
    offset = 8 + 56 * len(entries)
    for name, data in entries:
        table += struct.pack('<48sII', name.encode(), offset + len(payload), len(data))
        payload += data
    return b'GZPK' + struct.pack('<I', len(entries)) + table + payload


class PackageTests(unittest.TestCase):
    def read(self, data):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'app.gzp'
            path.write_bytes(data)
            return read_package(path)

    def entries(self, info=b'id=cbea.test\nname=Test\nexec=runner\n'):
        return [('Info', info), ('runner', b'old runtime'), ('res/program.exe', b'original')]

    def test_metadata_and_payload(self):
        app_id, executable, entries = self.read(archive(self.entries()))
        self.assertEqual((app_id, executable), ('cbea.test', 'runner'))
        self.assertEqual(entries['res/program.exe'], b'original')

    def test_paths_and_duplicates(self):
        for name in ('../escape', '/escape', 'res/../../escape', 'data/value',
                     '.pkg-ready', 'res//file', 'res/./file', 'res\\file', 'Info'):
            with self.subTest(name=name), self.assertRaises(ValueError):
                self.read(archive(self.entries() + [(name, b'x')]))

    def test_metadata(self):
        for info in (b'id=../bad\nname=X\n', b'id=cbea.test\n',
                     b'id=cbea.test\nname=X\nexec=missing\n', b'id=-bad\nname=X\n'):
            with self.subTest(info=info), self.assertRaises(ValueError):
                self.read(archive(self.entries(info)))

    def test_bounds(self):
        valid = archive(self.entries())
        for data in (b'', b'GZPK', b'BAD!' + valid[4:], valid[:12], valid[:-1],
                     b'GZPK' + struct.pack('<I', 65)):
            with self.subTest(size=len(data)), self.assertRaises(ValueError):
                self.read(data)
        bad = bytearray(valid)
        struct.pack_into('<I', bad, 8 + 48, 0)
        with self.assertRaises(ValueError):
            self.read(bad)


class RuntimeTests(unittest.TestCase):
    def test_unexpected_exit(self):
        abort = 'proc: cbea.aim pid 164 exited (255)'
        self.assertEqual(runtime_failures([abort], 'cbea.aim'), [abort])
        self.assertFalse(runtime_failures([abort], 'cbea.aim', 255))
        self.assertFalse(runtime_failures(['proc: other pid 164 exited (255)'], 'cbea.aim'))
        self.assertTrue(runtime_failures(['fault: Address load in cbea.aim'], 'cbea.aim', 255))


class RegistryTests(unittest.TestCase):
    def test_values_and_empty_keys(self):
        blob = encode_gzce_registry([{'key': r'HKLM\Software\Test', 'name': 'Greeting', 'type': 1,
                                     'data': 'Hello\0'.encode('utf-16le')}], [r'HKCU\Empty'])
        keys, values = read_registry(blob)
        self.assertIn(r'hkcu\empty', keys)
        self.assertEqual(values[r'hklm\software\test', 'greeting'], (1, 'Hello\0'.encode('utf-16le')))
        marker = blob.index(b'GZCK')
        self.assertEqual(read_registry(blob[:marker])[1], values)

    def test_truncated(self):
        blob = encode_gzce_registry([{'key': r'HKCU\Test', 'name': 'x', 'type': 4, 'data': b'1234'}])
        marker = blob.index(b'GZCK')
        for end in range(len(blob)):
            if end == marker:
                continue
            with self.subTest(end=end), self.assertRaises((ValueError, struct.error)):
                read_registry(blob[:end])


if __name__ == '__main__':
    unittest.main()
