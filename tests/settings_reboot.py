#!/usr/bin/env python3
"""Exercise the Settings confirmation and actual return-to-CE flow in a guest.

Supply a stock CE NAND and an autorun GlazeBoot CF. NAND visibility and
reboot permissions are covered by the native settings-test/reboot-test targets.
Only disposable copies are booted. All evidence stays under out/.
"""
import argparse
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import time
import zlib
from usb_runtime import Peer


def script(peer, events, wait=True):
    assert peer.call(16, struct.pack('<I', len(events)))[0] == 0
    data = b''.join(struct.pack('<IHHhh', *event) for event in events)
    assert peer.call(17, struct.pack('<I', 0) + data)[0] == 0
    assert peer.call(18)[0] == 0
    if not wait:
        return
    end = time.monotonic() + 15
    while time.monotonic() < end:
        if struct.unpack_from('<I', peer.call(19)[1])[0] == 4:
            time.sleep(.5)
            return
        time.sleep(.03)
    raise RuntimeError('input script did not complete')


def tap(peer, x, y, wait=True):
    script(peer, [(0, 1, 0, x, y), (60, 3, 0, x, y)], wait)


def screenshot(peer, path):
    status, meta = peer.call(13)
    assert status == 0 and struct.unpack('<IIII', meta) == (240, 320, 1, 153600)
    data = bytearray()
    while len(data) < 153600:
        status, block = peer.call(14, struct.pack('<I', len(data)))
        assert status == 0 and block
        data += block
    assert peer.call(15)[0] == 0
    raw = bytearray()
    for y in range(320):
        raw.append(0)
        for x in range(240):
            v = struct.unpack_from('<H', data, 2 * (y * 240 + x))[0]
            raw.extend(((v >> 11) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31))
    def chunk(kind, payload):
        return struct.pack('>I', len(payload)) + kind + payload + struct.pack('>I', zlib.crc32(kind + payload))
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 240, 320, 8, 2, 0, 0, 0)) +
                     chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))
    return data[16 * 240 * 2:]  # compare app pixels independently of the status bar


def run(args):
    work = Path('out/settings-reboot-ce').resolve()
    work.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(args.ce_nand, work / 'nand.img')
    shutil.copyfile(args.cf, work / 'cf.img')
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0)); listener.listen(1); listener.settimeout(20)
        with (work / 'stdout.log').open('wb') as out, (work / 'stderr.log').open('wb') as err:
            command = [str(args.emulator.resolve()), '--nand', str(work / 'nand.img'), '--rtc-host-time', '--speed', '0',
                       '--pcconnect-dock', 'usb-sync', '--pcconnect-bridge', f'tcp:127.0.0.1:{listener.getsockname()[1]}']
            command += ['--cf', str(work / 'cf.img')]
            process = subprocess.Popen(command, cwd=work, env=dict(os.environ, SDL_VIDEODRIVER='dummy'), stdout=out, stderr=err)
            try:
                sock, _ = listener.accept()
                with sock:
                    sock.settimeout(120)
                    peer = Peer(sock)
                    peer.send(0, b'settings-reboot-test'.ljust(64, b'\0') + struct.pack('<I', 0x52))
                    peer.offer()
                    end = time.monotonic() + 120
                    while b'springboard: ready' not in (work / 'stdout.log').read_bytes():
                        if time.monotonic() >= end:
                            raise RuntimeError('SpringBoard did not start')
                        time.sleep(.03)
                    peer.configure()
                    assert peer.call(1)[0] == 0
                    events = [(500 * i // 17, 1 if i == 0 else 2, 0, 40 + 195 * i // 17, 280) for i in range(18)]
                    script(peer, events + [(520, 3, 0, 235, 280)])
                    assert peer.call(6, b'bin/settings')[0] == 0
                    time.sleep(.5)
                    script(peer, [(0, 1, 0, 120, 270), (200, 2, 0, 120, 170), (220, 3, 0, 120, 170)])
                    root = screenshot(peer, work / 'root.png')
                    tap(peer, 120, 297)
                    confirm = screenshot(peer, work / 'confirmation.png')
                    assert confirm != root
                    tap(peer, 120, 237)  # Cancel
                    assert screenshot(peer, work / 'cancelled.png') == root
                    tap(peer, 120, 297)
                    shells = (work / 'stdout.log').read_bytes().count(b'CASIO Original Shell started')
                    tap(peer, 120, 190, wait=False)
                    end = time.monotonic() + 90
                    while (work / 'stdout.log').read_bytes().count(b'CASIO Original Shell started') <= shells:
                        if time.monotonic() >= end:
                            raise RuntimeError('Settings reboot did not return to stock CE')
                        time.sleep(.03)
                    print('CE RAM boot: confirmation, cancellation and Settings reboot to stock CE PASS', flush=True)
            finally:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill(); process.wait()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ce-nand', type=Path, required=True)
    parser.add_argument('--cf', type=Path, required=True)
    parser.add_argument('--emulator', type=Path, default=Path('../be300/build-host/be300'))
    args = parser.parse_args()
    run(args)
    print('settings-reboot: ALL PASS', flush=True)
