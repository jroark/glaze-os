#!/usr/bin/env python3
"""Prepare a backup-bound NAND transaction for review; never opens a device.

Installation needs the physical packer's plan. Restoration needs a fresh exact
raw snapshot of the current NAND and the original CE backup/inventory. A failed
transaction is never resumed blindly or repaired by guessing new mappings.
"""
import argparse
import importlib.util
import json
import struct
import subprocess
import zlib
from pathlib import Path
spec=importlib.util.spec_from_file_location('physical',Path(__file__).with_name('physical_nand.py'))
n=importlib.util.module_from_spec(spec);spec.loader.exec_module(n)

def prepare(source,target,factory,inventory,placement=None,restore=False):
    if any(len(data)!=n.RAW_SIZE for data in (source,target,factory)):raise ValueError('all images must be exact physical 512+16 snapshots')
    if inventory.get('version')!=1 or inventory['source_sha256']!=n.sha(factory):raise ValueError('factory inventory is not bound to the original CE backup')
    bad=inventory['quarantined']
    if len(set(bad))!=len(bad) or any(type(b) is not int or not 0<=b<1024 for b in bad):raise ValueError('invalid quarantine list')
    if restore:
        if target!=factory:raise ValueError('restoration target must be the exact original CE backup')
    else:
        if source!=factory:raise ValueError('first installation must start from the original CE snapshot')
        if not placement or placement['backup_sha256']!=n.sha(factory) or placement['physical_sha256']!=n.sha(target):
            raise ValueError('physical placement plan is not bound to these images')
        if placement['preserve_blocks']!=sorted(bad) or set(placement['erase_blocks'])!=set(range(1024))-set(bad):
            raise ValueError('physical placement plan changes the quarantine policy')
        if 0 in bad:raise ValueError('physical boot block zero is quarantined')
        if target[528:536]!=b'GZNAND1\0':raise ValueError('installation target lacks the physical manifest')
    bitmap=bytearray(128)
    for block in bad:
        at=block*n.BLOCK
        if source[at:at+n.BLOCK]!=factory[at:at+n.BLOCK] or target[at:at+n.BLOCK]!=factory[at:at+n.BLOCK]:
            raise ValueError('transaction changes or cannot verify a quarantined block')
        bitmap[block//8]|=1<<(block%8)
    if not restore:
        manifest=target[528:1040]
        if (struct.unpack_from('<5I',manifest,8)!=(1,n.FS_FIRST,n.FS_BLOCKS,n.RESERVE,len(bad)) or
            manifest[28:156]!=bitmap or struct.unpack_from('<I',manifest,508)[0]!=zlib.crc32(manifest[:508])):
            raise ValueError('invalid manifest geometry, quarantine or CRC')
    changed=[b for b in reversed(range(1024)) if source[b*n.BLOCK:(b+1)*n.BLOCK]!=target[b*n.BLOCK:(b+1)*n.BLOCK]]
    plan=b'GZNDTX1\0'+bytes.fromhex(n.sha(source))+bytes.fromhex(n.sha(target))+bitmap
    plan+=struct.pack('<I',zlib.crc32(plan))
    review={'version':1,'operation':'restore' if restore else 'install','source_sha256':n.sha(source),
            'target_sha256':n.sha(target),'factory_sha256':n.sha(factory),'preserve_blocks':sorted(bad),
            'write_order':changed,'commit_block_zero_last':not changed or changed[-1]==0 or 0 not in changed,
            'artifact':'offline transaction; execution requires nand=repair and explicit --execute'}
    return plan,review

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('source','target','factory-backup','inventory','out'):
        p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--placement',type=Path);p.add_argument('--restore',action='store_true')
    args=p.parse_args()
    try:
        inventory=json.loads(args.inventory.read_text())
        if inventory!=n.inspect(args.factory_backup):raise ValueError('original backup inventory scan differs')
        if not args.restore:
            n.native(['--check-boot',args.target])
        plan,review=prepare(args.source.read_bytes(),args.target.read_bytes(),args.factory_backup.read_bytes(),inventory,
                            json.loads(args.placement.read_text()) if args.placement else None,args.restore)
        path=Path(str(args.out)+'.review.json')
        if args.out.exists() or path.exists():raise ValueError('output already exists')
        n.write_new(args.out,plan);n.write_new(path,(json.dumps(review,indent=2)+'\n').encode())
        print(f'Prepared {review["operation"]}: {len(review["write_order"])} blocks; no device access')
    except (ValueError,OSError,KeyError,subprocess.CalledProcessError) as e:p.exit(1,f'nand-transaction: {e}\n')

if __name__=='__main__':main()
