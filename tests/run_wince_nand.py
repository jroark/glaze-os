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
import shlex
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
    ap.add_argument('--app',help='original executable in Program Files (default calc.exe); its DLLs come from the image')
    ap.add_argument('--input',type=Path,help='timed events, in milliseconds after the launch tap')
    ap.add_argument('--expect',action='append',default=[],help='serial log text that must appear (repeatable)')
    ap.add_argument('--allow-unresolved',action='store_true',help='do not fail on statically unresolved imports')
    ap.add_argument('--no-build',action='store_true',help='reuse the previous build (and its input script)')
    ap.add_argument('--work',type=Path,help='output directory (default out/wince-nand-original or -synthetic)')
    args=ap.parse_args()
    if not args.emulator.exists():
        for candidate in ('build-host-service','build-host','build-host-headless','build-ci'):
            if (ROOT.parent/'be300'/candidate/'be300').exists() or (Path.home()/'src/be300'/candidate/'be300').exists():
                args.emulator=next(p for p in (ROOT.parent/'be300'/candidate/'be300',Path.home()/'src/be300'/candidate/'be300') if p.exists());break
    work=(args.work or ROOT/('out/wince-nand-original' if args.image else 'out/wince-nand-synthetic')).resolve();work.mkdir(parents=True,exist_ok=True)
    for old in work.glob('screenshot_*.bmp'):old.unlink()
    folders=[]
    for d in ('Program Files','My Documents','Backup','Windows'):
        if (work/d).exists():shutil.rmtree(work/d)
    if args.image:
        original=args.image.read_bytes();start,count=u32(original,40),u32(original,44)
        packed=original[start*512:(start+count)*512]
        volume=Fat16(original)
        expected=args.app or 'calc.exe'
        # The original volume's folders, as the device has them.
        def cluster_of(path):
            d=volume.root;cl=None
            for part in path.split('/'):
                cl=next(e[3] for e in volume.entries(d) if e[0]==part);d=volume.chain(cl)
            return cl
        def copy_tree(fat_dir,host):
            host.mkdir(exist_ok=True)
            for name,_,attr,cluster,_ in volume.entries(volume.chain(cluster_of(fat_dir))):
                if attr&16:copy_tree(fat_dir+'/'+name,host/name)
                else:(host/name).write_bytes(volume.read(fat_dir+'/'+name))
        roots=sorted((e[0] for e in volume.entries(volume.root) if e[2]&16),key=str.lower)
        for d in roots:
            copy_tree(d,work/d);folders.append(work/d)
        names=sorted((p.name for p in (work/'Program Files').iterdir()),key=lambda n:(not (work/'Program Files'/n).is_dir(),n.lower()))
        target=next((n for n in names if n.lower() in (expected.lower(),expected.lower()+'.cpk')),None)
        if not target:ap.error(f'{expected} is not in the image\'s Program Files')
        folder_row=roots.index('Program Files')
        app_row=names.index(target)
    else:
        programs=work/'Program Files';programs.mkdir()
        from make_wince_abi import executable
        pe=bytearray(executable())
        # A tiny original test application, returning a recognizable status.
        struct.pack_into('<III',pe,512,0x24020025,0x03e00008,0) # li v0,37; jr ra; nop
        (programs/'Fixture.exe').write_bytes(pe)
        folders.append(programs)
        packed=synthetic_nk();expected='Fixture.exe';folder_row=app_row=0
    # Unlock, open Files and NAND (the fifth row), open Program Files, then
    # scroll its 36-pixel rows (260 visible below the bar, first centred at
    # y 78) with slow drags that rest before lifting, so nothing flings.
    rows=len(names) if args.image else 1
    scroll=max(0,min(36*app_row-150,36*rows-260))
    lines=['3000 drag 40 280 235 280 500','4500 tap 30 50','6500 tap 120 222',f'8500 tap 120 {78+36*folder_row}']
    t,left=10500,scroll
    while left>0:
        step=min(200,left);lines.append(f'{t} down 120 290')
        for i in range(1,21):lines.append(f'{t+i*50} move 120 {290-step*i//20}')
        lines.append(f'{t+1250} up 120 {290-step}');t+=1600;left-=step
    app_y=78+36*app_row-scroll
    launch=t;lines.append(f'{launch} tap 120 {app_y}')
    script=work/'input.txt'
    script.write_text('\n'.join(lines)+'\n')
    # --input times count from the launch tap.
    if args.input:
        with script.open('a') as f:
            for line in args.input.read_text().splitlines():
                parts=line.split(None,1)
                if parts and parts[0].isdigit():f.write(f'{launch+int(parts[0])} {parts[1] if len(parts)>1 else ""}\n')
                elif line.strip():f.write(line+'\n')
    if args.relaunch:
        if not args.image:ap.error('--relaunch requires --image')
        # Calculator's 2 + 3 =, Home, Files again, the same row, 2 + 3 = again.
        r=launch+25500
        with script.open('a') as f:f.write(f'{r} tap 120 232\n{r+1000} tap 207 265\n{r+2000} tap 164 232\n{r+3000} tap 164 265\n'
                                           f'{r+4000} tap 224 12\n{r+6000} tap 30 50\n{r+8000} tap 120 {app_y}\n{r+14000} tap 120 232\n'
                                           f'{r+15000} tap 207 265\n{r+16000} tap 164 232\n{r+17000} tap 164 265\n')
    # Natively in the cross environment, otherwise through ./build.sh's
    # container, which sees the repository at /work. The kernel reads the
    # script from romfs (test/input.txt), so after one build with any script,
    # --no-build repacks GlazeBoot's romfs with this run's script instead.
    native=shutil.which('mipsel-linux-gnu-gcc')
    relative=script.resolve().relative_to(ROOT)
    command=(['make','-j4','INPUT_SCRIPT='+str(script)] if native else
             ['./build.sh','INPUT_SCRIPT=/work/'+str(relative)])+['glazeboot','ramboot']
    if not args.no_build:
        with (work/'build.log').open('wb') as log:
            subprocess.run(command,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,check=True)
    romfs_path=work/'romfs.img'
    pack=subprocess.run(['make','-n','INPUT_SCRIPT='+str(relative),'out/glazeboot/romfs.img'],cwd=ROOT,capture_output=True,text=True)
    pack_line=next((l for l in pack.stdout.splitlines() if 'mkromfs.py' in l),None)
    if not pack_line:raise RuntimeError('cannot find the GlazeBoot romfs command:\n'+pack.stdout[-2000:]+pack.stderr[-2000:])
    pack_args=shlex.split(pack_line)
    pack_args[pack_args.index('out/glazeboot/romfs.img')]=str(romfs_path)
    subprocess.run(pack_args,cwd=ROOT,stdout=subprocess.DEVNULL,check=True)
    fat=work/'fat.img'
    fat.unlink(missing_ok=True)
    if shutil.which('mkfs.fat'):
        subprocess.run(['mkfs.fat','-F','16','-s','1','-n','CETEST','-C',str(fat),'6144'],stdout=subprocess.DEVNULL,check=True)
    else:
        fat.write_bytes(bytes(6144*1024))
        subprocess.run(['mformat','-i',str(fat),'-T',str(6144*2),'-h','16','-s','32','-c','1','-v','CETEST','::'],stdout=subprocess.DEVNULL,check=True)
    for d in folders:
        subprocess.run(['mcopy','-s','-i',str(fat),str(d),'::/'],stdout=subprocess.DEVNULL,check=True)
    data=bytearray((ROOT/'out/glaze-ramboot.bin').read_bytes())
    # Model the default GlazeBoot app selection, leaving enough RAM for NK's
    # temporary metadata recovery. SPL reads this partition's explicit length.
    romfs=romfs_path.read_bytes()
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
    assert 'PANIC' not in log and 'fault: ' not in log and 'wince: error:' not in log
    assert 'fs: NAND FAT volume mounted' in log
    assert 'wince: entering original executable' in log
    assert expected in log
    for text in args.expect:assert text in log,f'missing from serial log: {text}'
    unresolved=sorted({line.split('import ',1)[1] for line in log.splitlines() if 'wince: unresolved import ' in line})
    if unresolved:print('Unresolved imports:',', '.join(unresolved))
    assert args.allow_unresolved or not unresolved,'statically unresolved imports'
    if not args.image:assert 'wince: original executable returned 37' in log
    if args.relaunch:
        assert log.count('wince: entering original executable')==2, 'second launch did not enter CE'
        assert log.count('wince: recovered ')==1, 'metadata cache was not reused'
    assert hashlib.sha256(nand.read_bytes()[0x500000:]).hexdigest()==before,'Original CE partitions changed'
    print('Files/NAND CE launch: PASS; source partitions unchanged')
    for p in work.glob('screenshot_*.bmp'):print(p)

if __name__=='__main__':main()
