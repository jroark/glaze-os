"""Offline maintenance orchestration and signed resource validation."""
import contextlib
import hashlib
import json
import os
from pathlib import Path
import random
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / 'installer'), str(ROOT / 'tools'), str(ROOT / 'tests')]
from engine import Installer, version_key
from app_selection import archive
import package
import mkota
from ota_test import kernel_elf, romfs


class Maintenance(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.events = []
        self.i = Installer(self.root, self.events.append)
        self.i.maintenance_dir = self.root
        self.i.manifest = {'os': {'version': '1.1', 'build': 'release'}}

    def test_versions(self):
        self.assertEqual(version_key('1'), version_key('1.0'))
        self.assertGreater(version_key('1.10'), version_key('1.9'))

    def test_stale_response_ignored(self):
        calls = []
        def run(action, *args):
            calls.append(action)
            if action == 'get-result':
                operation = (self.root / 'request').read_text().split()[0]
                Path(args[0]).write_text(json.dumps({'id': 'stale' if calls.count(action) == 1 else operation, 'code': 0}))
        self.i.run = run
        with patch('engine.time.sleep'):
            self.assertEqual(self.i.maintenance_request('info')['code'], 0)
        self.assertEqual(calls.count('get-result'), 2)

    def test_device_error_not_retried(self):
        def run(action, *args):
            if action == 'get-result':
                operation = (self.root / 'request').read_text().split()[0]
                Path(args[0]).write_text(json.dumps({'id': operation, 'code': -1}))
        self.i.run = run
        with self.assertRaisesRegex(ValueError, 'Device install failed'):
            self.i.maintenance_request('install')

    def test_arming_prevents_quit_request(self):
        def run(action, *args):
            if action == 'get-result':
                operation = (self.root / 'request').read_text().split()[0]
                Path(args[0]).write_text(json.dumps({'id': operation, 'code': 0, 'phase': 'arming'}))
        self.i.run = run
        self.i.maintenance_request('update')
        self.assertTrue(self.i.maintenance_armed)

    def session(self):
        return contextlib.nullcontext()

    def test_non_glaze_and_installer_ram_session_rejected(self):
        self.i.verify = lambda: None
        self.i.run = lambda *args: self.fail('must not upload')
        for runtime, caps in [('CE', 0), ('Glaze', 32771), ('Glaze', 0)]:
            self.i.probe = lambda: {'runtime': runtime, 'capabilities': caps}
            with self.assertRaises(ValueError), self.i.maintenance_session():
                self.fail('must not enter maintenance')

    def test_session_cleanup_and_armed_reset(self):
        self.i.verify = lambda: None
        self.i.probe = lambda: {'runtime': 'Glaze', 'capabilities': 3}
        self.i.run = lambda *args: None
        requests = []
        self.i.maintenance_request = lambda action, **kw: requests.append(action)
        with self.i.maintenance_session():
            pass
        self.assertEqual(requests, ['quit'])
        requests.clear()
        with self.i.maintenance_session():
            self.i.maintenance_armed = True
        self.assertEqual(requests, [])

    def test_response_timeout(self):
        self.i.run = lambda *args: None
        with patch('engine.time.monotonic', side_effect=[0, 121]):
            with self.assertRaisesRegex(RuntimeError, 'timed out'):
                self.i.maintenance_request('info')

    def test_unsupported_ota_no_upload(self):
        self.i.maintenance_session = self.session
        self.i.maintenance_request = lambda action: {'flags': 0}
        self.i.run = lambda *args: self.fail('must not upload')
        with self.assertRaisesRegex(ValueError, 'does not support'):
            self.i.update_os()

    def test_equal_or_newer_no_upload(self):
        self.i.maintenance_session = self.session
        self.i.run = lambda *args: self.fail('must not upload')
        for version in ('1.1', '1.2'):
            self.i.maintenance_request = lambda action: {'flags': 1, 'version': version}
            with self.assertRaisesRegex(ValueError, 'already installed'):
                self.i.update_os()

    def test_low_space_no_upload(self):
        self.i.maintenance_session = self.session
        self.i.maintenance_request = lambda action: {'flags': 1, 'version': '1.0', 'free': 0}
        (self.root / 'update.gzu').write_bytes(b'update')
        self.i.run = lambda *args: self.fail('must not upload')
        with self.assertRaisesRegex(ValueError, 'space'):
            self.i.update_os()

    def test_apps_continue_after_individual_failure(self):
        self.i.maintenance_session = self.session
        catalog = [{'id': 'package:'+x, 'program': x, 'name': x, 'kind': 'package', 'default': False} for x in ('a', 'b')]
        (self.root / 'packages').mkdir()
        for name in ('a', 'b'):
            data = bytearray(archive({'Info': f'id={name}\nversion=1.0\nexec=app\n'.encode(), 'app': b'elf'}))
            data[:4] = b'GZPK'
            (self.root / 'packages' / (name + '.gzp')).write_bytes(data)
        requests = []
        def request(action):
            requests.append(action)
            if action == 'info':
                return {}
            if requests.count('install') == 1:
                raise ValueError('no space')
            return {'code': 0}
        self.i.maintenance_request = request
        self.i.run = lambda *args: None
        with patch('engine.load_catalog', return_value=catalog):
            with self.assertRaisesRegex(RuntimeError, 'Some apps'):
                self.i.install_apps(['package:a', 'package:b'])
        self.assertEqual(requests.count('install'), 2)
        self.assertFalse(any(e['event'] == 'success' for e in self.events))

    def test_confirmed_os_success(self):
        self.i.maintenance_session = self.session
        infos = iter([{'flags': 1, 'version': '1.0', 'free': 1000000},
                      {'version': '1.1', 'build': 'release', 'last_result': 1}])
        self.i.maintenance_request = lambda action, **kw: next(infos) if action == 'info' else {'phase': 'arming'}
        self.i.run = lambda *args: None
        (self.root / 'update.gzu').write_bytes(b'update')
        with patch('engine.time.sleep'):
            self.i.update_os()
        self.assertEqual(self.events[-1]['event'], 'success')

    def test_rejected_update_no_success(self):
        self.i.maintenance_session = self.session
        infos = iter([{'flags': 1, 'version': '1.0', 'free': 1000000},
                      {'version': '1.0', 'build': 'old', 'last_result': 2}])
        self.i.maintenance_request = lambda action, **kw: next(infos) if action == 'info' else {'phase': 'arming'}
        self.i.run = lambda *args: None
        (self.root / 'update.gzu').write_bytes(b'update')
        with patch('engine.time.sleep'), self.assertRaisesRegex(ValueError, 'SPL rejected'):
            self.i.update_os()


class SignedPayload(unittest.TestCase):
    def test_matching_signature_and_payload_required(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'config').mkdir()
            (root / 'out').mkdir()
            key = root / 'key.pem'
            subprocess.run([mkota.openssl(), 'genpkey', '-algorithm', 'ED25519', '-out', str(key)], check=True, capture_output=True)
            (root / 'config/ota_pubkey.bin').write_bytes(mkota.public_key(key))
            rng = random.Random(4)
            kernel, fs = kernel_elf(rng, 16384), romfs(rng, [4096])
            data = mkota.build_update(kernel, fs, '1.1', 'release', key)
            (root / 'VERSION').write_text('1.1\n')
            payload = mkota.payload(kernel, fs, '1.1', 'release')
            count = struct.unpack_from('<I', data, 16)[0]
            payload = payload.ljust(count * 16384, b'\xff')
            (root / 'out/glaze.bin').write_bytes(b'\xff' * 0x14000 + payload)
            update = root / 'update.gzu'
            update.write_bytes(data)
            with patch.object(package, 'ROOT', root):
                self.assertEqual(package.validate_update(update, 'release')['version'], '1.1')
                with self.assertRaises(ValueError):
                    package.validate_update(update, 'wrong-build')
                update.write_bytes(data[:-1] + bytes([data[-1] ^ 1]))
                with self.assertRaises(ValueError):
                    package.validate_update(update, 'release')
                update.write_bytes(data[:448] + bytes(64) + data[512:])
                with self.assertRaises(subprocess.CalledProcessError):
                    package.validate_update(update, 'release')
                update.write_bytes(data)
                (root / 'out/glaze.bin').write_bytes(bytes(0x14000 + len(payload)))
                with self.assertRaisesRegex(ValueError, 'differs'):
                    package.validate_update(update, 'release')

if __name__ == '__main__':
    unittest.main()
