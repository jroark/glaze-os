#!/usr/bin/env python3
"""Build/boot Glaze WL54-CF against a host HTTP fixture; preserve all artifacts.

Requires extracted firmware, Docker cross-toolchain and an SDL emulator build.
The guest opens Browser using scripted input, obtains DHCP and fetches a page
with a unique token. --host-ip must be an address reachable by the emulator's
NAT (a host LAN address, not 127.0.0.1). No Internet connection is required.
"""
import argparse
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import ipaddress
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import tempfile
import threading
import uuid


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--emulator', type=Path, required=True)
    parser.add_argument('--host-ip', type=ipaddress.IPv4Address, required=True)
    parser.add_argument('--seconds', type=int, default=35)
    args = parser.parse_args()
    emulator = args.emulator.resolve()
    if args.host_ip.is_loopback:
        parser.error('use a reachable host LAN address, not loopback')
    root = Path(__file__).resolve().parents[1]
    (root / 'out').mkdir(exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix='wl54cf-', dir=root / 'out'))
    token = uuid.uuid4().hex
    requests = []

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            requests.append(self.path)
            page = b'<html><body><h1>WL54-CF HTTP PASS</h1><p>Glaze wireless network works.</p></body></html>'
            self.send_response(200)
            self.send_header('Content-Type', 'text/html')
            self.send_header('Content-Length', str(len(page)))
            self.end_headers()
            self.wfile.write(page)

        def log_message(self, fmt, *values):
            with (work / 'http.log').open('a') as log:
                log.write(fmt % values + '\n')

    server = ThreadingHTTPServer((str(args.host_ip), 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        url = f'http://{args.host_ip}:{server.server_port}/{token}'
        script = work / 'input.txt'
        script.write_text('3000 drag 40 280 235 280 500\n4500 tap 90 120\n'
                          f'6500 tap 100 50\n7000 text {url}\n8000 key enter\n')
        with (work / 'build.log').open('w') as log:
            subprocess.run(['./build.sh', 'WL54CF=1', 'INIT=bin/springboard',
                            f'INPUT_SCRIPT={script.relative_to(root)}'],
                           cwd=root, stdout=log, stderr=subprocess.STDOUT, check=True)
        source = root / 'out/glaze.bin'
        before = digest(source)
        shutil.copyfile(source, work / 'nand.bin')
        env = dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy')
        with (work / 'stdout.log').open('w') as stdout, (work / 'stderr.log').open('w') as stderr:
            process = subprocess.Popen([str(emulator), '--nand', 'nand.bin', '--rtc-host-time',
                                        '--wl54cf'], cwd=work, env=env, stdout=stdout, stderr=stderr)
            try:
                process.wait(timeout=args.seconds)
            except subprocess.TimeoutExpired:
                process.send_signal(signal.SIGTERM)
                process.wait(timeout=10)
        if digest(source) != before:
            raise RuntimeError('source NAND changed')
        log = (work / 'stdout.log').read_text(errors='replace')
        diagnostics = (work / 'stderr.log').read_text(errors='replace')
        if process.returncode not in (0, -signal.SIGTERM):
            raise RuntimeError(f'emulator exit {process.returncode}')
        for milestone in ('wl54cf: firmware ready', 'wl54cf: associated BE300 channel 3',
                          'net: DHCP address '):
            if milestone not in log:
                raise RuntimeError(f'missing milestone: {milestone}')
        if f'/{token}' not in requests:
            raise RuntimeError('guest did not fetch the HTTP fixture')
        if any(s in log for s in ('PANIC', 'fault:')):
            raise RuntimeError('guest fault; inspect stdout.log')
        screenshots = re.findall(r'\[UI\] Screenshot saved: (.+)', diagnostics)
        if not screenshots or not (work / screenshots[-1].strip()).is_file():
            raise RuntimeError('no orderly-shutdown screenshot')
        (work / 'source.sha256').write_text(before + '\n')
        print(f'WL54-CF: firmware, association, DHCP, HTTP PASS; artifacts: {work}')
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)


if __name__ == '__main__':
    main()
