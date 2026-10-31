#!/usr/bin/env python3
"""Prepare a backup-bound NAND transaction for review; never opens a device.

Installation needs the physical packer's plan. Restoration needs a fresh exact
raw snapshot of the current NAND and the original CE backup/inventory. An
update replaces one installed Glaze image with another: --installed names the
source's own placement plan, which must be bound to the same CE backup.
--keep-filesystem updates a device whose file system has changed since: the
source is a fresh snapshot, --installed-image the image installed, and only the
blocks below the file system are replaced by the new image's. A failed
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

def bound(placement,factory,image,bad):
    return (placement and placement['backup_sha256']==n.sha(factory) and placement['physical_sha256']==n.sha(image)
            and placement['preserve_blocks']==sorted(bad) and set(placement['erase_blocks'])==set(range(1024))-set(bad))

def keep_filesystem(source,new,installed_image,factory,bad,placement,installed):
    """The target that puts new's boot, kernel and romfs blocks under the
    file system in source, a snapshot of installed_image after use."""
    if any(len(data)!=n.RAW_SIZE for data in (source,new,installed_image)):
        raise ValueError('all images must be exact physical 512+16 snapshots')
    if not bound(placement,factory,new,bad):raise ValueError('physical placement plan is not bound to the new image')
    if not bound(installed,factory,installed_image,bad):raise ValueError('installed placement plan is not bound to the installed image')
    boot,reserve=n.FS_FIRST*n.BLOCK,n.RESERVE*n.BLOCK
    if source[:boot]!=installed_image[:boot] or source[reserve:]!=installed_image[reserve:]:
        raise ValueError('the snapshot differs from the installed image outside its file system')
    if new[528:1040]!=installed_image[528:1040]:
        raise ValueError('the new image changes the physical manifest, so the file system would not carry over')
    return new[:boot]+source[boot:]

def prepare(source,target,factory,inventory,placement=None,restore=False,installed=None,kept=False):
    if any(len(data)!=n.RAW_SIZE for data in (source,target,factory)):raise ValueError('all images must be exact physical 512+16 snapshots')
    if inventory.get('version')!=1 or inventory['source_sha256']!=n.sha(factory):raise ValueError('factory inventory is not bound to the original CE backup')
    bad=inventory['quarantined']
    if len(set(bad))!=len(bad) or any(type(b) is not int or not 0<=b<1024 for b in bad):raise ValueError('invalid quarantine list')
    if restore:
        if target!=factory:raise ValueError('restoration target must be the exact original CE backup')
    elif not kept:  # keep_filesystem() bound both images
        if source!=factory and not bound(installed,factory,source,bad):
            raise ValueError('installation must start from the original CE snapshot or an installed image\'s placement plan')
        if not placement or placement['backup_sha256']!=n.sha(factory) or placement['physical_sha256']!=n.sha(target):
            raise ValueError('physical placement plan is not bound to these images')
        if placement['preserve_blocks']!=sorted(bad) or set(placement['erase_blocks'])!=set(range(1024))-set(bad):
            raise ValueError('physical placement plan changes the quarantine policy')
    if not restore:
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
    if kept and any(b>=n.FS_FIRST for b in changed):raise ValueError('a file-system-keeping update writes into the file system')
    plan=b'GZNDTX1\0'+bytes.fromhex(n.sha(source))+bytes.fromhex(n.sha(target))+bitmap
    plan+=struct.pack('<I',zlib.crc32(plan))
    review={'version':1,'operation':'restore' if restore else 'install' if source==factory else
            'update, file system kept' if kept else 'update','source_sha256':n.sha(source),
            'target_sha256':n.sha(target),'factory_sha256':n.sha(factory),'preserve_blocks':sorted(bad),
            'write_order':changed,'commit_block_zero_last':not changed or changed[-1]==0 or 0 not in changed,
            'artifact':'offline transaction; execution requires nand=repair and explicit --execute'}
    return plan,review

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('source','target','factory-backup','inventory','out'):
        p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--placement',type=Path);p.add_argument('--restore',action='store_true')
    p.add_argument('--installed',type=Path,help="the source image's placement plan, for an update")
    p.add_argument('--keep-filesystem',action='store_true',
                   help='keep the snapshot SOURCE\'s file system; writes the target to OUT.target.raw')
    p.add_argument('--installed-image',type=Path,help='with --keep-filesystem: the image --installed describes')
    args=p.parse_args()
    try:
        inventory=json.loads(args.inventory.read_text())
        if inventory!=n.inspect(args.factory_backup):raise ValueError('original backup inventory scan differs')
        if not args.restore:
            n.native(['--check-boot',args.target])
        source,target,factory=args.source.read_bytes(),args.target.read_bytes(),args.factory_backup.read_bytes()
        placement=json.loads(args.placement.read_text()) if args.placement else None
        installed=json.loads(args.installed.read_text()) if args.installed else None
        kept_path=Path(str(args.out)+'.target.raw')
        if args.keep_filesystem:
            if args.restore or not args.installed_image:raise ValueError('--keep-filesystem needs --installed-image and no --restore')
            target=keep_filesystem(source,target,args.installed_image.read_bytes(),factory,inventory['quarantined'],
                                   placement,installed)
        plan,review=prepare(source,target,factory,inventory,placement,args.restore,installed,args.keep_filesystem)
        path=Path(str(args.out)+'.review.json')
        if args.out.exists() or path.exists() or (args.keep_filesystem and kept_path.exists()):
            raise ValueError('output already exists')
        if args.keep_filesystem:
            n.write_new(kept_path,target);review['target_image']=str(kept_path)
        n.write_new(args.out,plan);n.write_new(path,(json.dumps(review,indent=2)+'\n').encode())
        print(f'Prepared {review["operation"]}: {len(review["write_order"])} blocks; no device access')
    except (ValueError,OSError,KeyError,subprocess.CalledProcessError) as e:p.exit(1,f'nand-transaction: {e}\n')

if __name__=='__main__':main()
