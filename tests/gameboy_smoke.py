#!/usr/bin/env python3
"""Linux/headless package install and gameplay regression. Run inside build tooling.

python3 tests/gameboy_smoke.py --be300 /path/to/be300
Builds scripted input, runs disposable NAND and CF copies, checks guest markers,
and restores the normal image build even on a regression failure.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--be300', type=Path, required=True)
    args = parser.parse_args()
    emulator = args.be300.resolve()
    if not emulator.is_file():
        parser.error(f'emulator not found: {emulator}')
    work = ROOT / 'out/shots/gameboy'
    work.mkdir(parents=True, exist_ok=True)
    log = work / 'build.log'
    with log.open('w') as buildlog:
        def run(command):
            subprocess.run(command, cwd=ROOT, stdout=buildlog,
                           stderr=subprocess.STDOUT, check=True)
        try:
            run(['make', '-j4', 'INPUT_SCRIPT=tests/t_gameboy.txt'])
            shutil.copyfile(ROOT / 'out/pkg-res/gameboy/demo.gb', work / 'test.gb')
            shutil.copyfile(ROOT / 'out/pkg-res/gameboy/demo.gbc', work / 'test.gbc')
            run(['sh', 'tools/mkcfimg.sh', str(work / 'cf.img'), '16', '16',
                 'out/pkg/gameboy.gzp', str(work / 'test.gb'), str(work / 'test.gbc')])
            shutil.copyfile(ROOT / 'out/glaze.bin', work / 'glaze.bin')
            env = dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy',
                       BE300_AUTOSTOP_SEC='100')
            with (work / 'stdout.log').open('w') as out, (work / 'stderr.log').open('w') as err:
                result = subprocess.run([str(emulator), '--nand', 'glaze.bin',
                                         '--rtc-host-time', '--cf', 'cf.img'],
                                        cwd=work, env=env, stdout=out, stderr=err, timeout=125)
            text = (work / 'stdout.log').read_text(errors='replace')
            diagnostics = (work / 'stderr.log').read_text(errors='replace')
            required = ['gameboy: ready', 'gameboy: loaded Glaze DMG demo', 'gameboy: paused',
                        'gameboy: resumed', 'gameboy: battery saved', 'gameboy: battery restored',
                        'gameboy: loaded Glaze Color demo', 'gameboy: loaded test.gb', 'gameboy: loaded test.gbc',
                        'gameboy: clock restored', 'gameboy: input 1', 'gameboy: input 16', 'gameboy: input 32',
                        'gameboy: sound off', 'gameboy: sound on', 'gameboy: reset', 'gameboy: Start', 'gameboy: Select',
                        'gameboy: backgrounded', 'gameboy: foregrounded']
            missing = [m for m in required if m not in text]
            if result.returncode or missing or '[BE300] Autostop reached' not in diagnostics:
                raise RuntimeError(f'Game Boy smoke failed: exit={result.returncode}, missing={missing}; see {work}')
            if 'fault:' in text or 'PANIC' in text or 'gameboy: battery save failed' in text:
                raise RuntimeError(f'Game Boy smoke reported a guest failure; see {work}')
            print(f'Game Boy smoke PASS: install, launch, controls, audio path, pause/resume, '
                  f'DMG/CGB, ROM from Card, battery/clock restoration, reset, Home/warm resume. Logs: {work}')
        finally:
            run(['make', '-j4'])


if __name__ == '__main__':
    main()
