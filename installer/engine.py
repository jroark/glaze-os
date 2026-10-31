"""Installer operations. All writes go through the production NAND transaction writer."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time
from app_selection import load_catalog, select, customize_logical, install_packages

RAW_SIZE = 17301504


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    # Frozen resources live inside the signed app; never create __pycache__ there.
    sys.dont_write_bytecode = True
    spec.loader.exec_module(module)
    return module


class Installer:
    def __init__(self, resources, emit, device='auto'):
        self.resources = Path(resources).resolve()
        self.emit = emit
        self.device = device
        self.ext = '.exe' if os.name == 'nt' else ''
        self.helper = self.resources / 'bin' / ('glaze-installer-usb' + self.ext)
        self.build = None

    def verify(self):
        manifest = json.loads((self.resources / 'manifest.json').read_text())
        for name, expected in manifest['files'].items():
            path = (self.resources / name).resolve()
            if not path.is_relative_to(self.resources) or digest(path) != expected:
                raise ValueError('Bundled resource failed verification: ' + name)
        self.manifest = manifest
        os.environ['GLAZE_NAND_HELPER'] = str(self.resources / 'bin' / ('nand-inventory' + self.ext))
        os.environ['GLAZE_MKFS_HELPER'] = str(self.resources / 'bin' / ('mkfsimg' + self.ext))
        self.n = load_module('physical', self.resources / 'tools/physical_nand.py')
        self.t = load_module('transaction', self.resources / 'tools/nand_transaction.py')

    def run(self, *args):
        self.emit({'event': 'phase', 'text': str(args[0])})
        p = subprocess.Popen([str(self.helper), self.device, *map(str, args)],
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             text=True, encoding='utf-8', errors='replace',
                             creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
        events = []
        for line in p.stdout:
            line = line.strip()
            try:
                event = json.loads(line)
            except ValueError:
                event = {'event': 'log', 'text': line}
                match = re.search(r'runtime=Glaze build=(\S+)', line)
                if match:
                    self.build = match[1]
            events.append(event)
            if event.get('event') == 'log':
                progress = re.search(r'(.*?): (\d+)/(32768) pages', line)
                if progress:
                    self.emit({'event': 'progress', 'phase': progress[1], 'current': int(progress[2]), 'total': int(progress[3])})
            self.emit(event)
        if p.wait():
            raise RuntimeError(f'{args[0]} failed. See the saved log; no automatic retry was attempted.')
        return events

    def probe(self):
        events = self.run('probe')
        device = next(e for e in events if e.get('event') == 'device')
        self.device = device['location']
        device['capabilities'] = next((e['value'] for e in events if e.get('event') == 'capabilities'), 0)
        return device

    def wait_glaze(self, required=0):
        start = time.monotonic()
        deadline, reset_at = start + 90, start + 3
        while time.monotonic() < deadline:
            time.sleep(1)
            try:
                events = self.run('probe')
            except RuntimeError:
                events = []
            if any(e.get('runtime') == 'Glaze' for e in events):
                caps = next((e['value'] for e in events if e.get('event') == 'capabilities'), 0)
                if caps & required == required:
                    return
                raise RuntimeError(f'Glaze connected but lacks required USB capabilities (need {required:#x}, got {caps:#x}).')
            if time.monotonic() >= reset_at:
                self.emit({'event': 'phase', 'text': 'Refreshing the USB connection after GlazeBoot handoff…'})
                try:
                    # Match glaze-usb's proven reconnect sequence. The helper
                    # resets an unclaimed device even if set-configuration fails.
                    self.run('reset-link')
                except RuntimeError:
                    pass  # GlazeBoot can still have USB powered down; retry.
                reset_at = time.monotonic() + 5
        raise RuntimeError('Glaze did not reconnect after USB refresh attempts. Unplug and reconnect USB once, then click Prepare installation again. No NAND programming was started by this reconnect step.')

    def ram_boot(self, mode):
        # ce_test writes its nonce reply beside the payload; bundles are read-only.
        with tempfile.TemporaryDirectory(prefix='glaze-ram-') as tmp:
            folder = Path(tmp) / 'boot'
            shutil.copytree(self.resources / 'ram', folder)
            (folder / 'glaze.cfg').write_text(f'nand={mode}\nramfs_kb=2048\n')
            self.run('boot', folder)
            self.wait_glaze(4096 | (16384 if mode == 'repair' else 0))

    def prepare(self, destination, requested_apps=None):
        self.verify()
        device = self.probe()
        ram_readonly = (device['runtime'] == 'Glaze' and
                        device.get('capabilities', 0) & 4096 and
                        not device.get('capabilities', 0) & 16384 and
                        self.build == self.manifest['build_id'])
        if device['runtime'] != 'CE' and not ram_readonly:
            raise ValueError('New installation requires stock Windows CE or this installer’s read-only Glaze RAM session. Use Restore for a saved installation.')
        catalog = load_catalog(self.resources)
        chosen = select(catalog, requested_apps)
        logical = customize_logical((self.resources / 'glaze.bin').read_bytes(), catalog, chosen)
        self.n.validate_logical(logical)
        destination = Path(destination).resolve()
        if shutil.disk_usage(destination).free < 256 * 1024 * 1024:
            raise ValueError('At least 256 MiB of free backup space is required.')
        folder = Path(tempfile.mkdtemp(prefix='Glaze-BE300-', dir=destination))
        if os.name != 'nt':
            folder.chmod(0o700)
        self.emit({'event': 'folder', 'path': str(folder)})
        if not ram_readonly:
            self.ram_boot('ro')
        backup = folder / 'original-ce.raw'
        self.run('backup', backup)
        inv = self.n.inspect(backup)
        self.n.write_new(folder / 'inventory.json', json.dumps(inv).encode())
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            bad = bytearray(128)
            for b in inv['quarantined']:
                if self.n.FS_FIRST <= b < self.n.RESERVE:
                    rel = b - self.n.FS_FIRST
                    bad[rel // 8] |= 1 << (rel % 8)
            (tmp / 'bad').write_bytes(bad)
            (tmp / 'seed').write_bytes(os.urandom(32))
            shutil.copytree(self.resources / 'userdata', tmp / 'userdata')
            install_packages(self.resources, catalog, chosen, tmp / 'userdata')
            subprocess.run([str(self.n.MKFS), str(tmp / 'userdata'), '684', str(tmp / 'fs'),
                            '--bad-blocks=' + str(tmp / 'bad'),
                            '/Library/Preferences/net.entropy=' + str(tmp / 'seed')], check=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            raw, placement = self.n.build(logical, backup.read_bytes(),
                                          inv, (tmp / 'fs').read_bytes())
        self.n.write_new(folder / 'candidate.raw', raw)
        self.n.native(['--check-boot', folder / 'candidate.raw'])
        self.n.write_new(folder / 'placement.json', json.dumps(placement).encode())
        plan, review = self.t.prepare(backup.read_bytes(), raw, backup.read_bytes(), inv, placement)
        self.n.write_new(folder / 'install.bin', plan)
        self.n.write_new(folder / 'review.json', json.dumps(review, indent=2).encode())
        files = ['original-ce.raw', 'candidate.raw', 'inventory.json', 'placement.json', 'install.bin', 'review.json']
        record = {'version': 1, 'device': self.device, 'apps': chosen, 'payload': self.manifest,
                  'files': {f: digest(folder / f) for f in files}}
        self.n.write_new(folder / 'installation.json', json.dumps(record, indent=2).encode())
        self.emit({'event': 'prepared', 'record': str(folder / 'installation.json'),
                   'text': f'{len(chosen)} apps selected. Backup verified. Installation will replace Windows CE and its stored files. Backup: {backup}'})

    def apply(self, record_path, restore=False):
        self.verify()
        record_path = Path(record_path).resolve()
        folder = record_path.parent
        record = json.loads(record_path.read_text())
        required = {'original-ce.raw', 'candidate.raw', 'inventory.json', 'placement.json', 'install.bin', 'review.json'}
        if record.get('version') != 1 or set(record.get('files', {})) != required:
            raise ValueError('Unsupported or incomplete installation record')
        for name, expected in record['files'].items():
            if digest(folder / name) != expected:
                raise ValueError('Saved installation artifact differs: ' + name)
        # Bind the operation to the recorded physical USB port. Moving the device
        # requires reconnecting at that port; source comparison additionally binds bytes.
        self.device = record['device']
        self.probe()
        self.ram_boot('repair')
        source, target, plan = folder / 'original-ce.raw', folder / 'candidate.raw', folder / 'install.bin'
        if restore:
            attempt = Path(tempfile.mkdtemp(prefix='restore-', dir=folder))
            source, target, plan = attempt / 'current.raw', folder / 'original-ce.raw', attempt / 'restore.bin'
            self.run('backup', source)
            data, review = self.t.prepare(source.read_bytes(), target.read_bytes(), target.read_bytes(),
                                         json.loads((folder / 'inventory.json').read_text()), restore=True)
            self.n.write_new(plan, data)
            self.n.write_new(attempt / 'review.json', json.dumps(review, indent=2).encode())
        self.emit({'event': 'programming', 'text': 'Programming and verifying NAND. Keep USB and power connected.'})
        self.run('apply', source, target, plan)
        self.emit({'event': 'verified', 'text': 'Whole-device programming verification passed.'})
        self.run('cold-reboot')
        if restore:
            deadline = time.monotonic() + 90
            while time.monotonic() < deadline:
                time.sleep(2)
                try:
                    if self.probe()['runtime'] == 'CE':
                        self.emit({'event': 'success', 'text': 'Original CE NAND restored and CE USB detected.'})
                        return
                except RuntimeError:
                    pass
            raise RuntimeError('CE bytes were restored and verified, but CE USB was not detected after reboot.')
        expected = self.manifest['build_id']
        try:
            self.wait_glaze()
            if self.build != expected:
                raise RuntimeError(f'Expected build {expected}, detected {self.build}')
        except RuntimeError as error:
            raise RuntimeError('NAND programming verified; boot verification incomplete. ' + str(error)) from error
        self.emit({'event': 'success', 'text': 'Glaze installed, verified, and detected after NAND boot.'})
