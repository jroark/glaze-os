#!/usr/bin/env python3
"""ROM -> relocated SPL -> protected kernel/romfs -> filesystem integration.
Uses synthetic backups only. Never opens a USB device or programs hardware.
"""
import argparse
import concurrent.futures
import importlib.util
import json
import os
import signal
import subprocess
import time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('physical',ROOT/'tools/physical_nand.py')
n=importlib.util.module_from_spec(spec);spec.loader.exec_module(n)


def run_case(emulator, folder, source, expect, reject, timeout, cf=None, preserve=(1,321,350)):
    folder.mkdir(parents=True,exist_ok=True)
    image=folder/'nand.raw'; image.write_bytes(source)
    out=folder/'stdout.log'; err=folder/'stderr.log'
    with out.open('wb') as stdout,err.open('wb') as stderr:
        command=[str(emulator),'--nand',str(image)]
        if cf:command+=['--cf',str(cf)]
        process=subprocess.Popen(command,cwd=folder,
                                 stdout=stdout,stderr=stderr,env={**os.environ,'SDL_VIDEODRIVER':'dummy'})
        end=time.monotonic()+timeout
        while process.poll() is None and time.monotonic()<end:
            text=out.read_text(errors='replace')
            if expect and expect in text:break
            if reject and 'springboard: ready' in text:break
            time.sleep(0.1)
        if process.poll() is None:
            process.send_signal(signal.SIGTERM)
            try:process.wait(timeout=3)
            except subprocess.TimeoutExpired:process.kill();process.wait()
    text=out.read_text(errors='replace')
    if expect and expect not in text:raise AssertionError(f'{folder.name}: missing {expect}')
    if reject and 'springboard: ready' in text:raise AssertionError(f'{folder.name}: corrupted image booted')
    if not reject and ('PANIC' in text or 'boot stopped' in text):raise AssertionError(f'{folder.name}: boot failed')
    result=image.read_bytes()
    assert len(result)==n.RAW_SIZE
    for block in preserve:
        assert result[block*n.BLOCK:(block+1)*n.BLOCK]==source[block*n.BLOCK:(block+1)*n.BLOCK],folder.name
    if cf:assert result==source,'CF recovery modified NAND'
    return folder.name


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--emulator',type=Path,default=ROOT.parent/'be300/build-host/be300')
    p.add_argument('--logical',type=Path,default=ROOT/'out/glaze.bin')
    p.add_argument('--artifacts',type=Path,required=True)
    p.add_argument('--timeout',type=float,default=20)
    p.add_argument('--recovery',type=Path,help='also boot this recovery container from CF with blank NAND')
    args=p.parse_args();args.artifacts=args.artifacts.resolve();args.artifacts.mkdir(parents=True,exist_ok=False)
    raw=bytearray(b'\xff'*n.RAW_SIZE);bad={1,321,350};pages=[];where=[]
    for block in range(1024):
        if block in bad:
            for page in (0,1):raw[(block*32+page)*528+517]=0
        else:
            for page in range(5):
                pages.append(b'\xff'*512+b'\xaa\x55\x0f\xff'+block.to_bytes(4,'little'));where.append(block*32+page)
    encoded=n.encode(pages)
    for i,page in enumerate(where):raw[page*528:(page+1)*528]=encoded[i*528:(i+1)*528]
    backup=args.artifacts/'backup.raw';backup.write_bytes(raw)
    inv=args.artifacts/'inventory.json';inv.write_text(json.dumps(n.inspect(backup),indent=2))
    userdata=args.artifacts/'userdata';userdata.mkdir();(userdata/'fixture.txt').write_text('physical NAND integration\n')
    candidate=args.artifacts/'candidate.raw';plan=args.artifacts/'plan.json'
    subprocess.run(['python3',str(ROOT/'tools/physical_nand.py'),'pack','--logical',str(args.logical),
        '--backup',str(backup),'--inventory',str(inv),'--userdata',str(userdata),
        '--out',str(candidate),'--plan',str(plan)],check=True)
    source=candidate.read_bytes();replacement=int(json.loads(plan.read_text())['boot_replacements']['1'])
    cases=[('clean',source,'springboard: ready',False)]
    def corrupt(name,page,changes,expect,reject):
        b=bytearray(source)
        for off,value in changes:b[page*528+off]^=value
        cases.append((name,b,expect,reject))
    spl_page=replacement*32+1
    corrupt('spl-one',spl_page,[(17,0xA5)],'springboard: ready',False)
    corrupt('spl-two',spl_page,[(17,0xA5),(43,0x59)],'springboard: ready',False)
    corrupt('spl-three',spl_page,[(17,0xA5),(43,0x59),(91,0x37)],None,True)
    corrupt('kernel-two',160,[(17,0xA5),(43,0x59)],'springboard: ready',False)
    corrupt('kernel-three',160,[(17,0xA5),(43,0x59),(91,0x37)],'boot stopped',True)
    corrupt('romfs-two',512,[(17,0xA5),(43,0x59)],'springboard: ready',False)
    corrupt('romfs-three',512,[(17,0xA5),(43,0x59),(91,0x37)],'missing or corrupt boot page',True)
    bad_manifest=bytearray(source);page=bytearray(bad_manifest[528:1056]);page[508]^=1
    bad_manifest[528:1056]=n.encode([page[:520]])
    cases.append(('manifest-crc',bad_manifest,'invalid physical image manifest',True))
    with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
        futures=[pool.submit(run_case,args.emulator.resolve(),args.artifacts/name,bytes(data),expect,reject,args.timeout)
                 for name,data,expect,reject in cases]
        for f in concurrent.futures.as_completed(futures):print(f'{f.result()}: PASS',flush=True)
    # Prove that the emulator's saved raw image is bootable on a second run.
    saved=(args.artifacts/'clean/nand.raw').read_bytes()
    run_case(args.emulator.resolve(),args.artifacts/'restart',saved,'springboard: ready',False,args.timeout)
    print('raw save/restart: PASS')
    if args.recovery:
        cf=args.artifacts/'recovery-cf.img'
        subprocess.run([str(ROOT/'tools/mkcfimg.sh'),str(cf),'16','16',str(args.recovery.resolve())],check=True)
        run_case(args.emulator.resolve(),args.artifacts/'recovery',b'\xff'*n.RAW_SIZE,
                 'springboard: ready',False,args.timeout,cf)
        print('CF recovery / blank NAND unchanged: PASS')

if __name__=='__main__':main()
