#!/usr/bin/env python3
"""Install/play both Sega packages in disposable NAND/CF images; restore normal NAND."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
ROOT=Path(__file__).resolve().parents[1]

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--be300',type=Path,required=True)
    p.add_argument('--system',choices=['mastersystem','genesis','both'],default='both')
    args=p.parse_args();emulator=args.be300.resolve()
    if not emulator.is_file():p.error('BE-300 executable not found')
    base=ROOT/'out/shots/sega';base.mkdir(parents=True,exist_ok=True)
    systems=['mastersystem','genesis'] if args.system=='both' else [args.system]
    with (base/'build.log').open('w') as buildlog:
        def run(cmd):subprocess.run(cmd,cwd=ROOT,stdout=buildlog,stderr=subprocess.STDOUT,check=True)
        try:
            run(['make','-j4','INPUT_SCRIPT=tests/t_sega.txt'])
            clean=base/'scripted.bin';shutil.copyfile(ROOT/'out/glaze.bin',clean)
            for system in systems:
                ext='sms' if system=='mastersystem' else 'gen'
                work=base/system;work.mkdir(exist_ok=True)
                shutil.copyfile(ROOT/f'out/pkg-res/{system}/demo.{ext}',work/f'test.{ext}')
                run(['sh','tools/mkcfimg.sh',str(work/'cf.img'),'16','16',f'out/pkg/{system}.gzp',str(work/f'test.{ext}')])
                shutil.copyfile(clean,work/'glaze.bin')
                env=dict(os.environ,SDL_VIDEODRIVER='dummy',SDL_AUDIODRIVER='dummy',BE300_AUTOSTOP_SEC='100')
                with (work/'stdout.log').open('w') as out,(work/'stderr.log').open('w') as err:
                    r=subprocess.run([str(emulator),'--nand','glaze.bin','--rtc-host-time','--cf','cf.img'],cwd=work,env=env,stdout=out,stderr=err,timeout=125)
                text=(work/'stdout.log').read_text(errors='replace')
                required=['sega: ready','sega: loaded Glaze demo',f'sega: loaded test.{ext}','sega: battery saved','sega: battery restored','sega: input 8','sega: paused','sega: resumed','sega: console Start/Pause','sega: changed TV standard','sega: sound off','sega: sound on','sega: reset','sega: backgrounded','sega: foregrounded']
                if system=='genesis':required+=['sega: remapped buttons','sega: extra button','sega: input 32','sega: input 64']
                else:required+=['sega: input 16','sega: input 32']
                missing=[m for m in required if m not in text]
                if r.returncode or missing:raise RuntimeError(f'{system}: exit={r.returncode}, missing={missing}; logs: {work}')
                for marker in ['fault:','PANIC','battery save failed','CPU failed','invalid opcode']:
                    if marker in text:raise RuntimeError(f'{system}: {marker}; logs: {work}')
                print(f'{system} PASS: install, demo/Card ROM, controls, audio path, saves, pause/reset, TV standard, Home/resume. Logs: {work}',flush=True)
        finally:run(['make','-j4'])
if __name__=='__main__':main()
