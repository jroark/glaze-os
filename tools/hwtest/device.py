"""The real BE-300, through out/host/glaze-usb (docs/dev/HWTEST.md).

Every device command goes through one lock, because only one glaze-usb process
can claim the USB interface (or own the debug dispatcher over the network).
A pump thread drains the console ring into device.log between commands; checks
search that file from an offset, since `logs --once` empties the ring.
"""
import os
import re
import subprocess
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GLAZE_USB = os.path.join(ROOT, 'out', 'host', 'glaze-usb')
TRANSIENT = ('LIBUSB_ERROR_NO_DEVICE', 'LIBUSB_ERROR_PIPE', 'LIBUSB_ERROR_IO', 'timed out',
             'No BE-300', 'not found', 'Resource busy', 'LIBUSB_ERROR_BUSY')


class DeviceError(Exception):
    pass


class Device:
    def __init__(self, rundir, host=None, key=None, glaze_usb=GLAZE_USB):
        self.rundir = rundir
        self.host, self.key = host, key
        self.tool = glaze_usb
        self.lock = threading.RLock()
        self.log_path = os.path.join(rundir, 'device.log')
        self.cmd_log = open(os.path.join(rundir, 'commands.log'), 'a')
        self._pump = None
        self._stop = threading.Event()
        self.pump_paused = threading.Event()
        self.build = None
        self.capabilities = 0

    # -- low level -------------------------------------------------------
    def _argv(self, args):
        argv = [self.tool]
        if self.host:
            argv += ['--host', self.host, '--key', self.key]
        return argv + list(args)

    def run(self, *args, timeout=60, retries=1, check=True, keep_log=False):
        """Runs one glaze-usb command; returns (rc, output). Transient USB
        errors are retried after a pause (CE drops every other command)."""
        out, rc = '', -1
        for attempt in range(retries + 1):
            with self.lock:
                t0 = time.time()
                try:
                    p = subprocess.run(self._argv(args), capture_output=True, text=True,
                                       errors='replace', timeout=timeout)
                    rc, out = p.returncode, p.stdout + p.stderr
                except subprocess.TimeoutExpired as e:
                    rc = -9
                    out = ((e.stdout or b'').decode('utf-8', 'replace') if isinstance(e.stdout, bytes)
                           else (e.stdout or '')) + '\n[hwtest] timed out after %ds' % timeout
                self.cmd_log.write('$ glaze-usb %s  (rc=%d, %.1fs)\n%s\n' % (
                    ' '.join(args), rc, time.time() - t0, out[-2000:]))
                self.cmd_log.flush()
                if keep_log:
                    self._append_log(out)
                self._note_hello(out)
            if rc == 0 or not any(t in out for t in TRANSIENT) or attempt == retries:
                break
            time.sleep(2)
        if check and rc != 0:
            raise DeviceError('glaze-usb %s failed (rc=%d): %s' % (' '.join(args), rc, out.strip()[-400:]))
        return rc, out

    def _note_hello(self, out):
        m = re.search(r'runtime=(\S+) build=(\S+) free=(\d+) KiB capabilities=0x([0-9a-f]+)', out)
        if m:
            self.runtime, self.build = m.group(1), m.group(2)
            self.free_kib, self.capabilities = int(m.group(3)), int(m.group(4), 16)

    def _append_log(self, out):
        lines = [l for l in out.splitlines() if not l.startswith('runtime=')]
        if lines:
            with open(self.log_path, 'a') as f:
                f.write('\n'.join(lines) + '\n')

    # -- logs ------------------------------------------------------------
    def drain(self):
        rc, out = self.run('logs', '--once', timeout=30, retries=0, check=False)
        if rc == 0:
            self._append_log(out)
        return rc == 0

    def log_offset(self):
        try:
            return os.path.getsize(self.log_path)
        except OSError:
            return 0

    def log_text(self, since=0):
        try:
            with open(self.log_path, errors='replace') as f:
                f.seek(since)
                return f.read()
        except OSError:
            return ''

    def wait_log(self, pattern, since=0, timeout=60, fail=None):
        """Drains until `pattern` appears after `since`; returns the match.
        `fail` is a pattern that ends the wait early. Raises on timeout."""
        rx = re.compile(pattern, re.M)
        frx = re.compile(fail, re.M) if fail else None
        deadline = time.time() + timeout
        while True:
            text = self.log_text(since)
            m = rx.search(text)
            if m:
                return m
            if frx and frx.search(text):
                raise DeviceError('log shows %r while waiting for %r' % (frx.search(text).group(0), pattern))
            if time.time() > deadline:
                raise DeviceError('no %r in the log within %ds' % (pattern, timeout))
            self.drain()
            time.sleep(2)

    def start_pump(self, interval=3.0):
        if self._pump:
            return
        def loop():
            while not self._stop.wait(interval):
                if not self.pump_paused.is_set():
                    try:
                        self.drain()
                    except Exception:
                        pass
        self._pump = threading.Thread(target=loop, daemon=True)
        self._pump.start()

    def stop_pump(self):
        self._stop.set()

    # -- sessions ----------------------------------------------------------
    def probe(self):
        rc, out = self.run('probe', timeout=20, retries=0, check=False)
        return rc == 0 and 'runtime=' in out

    def wait_device(self, timeout=120):
        """Waits for the device to answer HELLO again (after a wake, reload or
        disconnect). Returns seconds taken; raises on timeout."""
        t0 = time.time()
        last_reset = t0
        while time.time() - t0 < timeout:
            if self.probe():
                return time.time() - t0
            # Like glaze-usb's reload reconnect: a device that lost its
            # address without the host noticing answers only after a bus
            # reset, so reset every 6 s while it stays silent.
            if not self.host and time.time() - last_reset >= 6:
                self.run('reset-link', timeout=20, retries=0, check=False)
                last_reset = time.time()
            time.sleep(2)
        raise DeviceError('device did not answer within %ds' % timeout)

    def reload(self, image_dir, timeout=240):
        rc, out = self.run('reload', image_dir, timeout=timeout, retries=0, check=False, keep_log=True)
        if rc != 0:
            raise DeviceError('reload %s failed (rc=%d): %s' % (image_dir, rc, out.strip()[-400:]))
        self.drain()
        return out

    def screenshot(self, path):
        self.run('screenshot', path, timeout=60)
        return path

    def script(self, script_path, shot=None, timeout=600):
        args = ['script', script_path] + ([shot] if shot else [])
        return self.run(*args, timeout=timeout)

    def script_text(self, text, shot=None, name='inline'):
        path = os.path.join(self.rundir, 'scripts', name + '.txt')
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, 'w') as f:
            f.write(text.strip() + '\n')
        return self.script(path, shot)

    def put(self, local, remote, timeout=600):
        return self.run('put', local, remote, timeout=timeout)

    def get(self, remote, local, timeout=600):
        return self.run('get', remote, local, timeout=timeout)

    def launch(self, remote, wait=False, timeout=600, check=True):
        args = ['run', remote] + (['--wait'] if wait else [])
        return self.run(*args, timeout=timeout, check=check)
