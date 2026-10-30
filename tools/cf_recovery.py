#!/usr/bin/env python3
"""Inspect a read-only CF prefix using the BE-300 masked ROM's FAT16 rules.

The ROM does not traverse directories or follow a file's FAT chain. It scans
entries 2..9 in the SECOND FAT's first sector, then streams consecutive sectors.
This reports the selected stream; it never edits an image or physical card.
"""
import argparse
import json
import hashlib
import mmap
import struct
from pathlib import Path

def u16(b,off):return struct.unpack_from('<H',b,off)[0]
def u32(b,off):return struct.unpack_from('<I',b,off)[0]

def inspect(data,sectors=None):
    if len(data)<512 or len(data)%512:raise ValueError('snapshot must contain whole 512-byte sectors')
    sector=data[:512]
    if sector[510:512]!=b'\x55\xaa':raise ValueError('missing sector-zero signature')
    part=0
    if u16(sector,11)!=512:
        if any(sector[446+i*16]&0x7f for i in range(4)):
            raise ValueError('partition status unsupported by ROM')
        part=u32(sector,454)
        if not part or part>65535:raise ValueError('ROM reads only the low 16 bits of first partition LBA')
        if len(data)<(part+1)*512:return {'complete':False,'needs_prefix_sectors':part+1,'partition_lba':part}
        sector=data[part*512:(part+1)*512]
    if sector[510:512]!=b'\x55\xaa' or u16(sector,11)!=512:raise ValueError('invalid FAT boot sector')
    spc=sector[13];reserved=u16(sector,14);fats=sector[16];roots=u16(sector,17);spf=u16(sector,22)
    total=u16(sector,19) or u32(sector,32)
    if sector[54:59]!=b'FAT16':raise ValueError('only the ROM FAT16 path is validated for recovery')
    if not spc or spc&(spc-1) or not reserved or reserved>255 or fats!=2 or not roots or roots%16 or not spf:
        raise ValueError('geometry unsupported by ROM (512-byte sectors, two FATs, one-byte reserved count)')
    first=part+reserved+2*spf+roots//16
    if total<=first-part or (sectors is not None and part+total>sectors):raise ValueError('volume exceeds card geometry')
    clusters=(total-(first-part))//spc
    if not 4085<=clusters<=65524 or spf*256<clusters+2:raise ValueError('invalid FAT16 cluster/table geometry')
    fat=part+reserved+spf
    report={'complete':False,'partition_lba':part,'sectors_per_cluster':spc,
            'second_fat_lba':fat,'data_lba':first,'volume_sectors':total,
            'needs_prefix_sectors':fat+1}
    if len(data)<(fat+1)*512:return report
    # ROM 9FC01004 scans entries 2..9; 9FC00FE8 returns next-3, or -1.
    selected=None
    for cluster in range(2,10):
        value=u16(data,fat*512+cluster*2)
        if 3<=value<0xff8 and value!=0xff:
            selected=(cluster,value);break
    if selected is None:
        report.update(complete=True,rom_stream_lba=None,reason='ROM finds no candidate in FAT entries 2..9')
        return report
    cluster,value=selected
    lba=first+(value-3)*spc
    if lba>=part+total:raise ValueError('ROM-selected sector is outside volume')
    report.update(selected_fat_entry=cluster,selected_fat_value=value,rom_stream_lba=lba,
                  needs_prefix_sectors=lba+1,
                  contiguous_hint=value==cluster+1)
    if len(data)<(lba+1)*512:return report
    report.update(complete=True,b000ff=data[lba*512:lba*512+7]==b'B000FF\n')
    return report

def verify(data,recovery,sectors=None):
    report=inspect(data,sectors)
    if not report.get('complete') or not report.get('b000ff') or not report.get('contiguous_hint'):
        raise ValueError('ROM does not select a contiguous B000FF recovery stream')
    if not recovery.startswith(b'B000FF\n'):raise ValueError('invalid recovery container')
    start=report['rom_stream_lba']*512
    if start+len(recovery)>len(data):raise ValueError('snapshot does not contain the full recovery stream')
    if data[start:start+len(recovery)]!=recovery:raise ValueError('ROM stream differs from the recovery payload')
    bpb=report['partition_lba']*512
    spf=u16(data,bpb+22)
    fat2=report['second_fat_lba']*512
    if data[fat2-spf*512:fat2]!=data[fat2:fat2+spf*512]:raise ValueError('FAT copies differ')
    size=report['sectors_per_cluster']*512
    clusters=(len(recovery)+size-1)//size
    first=report['selected_fat_entry']
    for cluster in range(first,first+clusters):
        value=u16(data,fat2+cluster*2)
        if (cluster<first+clusters-1 and value!=cluster+1) or (cluster==first+clusters-1 and value<0xfff8):
            raise ValueError('recovery FAT chain is fragmented or has the wrong extent')
    report.update(recovery_sha256=hashlib.sha256(recovery).hexdigest(),recovery_bytes=len(recovery),
                  verified_contiguous_clusters=clusters)
    return report

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('snapshot',type=Path);p.add_argument('--sectors',type=int)
    p.add_argument('--recovery',type=Path,help='verify the complete ROM stream and both FAT copies against this payload')
    args=p.parse_args()
    try:
        with args.snapshot.open('rb') as f,mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ) as data:
            result=verify(data,args.recovery.read_bytes(),args.sectors) if args.recovery else inspect(data,args.sectors)
        print(json.dumps(result,indent=2))
    except (ValueError,OSError) as e:p.exit(1,f'cf-recovery: {e}\n')

if __name__=='__main__':main()
