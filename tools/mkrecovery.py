#!/usr/bin/env python3
"""Masked-ROM B000FF recovery image: kernel + RAM romfs, NAND read-only.
The ROM must load staged kernel data away from its own low-memory globals.
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path


def layout(kernel, romfs, repair=False):
    if len(kernel)<4096 or kernel[:4]!=b'GZKB':raise ValueError('invalid kernel.bin header')
    _,load,entry,size,end=struct.unpack_from('<5I',kernel)
    if not size or len(kernel)!=4096+size or load<0x80010000 or load+size>0x81000000 or end<load+size or not load<=entry<load+size:
        raise ValueError('invalid kernel layout')
    padded=(size+3)&~3
    if load+padded>0x80200000 or entry&3:
        raise ValueError('kernel overlaps recovery stub or has an unaligned entry')
    romfs_pa=(16*1024*1024-len(romfs))&~8191
    ramfs_pa=romfs_pa-2*1024*1024
    if end>0x80000000+ramfs_pa or 0x300000+padded+32>ramfs_pa:
        raise ValueError('recovery staging does not fit SDRAM')
    if len(romfs)<8 or romfs[:4]!=b'GZRF':raise ValueError('invalid romfs')
    count=struct.unpack_from('<I',romfs,4)[0]
    if count>256 or 8+count*56>len(romfs):raise ValueError('invalid romfs table')
    for i in range(count):
        offset,length=struct.unpack_from('<II',romfs,8+i*56+48)
        if offset%4096 or offset<8+count*56 or offset+length>len(romfs):
            raise ValueError('invalid romfs file extent')
    info=struct.pack('<8I',0x42525A47,1,8|(4 if repair else 0),romfs_pa,len(romfs),ramfs_pa,128,0)
    staged=kernel[4096:]+bytes(padded-size)+info
    return load,entry,padded,romfs_pa,staged


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--kernel',type=Path,required=True);p.add_argument('--romfs',type=Path,required=True)
    p.add_argument('--flags',type=Path);p.add_argument('--stub',type=Path);p.add_argument('--out',type=Path)
    p.add_argument('--nand-repair',action='store_true',help='explicit raw repair mode; /NAND filesystem remains disabled')
    args=p.parse_args()
    try:
        kernel=args.kernel.read_bytes();romfs=args.romfs.read_bytes()
        load,entry,padded,romfs_pa,staged=layout(kernel,romfs,args.nand_repair)
        if args.flags:
            args.flags.write_text(f'-DRECOVERY_LOAD=0x{load:X} -DRECOVERY_ENTRY=0x{entry:X} -DRECOVERY_WORDS={padded//4}\n')
        if args.out:
            if not args.stub:p.error('--out requires --stub')
            stub=args.stub.read_bytes()
            if not stub or len(stub)>4096:raise ValueError('invalid recovery stub size')
            records=bytearray()
            for addr,data in [(0x80200000,stub),(0x80300000,staged),(0x80000000|romfs_pa,romfs)]:
                records+=struct.pack('<III',addr,len(data),0xFFFFFFFF)+data
            records+=struct.pack('<IIII',0,0x80200000,0,0xFFFFFFFF)
            image=b'B000FF\n'+struct.pack('<II',0x80200000,len(records))+records
            args.out.write_bytes(image)
            print(json.dumps({'image':str(args.out),'sha256':hashlib.sha256(image).hexdigest(),
                              'kernel_bytes':len(staged)-32,'romfs_pa':romfs_pa,
                              'nand':'raw repair; filesystem disabled' if args.nand_repair else 'read-only'}))
    except (ValueError,OSError) as e:p.exit(1,f'mkrecovery: {e}\n')

if __name__=='__main__':main()
