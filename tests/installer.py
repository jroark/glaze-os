"""Wizard orchestration tests: never open USB or program hardware."""
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'installer'))
from engine import Installer, digest


class InstallerTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.events = []
        self.engine = Installer(self.root, self.events.append)

    def record(self):
        names = ['original-ce.raw', 'candidate.raw', 'inventory.json', 'placement.json', 'install.bin', 'review.json']
        for name in names:
            (self.root / name).write_bytes(b'{}')
        record = {'version': 1, 'device': '1.2', 'files': {n: digest(self.root / n) for n in names}}
        path = self.root / 'installation.json'
        path.write_text(json.dumps(record))
        return path

    def test_reconnect_refreshes_stale_usb_configuration(self):
        clock = [0]
        calls = []
        def run(command):
            calls.append(command)
            if command == 'reset-link':
                return []
            if 'reset-link' not in calls:
                raise RuntimeError('USB: LIBUSB_ERROR_OTHER')
            return [{'event': 'device', 'runtime': 'Glaze'}, {'event': 'capabilities', 'value': 4096}]
        with patch('engine.time.monotonic', side_effect=lambda: clock[0]), \
             patch('engine.time.sleep', side_effect=lambda n: clock.__setitem__(0, clock[0] + n)), \
             patch.object(self.engine, 'run', side_effect=run):
            self.engine.wait_glaze(4096)
        self.assertEqual(calls, ['probe', 'probe', 'probe', 'reset-link', 'probe'])

    def test_reconnect_retries_reset_when_usb_was_powered_down(self):
        clock, resets = [0], [0]
        def run(command):
            if command == 'reset-link':
                resets[0] += 1
                if resets[0] == 1:
                    raise RuntimeError('USB not ready')
                return []
            if resets[0] < 2:
                raise RuntimeError('USB: LIBUSB_ERROR_OTHER')
            return [{'event': 'device', 'runtime': 'Glaze'}, {'event': 'capabilities', 'value': 4096}]
        with patch('engine.time.monotonic', side_effect=lambda: clock[0]), \
             patch('engine.time.sleep', side_effect=lambda n: clock.__setitem__(0, clock[0] + n)), \
             patch.object(self.engine, 'run', side_effect=run):
            self.engine.wait_glaze(4096)
        self.assertEqual(resets[0], 2)
        self.assertEqual(clock[0], 9)

    def test_reconnect_stops_at_deadline_with_actionable_error(self):
        clock = [0]
        with patch('engine.time.monotonic', side_effect=lambda: clock[0]), \
             patch('engine.time.sleep', side_effect=lambda n: clock.__setitem__(0, clock[0] + n)), \
             patch.object(self.engine, 'run', side_effect=RuntimeError('unavailable')):
            with self.assertRaisesRegex(RuntimeError, 'Unplug and reconnect USB once'):
                self.engine.wait_glaze(4096)
        self.assertEqual(clock[0], 90)

    def test_connected_kernel_with_missing_capabilities_is_not_reset(self):
        with patch('engine.time.sleep'), patch.object(self.engine, 'run', return_value=[
                {'event': 'device', 'runtime': 'Glaze'}, {'event': 'capabilities', 'value': 0}]) as run:
            with self.assertRaisesRegex(RuntimeError, 'lacks required USB capabilities'):
                self.engine.wait_glaze(4096)
        self.assertEqual(run.call_args_list[0].args, ('probe',))
        self.assertEqual(run.call_count, 1)

    def test_tampered_saved_backup_never_boots_or_writes(self):
        record = self.record()
        (self.root / 'original-ce.raw').write_bytes(b'changed')
        with patch.object(self.engine, 'verify'), patch.object(self.engine, 'run') as run:
            with self.assertRaisesRegex(ValueError, 'differs'):
                self.engine.apply(record)
            run.assert_not_called()

    def test_incomplete_record_never_boots(self):
        record = self.record()
        data = json.loads(record.read_text())
        del data['files']['original-ce.raw']
        record.write_text(json.dumps(data))
        with patch.object(self.engine, 'verify'), patch.object(self.engine, 'run') as run:
            with self.assertRaisesRegex(ValueError, 'incomplete'):
                self.engine.apply(record)
            run.assert_not_called()

    def test_install_order_and_port_binding(self):
        record = self.record()
        self.engine.manifest = {'build_id': 'test'}
        self.engine.build = 'test'
        calls = []
        with patch.object(self.engine, 'verify'), patch.object(self.engine, 'probe'), \
             patch.object(self.engine, 'ram_boot', side_effect=lambda m: calls.append(m)), \
             patch.object(self.engine, 'run', side_effect=lambda *a: calls.append(a[0])), \
             patch.object(self.engine, 'wait_glaze'):
            self.engine.apply(record)
        self.assertEqual(calls, ['repair', 'apply', 'cold-reboot'])
        self.assertEqual(self.engine.device, '1.2')
        self.assertEqual(self.events[-1]['event'], 'success')

    def test_write_failure_never_reboots_or_claims_success(self):
        record = self.record()
        with patch.object(self.engine, 'verify'), patch.object(self.engine, 'probe'), \
             patch.object(self.engine, 'ram_boot'), \
             patch.object(self.engine, 'run', side_effect=RuntimeError('write failed')) as run:
            with self.assertRaisesRegex(RuntimeError, 'write failed'):
                self.engine.apply(record)
        self.assertEqual(run.call_count, 1)
        self.assertFalse(any(e['event'] in ('verified', 'success') for e in self.events))

    def test_verified_programming_distinguished_from_boot_failure(self):
        record = self.record()
        self.engine.manifest = {'build_id': 'test'}
        with patch.object(self.engine, 'verify'), patch.object(self.engine, 'probe'), \
             patch.object(self.engine, 'ram_boot'), patch.object(self.engine, 'run'), \
             patch.object(self.engine, 'wait_glaze', side_effect=RuntimeError('timeout')):
            with self.assertRaisesRegex(RuntimeError, 'programming verified; boot verification incomplete'):
                self.engine.apply(record)
        self.assertTrue(any(e['event'] == 'verified' for e in self.events))
        self.assertFalse(any(e['event'] == 'success' for e in self.events))

    def test_prepare_refuses_existing_glaze(self):
        with patch.object(self.engine, 'verify'), patch.object(self.engine, 'probe', return_value={'runtime': 'Glaze'}), \
             patch.object(self.engine, 'ram_boot') as boot:
            with self.assertRaisesRegex(ValueError, 'stock Windows CE'):
                self.engine.prepare(self.root)
            boot.assert_not_called()

    def test_payload_integrity_before_loading_code(self):
        (self.root / 'glaze.bin').write_bytes(b'changed')
        (self.root / 'manifest.json').write_text(json.dumps({'files': {'glaze.bin': hashlib.sha256(b'original').hexdigest()}}))
        with self.assertRaisesRegex(ValueError, 'failed verification'):
            self.engine.verify()

    def test_boot_identity_must_match(self):
        record = self.record()
        self.engine.manifest = {'build_id': 'expected'}
        self.engine.build = 'old'
        with patch.object(self.engine, 'verify'), patch.object(self.engine, 'probe'), \
             patch.object(self.engine, 'ram_boot'), patch.object(self.engine, 'run'), patch.object(self.engine, 'wait_glaze'):
            with self.assertRaisesRegex(RuntimeError, 'Expected build expected, detected old'):
                self.engine.apply(record)


