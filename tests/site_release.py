"""Release packaging, private-data exclusions and publishing safeguards."""
import hashlib
import importlib.util
import json
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch
from zipfile import ZipFile

ROOT = Path(__file__).resolve().parent.parent


def module(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'tools' / (name + '.py'))
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


packager = module('package_glazeboot')
publisher = module('publish_site')
REVISION = 'e4500c1204252cdf37bc8dfd86e8e678bc7b641a'


FIRMWARE = {'firmware/cf8385.bin': b'approved main fixture',
            'firmware/cf8385_helper.bin': b'approved helper fixture'}


def romfs(firmware=False, omit=None):
    entries = {'bin/' + name: b'' for name in (*packager.BUILTIN_APPS, 'springboard', 'audio', 'net', 'fs')}
    if firmware:
        entries.update(FIRMWARE)
    entries.pop(omit, None)
    offset = 8 + 56 * len(entries)
    table = b''
    payload = b''
    for name, data in entries.items():
        table += struct.pack('<48sII', name.encode(), offset + len(payload), len(data))
        payload += data
    return b'GZRF' + struct.pack('<I', len(entries)) + table + payload


class SiteRelease(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.build = self.root / 'build'
        self.site = self.root / 'site'
        self.build.mkdir()
        manifest = packager.firmware_manifest(ROOT)
        for name, data in FIRMWARE.items():
            manifest['files']['libertas/' + Path(name).name] = {
                'size_bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
        firmware_patch = patch.object(packager, 'firmware_manifest', return_value=manifest)
        firmware_patch.start()
        self.addCleanup(firmware_patch.stop)
        self.site.mkdir()
        for name, data in {'GlazeBoot.exe': b'MZfixture', 'kernel.bin': REVISION[:7].encode(),
                           'romfs.img': romfs(True), 'glaze.cfg': b'nand=ro\n',
                           'glaze.seed': b'private seed must not ship'}.items():
            (self.build / name).write_bytes(data)
        nand = bytearray(16 * 1024 * 1024)
        archive = romfs()
        nand[0x40000:0x40000 + len(archive)] = archive
        (self.build / 'glaze.bin').write_bytes(nand)
        for name in publisher.REQUIRED:
            path = self.site / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('fixture')

    def package(self):
        return packager.package(self.build, self.site, REVISION, '2026-10-25', ROOT)

    def test_archive_checksums_and_private_seed(self):
        release = self.package()
        archive = self.site / 'downloads' / release['filename']
        publisher.check_site(self.site)
        with ZipFile(archive) as z:
            self.assertIsNone(z.testzip())
            self.assertIn('Glaze/GlazeBoot.exe', z.namelist())
            self.assertIn('Emulator/glaze.bin', z.namelist())
            self.assertIn('licenses/firmware/LICENCE.Marvell', z.namelist())
            self.assertEqual(z.read('licenses/firmware/LICENCE.Marvell'), (ROOT / 'firmware/LICENCE.Marvell').read_bytes())
            self.assertEqual(set(release['builtin_apps']), set(packager.BUILTIN_APPS))
            self.assertTrue(release['vendor_firmware_included'])
            self.assertFalse(release['emulator_firmware_included'])
            self.assertFalse(any('glaze.seed' in name or name.startswith('firmware/') for name in z.namelist()))
            for line in z.read('SHA256SUMS').decode().splitlines():
                digest, name = line.split('  ', 1)
                self.assertEqual(hashlib.sha256(z.read(name)).hexdigest(), digest)
            z.extractall(self.root / 'extract')
        script = self.root / 'extract/prepare-card.py'
        subprocess.run([sys.executable, str(script)], check=True, capture_output=True)
        seed = self.root / 'extract/Glaze/glaze.seed'
        before = seed.read_bytes()
        self.assertEqual(len(before), 32)
        subprocess.run([sys.executable, str(script)], check=True, capture_output=True)
        self.assertEqual(seed.read_bytes(), before)
        archive.write_bytes(archive.read_bytes() + b'changed')
        with self.assertRaisesRegex(SystemExit, 'does not match'):
            publisher.check_site(self.site)

    def test_reject_unapproved_firmware_and_wrong_build(self):
        (self.build / 'romfs.img').write_bytes(romfs())
        with self.assertRaisesRegex(ValueError, 'approved Linux firmware pair'):
            self.package()
        (self.build / 'romfs.img').write_bytes(romfs(True).replace(b'approved main fixture', b'tampered main fixture'))
        with self.assertRaisesRegex(ValueError, 'unapproved firmware bytes'):
            self.package()
        (self.build / 'romfs.img').write_bytes(romfs(True))
        nand = bytearray((self.build / 'glaze.bin').read_bytes())
        image = romfs(True)
        nand[0x40000:0x40000 + len(image)] = image
        (self.build / 'glaze.bin').write_bytes(nand)
        with self.assertRaisesRegex(ValueError, 'NAND image contains restricted firmware'):
            self.package()
        (self.build / 'kernel.bin').write_bytes(b'wrong revision')
        with self.assertRaisesRegex(ValueError, 'requested revision'):
            self.package()

    def test_all_builtin_apps_are_required(self):
        for name in packager.BUILTIN_APPS:
            with self.subTest(app=name):
                (self.build / 'romfs.img').write_bytes(romfs(True, 'bin/' + name))
                with self.assertRaisesRegex(ValueError, 'missing a required application'):
                    self.package()

    def test_missing_download_fails_before_publish(self):
        release = self.package()
        (self.site / 'downloads' / release['filename']).unlink()
        with self.assertRaisesRegex(SystemExit, 'missing; build it'):
            publisher.check_site(self.site)

    def test_old_releases_are_preserved_and_html_is_last(self):
        self.package()
        calls = []
        with patch.object(publisher, 'aws', side_effect=lambda *a, **kw: calls.append(a)):
            publisher.sync('profile', self.site, 'bucket', True)
        self.assertEqual(len(calls), 3)
        self.assertIn('downloads/*', calls[0])
        self.assertNotIn('--delete', calls[1])
        self.assertIn(str(self.site / 'downloads'), calls[1])
        self.assertIn('*.html', calls[-1])
        self.assertTrue(all('--dryrun' in call for call in calls))


if __name__ == '__main__':
    unittest.main()
