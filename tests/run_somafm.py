#!/usr/bin/env python3
"""Boot a disposable image with the real SomaFM package preinstalled.

Build first: ./build.sh INPUT_SCRIPT=tests/t_somafm.txt
Then: python3 tests/run_somafm.py --seconds 660
Use --no-net for the Ethernet error screen. Artifacts stay under out/.
"""
import argparse
import os
from pathlib import Path
import shutil
import signal
import re
import struct
import subprocess
import tempfile


def prepare(root, preinstall=True, install_from_files=False):
    work = Path(tempfile.mkdtemp(prefix='somafm-', dir=root/'out'))
    shutil.copytree(root/'userdata', work/'userdata')
    if install_from_files:
        shutil.copyfile(root/'out/pkg/somafm.gzp', work/'userdata/Documents/SomaFM.gzp')
    archive = (root/'out/pkg/somafm.gzp').read_bytes()
    assert archive[:4] == b'GZPK'
    bundle = work/'userdata/Applications/somafm.app'
    for i in range(struct.unpack_from('<I', archive, 4)[0] if preinstall else 0):
        name, off, size = struct.unpack_from('<48sII', archive, 8+56*i)
        target = bundle/name.split(b'\0')[0].decode()
        assert target.resolve().is_relative_to(bundle.resolve())
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(archive[off:off+size])
    subprocess.run(['make', 'OUT=out/native', 'out/native/host/mkfsimg'], cwd=root, check=True, capture_output=True)
    blocks = re.search(r'#define GZ_FLASH_BLOCKS\s+(\d+)', (root/'lib/include/glaze/abi.h').read_text()).group(1)
    subprocess.run([str(root/'out/native/host/mkfsimg'), str(work/'userdata'), blocks, str(work/'fs.img')], check=True)
    nand = bytearray((root/'out/glaze.bin').read_bytes())
    fs = (work/'fs.img').read_bytes()
    assert len(fs) == len(nand)-0x500000
    nand[0x500000:] = fs
    (work/'glaze.bin').write_bytes(nand)
    return work


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--seconds', type=int, default=60)
    parser.add_argument('--no-net', action='store_true')
    parser.add_argument('--gui', action='store_true')
    parser.add_argument('--guest', action='store_true', help='require the optional radio_guest integration fixture to pass')
    parser.add_argument('--expect-playing', action='store_true', help='require the scripted app resume to report live playback')
    parser.add_argument('--no-preinstall', action='store_true', help='start without an installed SomaFM package')
    parser.add_argument('--install-from-files', action='store_true', help='put the package in Documents instead of preinstalling it')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    work = prepare(root, not (args.no_preinstall or args.install_from_files), args.install_from_files)
    print(work, flush=True)
    emulator = os.environ.get('BE300')
    if not emulator:
        common = subprocess.check_output(['git', 'rev-parse', '--git-common-dir'], cwd=root, text=True).strip()
        main_root = (root/common).resolve().parent
        emulator = str(main_root.parent/'be300/build-host/be300')
    env = dict(os.environ)
    if not args.gui:
        env.update(SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy')
    command = [emulator, '--nand', 'glaze.bin', '--rtc-host-time', '--audio']
    if not args.no_net:
        command += ['--ne2000']
    with (work/'stdout.log').open('w') as stdout, (work/'stderr.log').open('w') as stderr:
        p = subprocess.Popen(command, cwd=work, env=env, stdout=stdout, stderr=stderr)
        try:
            p.wait(timeout=args.seconds)
        except subprocess.TimeoutExpired:
            p.send_signal(signal.SIGTERM)
            p.wait(timeout=10)
    log = (work/'stdout.log').read_text(errors='replace')
    failures = [line for line in log.splitlines() if any(s in line for s in ('fault:', 'PANIC', 'dynamic linker:'))
                or re.search(r'proc: (?:audio|somafm) pid \d+ exited \([1-9]', line)]
    for line in log.splitlines():
        if any(s in line for s in ('radio playing:', 'radio stopped', 'radio guest:', 'somafm:', 'appstore: install', 'sb: launch', 'fault:', 'PANIC', 'dynamic linker:')):
            print(line)
    if failures or p.returncode not in (0, -signal.SIGTERM):
        raise SystemExit(f'Emulator failed: exit={p.returncode}, errors={failures}')
    if args.guest and ('radio guest: PASS' not in log or 'radio guest: FAIL' in log):
        raise SystemExit('Guest audio integration checks did not pass')
    if args.expect_playing and 'somafm: resume phase=3' not in log:
        raise SystemExit('The scripted app resume did not report live playback')
    print(f'Artifacts: {work}')


if __name__ == '__main__':
    main()
