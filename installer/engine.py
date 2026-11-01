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
import uuid
from contextlib import contextmanager
from app_selection import load_catalog, select, customize_logical, install_packages, entries, archive

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
        self.verify_wince = True
        self.install_ui = False
        self.restoring = False

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
            if event.get('event') == 'device' and event.get('runtime') == 'CE':
                self.install_ui = False
            if event.get('event') == 'capabilities':
                self.install_ui = bool(event['value'] & 32768)
            events.append(event)
            if event.get('event') == 'log':
                progress = re.search(r'(.*?): (\d+)/(32768) pages', line)
                if progress:
                    self.emit({'event': 'progress', 'phase': progress[1], 'current': int(progress[2]), 'total': int(progress[3])})
            self.emit(event)
        if p.wait():
            if args[0] == 'boot' and any('No CE staging volume has enough' in e.get('text', '') for e in events):
                raise RuntimeError('Not enough NAND space to start Glaze. In Windows CE, delete built-in apps you no longer need, keeping PC Connect available, then try again.')
            raise RuntimeError(f'{args[0]} failed. See the saved log; no automatic retry was attempted.')
        return events

    def device_status(self, stage):
        if self.install_ui:
            self.run('status', stage, 0, 0)

    def probe(self):
        events = self.run('probe')
        device = next(e for e in events if e.get('event') == 'device')
        self.device = device['location']
        device['capabilities'] = next((e['value'] for e in events if e.get('event') == 'capabilities'), 0)
        self.emit(dict(device, event='device'))
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
        raise RuntimeError('Glaze did not reconnect after USB refresh attempts. Unplug and reconnect USB once, then try Install Glaze again. No NAND programming was started by this reconnect step.')

    def ram_boot(self, mode):
        # ce_test writes its nonce reply beside the payload; bundles are read-only.
        with tempfile.TemporaryDirectory(prefix='glaze-ram-') as tmp:
            folder = Path(tmp) / 'boot'
            shutil.copytree(self.resources / 'ram', folder)
            ram_files = entries((folder / 'romfs.img').read_bytes(), b'GZRF')
            ram_files['installer/session'] = b'GZREST1\n' if self.restoring else b'GZINST1\n'
            (folder / 'romfs.img').write_bytes(archive(ram_files))
            (folder / 'glaze.cfg').write_text(f'nand={mode}\nramfs_kb=2048\n')
            self.emit({'event': 'status', 'text': 'Starting Glaze…'})
            if self.verify_wince:
                self.run('boot', folder)
            else:
                self.run('boot', folder, '--skip-ce-test')
            self.wait_glaze(4096 | (16384 if mode == 'repair' else 0))

    def prepare(self, destination, requested_apps=None, verify_wince=True):
        self.verify_wince = verify_wince
        self.emit({'event': 'status', 'text': 'Connecting to your BE-300…'})
        self.verify()
        device = self.probe()
        ram_readonly = (device['runtime'] == 'Glaze' and
                        device.get('capabilities', 0) & 4096 and
                        not device.get('capabilities', 0) & 16384 and
                        device.get('capabilities', 0) & 8)
        if device['runtime'] != 'CE' and not ram_readonly:
            raise ValueError('New installation requires stock Windows CE or this installer’s read-only Glaze RAM session. Use Restore for a saved installation.')
        catalog = load_catalog(self.resources)
        chosen = select(catalog, requested_apps)
        logical = customize_logical((self.resources / 'glaze.bin').read_bytes(), catalog, chosen, self.resources)
        self.n.validate_logical(logical)
        destination = Path(destination).resolve()
        if shutil.disk_usage(destination).free < 256 * 1024 * 1024:
            raise ValueError('At least 256 MiB of free backup space is required.')
        folder = Path(tempfile.mkdtemp(prefix='Glaze-BE300-', dir=destination))
        if os.name != 'nt':
            folder.chmod(0o700)
        self.emit({'event': 'folder', 'path': str(folder)})
        if not ram_readonly or self.build != self.manifest['build_id'] or not self.install_ui:
            self.ram_boot('ro')
        backup = folder / 'original-ce.raw'
        self.device_status(1)
        self.emit({'event': 'status', 'text': 'Reading your device…'})
        self.run('backup' if verify_wince else 'snapshot', backup)
        self.device_status(2)
        self.emit({'event': 'status', 'text': 'Preparing your apps…'})
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
        record = {'version': 1, 'verify_wince': verify_wince, 'device': self.device, 'apps': chosen, 'payload': self.manifest,
                  'files': {f: digest(folder / f) for f in files}}
        self.n.write_new(folder / 'installation.json', json.dumps(record, indent=2).encode())
        self.emit({'event': 'prepared', 'record': str(folder / 'installation.json'),
                   'text': 'Your apps are ready to install.'})
        return folder / 'installation.json'

    def install_device(self, destination, requested_apps=None, save_backup=True, verify_wince=True):
        try:
            return self._install_device(destination, requested_apps, save_backup, verify_wince)
        except Exception:
            try:
                self.device_status(7)
            except Exception:
                pass
            raise

    def _install_device(self, destination, requested_apps=None, save_backup=True, verify_wince=True):
        # Source bytes are needed for bad-block inventory and transaction
        # binding. Without a backup these exist only in an OS temporary folder,
        # which is removed on both success and failure, including partial files.
        if save_backup:
            destination = Path(destination).expanduser().resolve()
            destination.mkdir(parents=True, exist_ok=True)
            record = self.prepare(destination, requested_apps, verify_wince=verify_wince)
            self.emit({'event': 'backup-saved', 'path': str(record.parent),
                       'text': 'Windows CE backup saved.'})
            self.apply(record)
        else:
            with tempfile.TemporaryDirectory(prefix='glaze-install-') as temporary:
                record = self.prepare(temporary, requested_apps, verify_wince=verify_wince)
                self.apply(record)

    def apply(self, record_path, restore=False):
        self.restoring = restore
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
        self.emit({'event': 'status', 'text': 'Getting ready to restore…' if restore else 'Getting ready to install…'})
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
        self.emit({'event': 'programming', 'text': ('Restoring Windows CE…' if restore else 'Installing Glaze…') + ' Keep power and USB connected.'})
        self.device_status(3)
        self.run('apply', source, target, plan)
        self.emit({'event': 'verified', 'text': 'Whole-device programming verification passed.'})
        self.emit({'event': 'status', 'text': 'Restarting your BE-300…'})
        self.device_status(6)
        self.run('cold-reboot')
        if restore:
            deadline = time.monotonic() + 90
            while time.monotonic() < deadline:
                time.sleep(2)
                try:
                    if self.probe()['runtime'] == 'CE':
                        self.emit({'event': 'success', 'text': 'Windows CE is restored. You can disconnect USB.'})
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
        self.emit({'event': 'success', 'text': 'Glaze is installed. You can disconnect USB.'})

    @contextmanager
    def maintenance_session(self):
        self.verify()
        device = self.probe()
        if device['runtime'] != 'Glaze' or device['capabilities'] & 3 != 3 or device['capabilities'] & 32768:
            raise ValueError('Connect a BE-300 running Glaze with USB file and app support. Fresh installation is a separate action.')
        with tempfile.TemporaryDirectory(prefix='glaze-maintenance-') as directory:
            self.maintenance_dir = Path(directory)
            self.run('put', self.resources / 'maintenance.gzp', '/Library/Installer/maintenance.gzp')
            self.run('maintenance')
            self.maintenance_armed = False
            try:
                yield
            finally:
                try:
                    if not self.maintenance_armed:
                        self.maintenance_request('quit', timeout=10)
                except (RuntimeError, ValueError):
                    pass  # Arming an OS update disconnects USB.

    def maintenance_request(self, action, timeout=120, wait=True):
        operation_id = uuid.uuid4().hex
        request = self.maintenance_dir / 'request'
        request.write_text(operation_id + ' ' + action)
        self.run('put', request, '/Library/Installer/request')
        if not wait:
            return
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            result = self.maintenance_dir / (uuid.uuid4().hex + '.json')
            try:
                self.run('get-result', result)
                data = json.loads(result.read_text())
                if data.get('id') == operation_id:
                    if data['code'] < 0:
                        raise ValueError(f"Device {action} failed ({data['code']}). Check free space, power and OS/SPL support; completed app installations are retained.")
                    if data.get('phase') == 'arming':
                        self.maintenance_armed = True
                    return data
            except (RuntimeError, OSError, json.JSONDecodeError):
                pass
            finally:
                result.unlink(missing_ok=True)
            time.sleep(0.5)
        raise RuntimeError('Device response timed out. Reconnect USB and check status before trying again.')

    def maintenance_info(self):
        with self.maintenance_session():
            info = self.maintenance_request('info')
            info.update(event='maintenance-info', bundled=self.manifest['os']['version'])
            self.emit(info)
            return info

    def install_apps(self, requested):
        failures = []
        with self.maintenance_session():
            info = self.maintenance_request('info')
            self.emit(dict(info, event='maintenance-info', bundled=self.manifest['os']['version']))
            catalog = load_catalog(self.resources)
            chosen = select(catalog, requested)
            for app in catalog:
                if app['kind'] != 'package' or app['id'] not in chosen:
                    continue
                self.emit({'event': 'status', 'text': 'Installing ' + app['name'] + '…'})
                try:
                    package = self.resources / 'packages' / (app['program'] + '.gzp')
                    files = entries(package.read_bytes(), b'GZPK')
                    metadata = dict(line.split('=', 1) for line in files.get('Info', b'').decode().splitlines() if '=' in line)
                    if metadata.get('id') != app['program'] or metadata.get('exec', 'app') not in files:
                        raise ValueError('Bundled package metadata does not match the app catalog.')
                    installed = info.get('apps', {}).get(app['program'])
                    if installed and version_key(metadata['version']) <= version_key(installed):
                        self.emit({'event': 'log', 'text': app['name'] + ' is already current.'})
                        continue
                    self.run('put', package, '/Library/Installer/app.gzp')
                    result = self.maintenance_request('install')
                    self.emit({'event': 'log', 'text': app['name'] + (' is already current.' if result['code'] == 1 else ' installed.')})
                except (RuntimeError, ValueError) as error:
                    failures.append(app['name'] + ': ' + str(error))
        if failures:
            raise RuntimeError('Some apps were not installed. ' + '; '.join(failures))
        self.emit({'event': 'success', 'text': 'Selected apps are installed. Existing app data was preserved.'})

    def update_os(self):
        expected = None
        with self.maintenance_session():
            info = self.maintenance_request('info')
            expected = self.manifest['os']
            if not info['flags'] & 1:
                raise ValueError('This OS or SPL does not support signed updates. No NAND programming was started.')
            if version_key(expected['version']) <= version_key(info['version']):
                raise ValueError('The bundled OS is already installed, or the device has a newer version.')
            payload = self.resources / 'update.gzu'
            if info['free'] < payload.stat().st_size + 65536:
                raise ValueError('Not enough Flash space for the OS update.')
            self.emit({'event': 'status', 'text': 'Transferring and verifying Glaze OS ' + expected['version'] + '…'})
            self.run('put', payload, '/Library/Updates/installer.gzu')
            self.maintenance_request('update', timeout=300)
            self.emit({'event': 'status', 'text': 'Installing Glaze OS. Keep external power connected; this can take several minutes.'})
            # Do not send another request to a companion that is about to reset.
            time.sleep(5)
        deadline = time.monotonic() + 900
        while time.monotonic() < deadline:
            try:
                with self.maintenance_session():
                    info = self.maintenance_request('info', timeout=10)
                    if info['version'] == expected['version'] and info['build'] == expected['build'] and info['last_result'] == 1:
                        self.emit({'event': 'success', 'text': 'Glaze OS updated. Apps, settings and saved data were preserved.'})
                        return
                    if info['last_result'] == 2:
                        raise ValueError('The SPL rejected the update. The previous OS is running.')
            except RuntimeError:
                pass
            time.sleep(5)
        raise RuntimeError('OS boot verification is incomplete. Keep power connected and use Check status after reconnecting USB. The update will not be armed again automatically.')


def version_key(version):
    parts = [int(p) for p in version.split('.')]
    while parts and parts[-1] == 0:
        parts.pop()
    return tuple(parts)
