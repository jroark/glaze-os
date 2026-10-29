#!/usr/bin/env python3
"""Exercise Files -> /NAND -> CE app through the real RAM-boot trampoline.

Builds a disposable test NAND with separate Glaze, CE NK metadata and FAT
partitions. Vendor bytes are read only from --image and stay under ignored out/.
This tests Glaze's RAM-boot handoff, not the stock CE/USB transport itself.
Run inside the cross-toolchain environment. Without --image, uses synthetic PE.
"""
from pathlib import Path
import argparse
import hashlib
import os
import shutil
import struct
import subprocess
import sys
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from wince_assets import Fat16,u32
from wince_nand_assets import synthetic_nk,cpk

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--image',type=Path)
    ap.add_argument('--emulator',type=Path,default=ROOT.parent/'be300/build-host-service/be300')
    ap.add_argument('--seconds',type=int,default=65)
    ap.add_argument('--relaunch',action='store_true',help='exercise Calculator input and cached metadata on a second launch')
    args=ap.parse_args()
    work=ROOT/('out/wince-nand-original' if args.image else 'out/wince-nand-synthetic');work.mkdir(parents=True,exist_ok=True)
    programs=work/'Program Files';programs.mkdir(exist_ok=True)
    for p in programs.iterdir():p.unlink()
    if args.image:
        original=args.image.read_bytes();start,count=u32(original,40),u32(original,44)
        packed=original[start*512:(start+count)*512]
        volume=Fat16(original)
        for name in ('calc.exe.cpk','LC.dll.cpk'):(programs/name).write_bytes(volume.read('Program Files/'+name))
        expected='calc.exe'
    else:
        from make_wince_abi import executable
        pe=bytearray(executable())
        # A tiny original test application, returning a recognizable status.
        struct.pack_into('<III',pe,512,0x24020025,0x03e00008,0) # li v0,37; jr ra; nop
        (programs/'Fixture.exe').write_bytes(pe)
        packed=synthetic_nk();expected='Fixture.exe'
    script=work/'input.txt'
    script.write_text('3000 drag 40 280 235 280 500\n4500 tap 30 50\n6500 tap 120 222\n8500 tap 120 78\n10500 tap 120 78\n')
    if args.relaunch:
        if not args.image:ap.error('--relaunch requires --image')
        with script.open('a') as f:f.write('36000 tap 120 232\n37000 tap 207 265\n38000 tap 164 232\n39000 tap 164 265\n40000 tap 224 12\n42000 tap 30 50\n44000 tap 120 78\n50000 tap 120 232\n51000 tap 207 265\n52000 tap 164 232\n53000 tap 164 265\n')
    with (work/'build.log').open('wb') as log:
        subprocess.run(['make','-j4','INPUT_SCRIPT='+str(script),'glazeboot','ramboot'],cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,check=True)
    fat=work/'fat.img'
    fat.unlink(missing_ok=True)
    subprocess.run(['mkfs.fat','-F','16','-s','1','-n','CETEST','-C',str(fat),'6144'],stdout=subprocess.DEVNULL,check=True)
    subprocess.run(['mcopy','-s','-i',str(fat),str(programs),'::/'],stdout=subprocess.DEVNULL,check=True)
    data=bytearray((ROOT/'out/glaze-ramboot.bin').read_bytes())
    # Model the default GlazeBoot app selection, leaving enough RAM for NK's
    # temporary metadata recovery. SPL reads this partition's explicit length.
    romfs=(ROOT/'out/glazeboot/romfs.img').read_bytes()
    data[0x40000:0x500000]=b'\xff'*(0x500000-0x40000)
    data[0x40000:0x40000+len(romfs)]=romfs
    struct.pack_into('<II',data,4*16+8,0x40000//512,(len(romfs)+511)//512)
    if len(packed)>0x500000:raise RuntimeError('CE NK does not fit test partition')
    data[0x500000:0x500000+len(packed)]=packed
    struct.pack_into('<II',data,40,0x500000//512,(len(packed)+511)//512)
    fat_data=fat.read_bytes();data[0xa00000:]=fat_data
    struct.pack_into('<II',data,56,0xa00000//512,len(fat_data)//512)
    nand=work/'nand.img';nand.write_bytes(data)
    before=hashlib.sha256(data[0x500000:]).hexdigest()
    env=dict(os.environ,SDL_VIDEODRIVER='dummy',SDL_AUDIODRIVER='dummy',BE300_AUTOSTOP_SEC=str(args.seconds))
    with (work/'stdout.log').open('wb') as out,(work/'stderr.log').open('wb') as err:
        proc=subprocess.Popen([str(args.emulator),'--nand',str(nand),'--rtc-host-time','--speed','166'],cwd=work,env=env,stdout=out,stderr=err)
        try:proc.wait(timeout=args.seconds+25)
        except subprocess.TimeoutExpired:proc.terminate();proc.wait(timeout=15)
    log=(work/'stdout.log').read_text(errors='replace');print(log[-6000:])
    assert 'PANIC' not in log and 'user fault' not in log and 'wince: error:' not in log
    assert 'fs: NAND FAT volume mounted' in log
    assert 'wince: entering original executable' in log
    assert expected in log
    if not args.image:assert 'wince: original executable returned 37' in log
    if args.relaunch:
        assert log.count('wince: entering original executable')==2, 'second launch did not enter CE'
        assert log.count('wince: recovered ')==1, 'metadata cache was not reused'
    assert hashlib.sha256(nand.read_bytes()[0x500000:]).hexdigest()==before,'Original CE partitions changed'
    print('Files/NAND CE launch: PASS; source partitions unchanged')
    for p in work.glob('screenshot_*.bmp'):print(p)

if __name__=='__main__':main()