class OfflinePipelineTest(unittest.TestCase):
    @unittest.skipUnless((Path(__file__).resolve().parents[1] / 'out/installer/resources/manifest.json').exists(),
                         'requires staged installer resources')
    def test_prepare_and_fresh_restore_with_production_encoder(self):
        import importlib.util
        import shutil
        root = Path(__file__).resolve().parents[1]
        spec = importlib.util.spec_from_file_location('physical_fixture', root / 'tests/physical_nand.py')
        fixture = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(fixture)
        fixture.PhysicalNand.setUpClass()
        self.addCleanup(fixture.PhysicalNand.tearDownClass)
        factory = fixture.PhysicalNand.backup
        events, calls = [], []
        with tempfile.TemporaryDirectory() as tmp:
            engine = Installer(root / 'out/installer/resources', events.append, '1.2')
            def run(*args):
                calls.append(args[0])
                if args[0] == 'probe':
                    return [{'event': 'device', 'runtime': 'CE', 'location': '1.2'}]
                if args[0] == 'backup':
                    Path(args[1]).write_bytes(factory)
                return []
            with patch.object(engine, 'run', side_effect=run), patch.object(engine, 'ram_boot'):
                engine.prepare(tmp, ['builtin:files', 'builtin:settings', 'package:hello'])
            record = Path(next(e['record'] for e in events if e['event'] == 'prepared'))
            folder = record.parent
            self.assertEqual(json.loads(record.read_text())['apps'], ['builtin:files', 'builtin:settings', 'package:hello'])
            # Retry after a failed handoff can reuse this installer's RO RAM
            # session instead of requiring another CE boot or touching NAND.
            resumed = Installer(root / 'out/installer/resources', events.append, '1.2')
            resumed.build = json.loads((root / 'out/installer/resources/manifest.json').read_text())['build_id']
            def resumed_run(*args):
                if args[0] == 'probe':
                    return [{'event': 'device', 'runtime': 'Glaze', 'location': '1.2'},
                            {'event': 'capabilities', 'value': 4096}]
                return run(*args)
            with patch.object(resumed, 'run', side_effect=resumed_run), patch.object(resumed, 'ram_boot') as boot:
                resumed.prepare(tmp, [])
                boot.assert_not_called()
            self.assertEqual((folder / 'original-ce.raw').read_bytes(), factory)
            candidate = (folder / 'candidate.raw').read_bytes()
            self.assertEqual(len(candidate), 17301504)
            fresh = bytearray(candidate)
            fresh[400 * 32 * 528] ^= 1  # live filesystem changed since installation
            calls.clear()
            def restore_run(*args):
                calls.append(args[0])
                if args[0] == 'probe':
                    return [{'event': 'device', 'runtime': 'CE', 'location': '1.2'}]
                if args[0] == 'backup':
                    Path(args[1]).write_bytes(fresh)
                if args[0] == 'apply':
                    plan = Path(args[3]).read_bytes()
                    self.assertEqual(plan[8:40], hashlib.sha256(fresh).digest())
                    self.assertEqual(plan[40:72], hashlib.sha256(factory).digest())
                return []
            with patch.object(engine, 'run', side_effect=restore_run), patch.object(engine, 'ram_boot'), \
                 patch('engine.time.sleep'):
                engine.apply(record, restore=True)
            self.assertEqual(calls, ['probe', 'backup', 'apply', 'cold-reboot', 'probe'])
            self.assertEqual(events[-1]['event'], 'success')


if __name__ == '__main__':
    unittest.main()
