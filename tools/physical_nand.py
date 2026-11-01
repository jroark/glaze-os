#!/usr/bin/env python3
"""Create an OFFLINE physical NAND candidate and reviewable block plan.

Requires a complete 512+16 backup and its conservative tag/ECC inventory.
No device access, erase or programming capability is provided here.
"""
import argparse
import hashlib
import json
import os
import struct
import subprocess
import tempfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PAGES = 32768
RAW_PAGE = 528
BLOCK = 32 * RAW_PAGE
DATA_BLOCK = 32 * 512
RAW_SIZE = PAGES * RAW_PAGE
FS_FIRST, FS_BLOCKS, RESERVE = 320, 684, 1004
HELPER = Path(os.environ.get('GLAZE_NAND_HELPER', ROOT / 'out/host/nand-inventory'))
MKFS = Path(os.environ.get('GLAZE_MKFS_HELPER', ROOT / 'host/mkfsimg'))


def sha(data):
    return hashlib.sha256(data).hexdigest()


def native(args, data=None):
    return subprocess.run([str(HELPER), *map(str, args)], input=data,
                          stdout=subprocess.PIPE, check=True).stdout


def inspect(backup):
    data = Path(backup).read_bytes()
    if len(data) != RAW_SIZE:
        raise ValueError('backup must contain exactly 32768 physical 512+16 pages')
    if data[528:536] == b'GZNAND1\0':
        raise ValueError('v1 inventory requires a CE source; Glaze updates must also import the runtime BBT')
    bitmap = native([backup])
    if len(bitmap) != 128:
        raise ValueError('invalid inventory helper response')
    return {'version': 1, 'classification': 'CE tag/ECC; untrusted blocks quarantined',
            'source_sha256': sha(data),
            'quarantined': [b for b in range(1024) if bitmap[b//8] & (1 << (b%8))]}


def encode(pages):
    encoded = native(['--encode'], b''.join(pages))
    if len(encoded) != len(pages) * RAW_PAGE:
        raise ValueError('encoder returned incomplete physical pages')
    return encoded


def manifest(bad, digest):
    page = bytearray(b'\xff' * 512)
    page[:8] = b'GZNAND1\0'
    struct.pack_into('<5I', page, 8, 1, FS_FIRST, FS_BLOCKS, RESERVE, len(bad))
    page[28:156] = bytes(128)
    for b in bad:
        page[28+b//8] |= 1 << (b%8)
    page[156:188] = bytes.fromhex(digest)
    struct.pack_into('<I', page, 508, zlib.crc32(page[:508]))
    return page


def validate_logical(data):
    if len(data) != 16*1024*1024:
        raise ValueError('logical image must be exactly 16 MiB')
    expected = [(0,32,32),(32,1,128),(160,1,352),(10240,1,22528),(512,1,9728)]
    for i,(start,minimum,maximum) in enumerate(expected):
        first,count=struct.unpack_from('<II',data,i*16+8)
        if first != start or not minimum <= count <= maximum:
            raise ValueError(f'invalid logical partition {i}')
    if data[0x4000:0x4007] != b'B000FF\n' or data[0x40000:0x40004] != b'GZRF':
        raise ValueError('missing SPL container or romfs signature')
    start,length=struct.unpack_from('<II',data,0x4007)
    spl_pages=struct.unpack_from('<I',data,16+12)[0]
    if start != 0x80F00000 or not 16 <= length <= spl_pages*512-15:
        raise ValueError('invalid SPL record extent')
    pos,end=0x400F,0x400F+length
    ranges=[]
    while pos+12 <= end:
        address,size,_=struct.unpack_from('<III',data,pos);pos+=12
        if not address:
            if not any(a <= size < b for a,b in ranges):
                raise ValueError('SPL entry is outside its loaded records')
            break
        if not size or address < 0x80F00000 or address+size > 0x80FF0000 or pos+size>end:
            raise ValueError('invalid SPL load record')
        if any(address<b and a<address+size for a,b in ranges):
            raise ValueError('overlapping SPL load records')
        ranges.append((address,address+size));pos+=size
    else:
        raise ValueError('SPL record terminator missing')
    archive_pages=struct.unpack_from('<I',data,4*16+12)[0]
    count=struct.unpack_from('<I',data,0x40004)[0]
    if count>256 or 8+count*56>archive_pages*512:
        raise ValueError('invalid romfs table extent')
    for i in range(count):
        off,size=struct.unpack_from('<II',data,0x40008+i*56+48)
        if off%4096 or off+size>archive_pages*512:
            raise ValueError('invalid romfs file extent')


def build(logical, backup, inventory, filesystem):
    validate_logical(logical)
    if len(backup) != RAW_SIZE:
        raise ValueError('invalid logical image or physical backup geometry')
    if inventory.get('version') != 1 or inventory.get('source_sha256') != sha(backup):
        raise ValueError('inventory is not bound to this exact backup')
    bad = inventory['quarantined']
    if any(type(b) is not int or b < 0 or b >= 1024 for b in bad) or len(set(bad)) != len(bad):
        raise ValueError('invalid quarantined block list')
    bad = set(bad)
    if 0 in bad:
        raise ValueError('physical block 0 is quarantined; ROM recovery/placement is not established')
    if FS_FIRST in bad and FS_FIRST+1 in bad:
        raise ValueError('both filesystem BBT blocks are quarantined')
    if len(filesystem) != FS_BLOCKS*DATA_BLOCK:
        raise ValueError('filesystem must be freshly built for 684 blocks, not a truncated legacy image')
    for b in bad & set(range(FS_FIRST, RESERVE)):
        if filesystem[(b-FS_FIRST)*DATA_BLOCK:(b-FS_FIRST+1)*DATA_BLOCK] != b'\xff'*DATA_BLOCK:
            raise ValueError('filesystem was not created with this bad-block inventory')
    pool = iter(b for b in range(RESERVE, 1024) if b not in bad)
    mapping = {}
    for b in range(FS_FIRST):
        try:
            mapping[b] = next(pool) if b in bad else b
        except StopIteration:
            raise ValueError('not enough good reserved blocks for immutable boot content') from None
    digest = sha(json.dumps(inventory, sort_keys=True, separators=(',', ':')).encode())
    source = bytearray(logical)
    if struct.unpack_from('<I', source, 3*16+8)[0] != FS_FIRST*32:
        raise ValueError('unexpected filesystem partition location')
    struct.pack_into('<I', source, 3*16+12, FS_BLOCKS*32)
    source[512:1024] = manifest(bad, digest)
    raw = bytearray(backup)  # Quarantined blocks retain ALL original data/OOB.
    erased = b'\xff'*BLOCK
    for b in range(1024):
        if b not in bad:
            raw[b*BLOCK:(b+1)*BLOCK] = erased
    pages = []
    for logical_block in range(FS_FIRST):
        tag = b'\xaa\x55\x0f\xff' + struct.pack('<I', logical_block)
        for within in range(32):
            at = (logical_block*32+within)*512
            pages.append(bytes(source[at:at+512]) + tag)
    boot = encode(pages)
    for b, physical in mapping.items():
        raw[physical*BLOCK:(physical+1)*BLOCK] = boot[b*BLOCK:(b+1)*BLOCK]
    for b in range(FS_FIRST, RESERVE):
        if b in bad:
            continue
        for within in range(32):
            src = ((b-FS_FIRST)*32+within)*512
            dst = (b*32+within)*RAW_PAGE
            raw[dst:dst+512] = filesystem[src:src+512]
    plan = {'version': 1, 'artifact': 'offline candidate; not a flash command',
            'backup_sha256': sha(backup), 'inventory_sha256': digest,
            'logical_sha256': sha(logical), 'filesystem_sha256': sha(filesystem),
            'physical_sha256': sha(raw), 'raw_page_bytes': RAW_PAGE,
            'fs_first': FS_FIRST, 'fs_blocks': FS_BLOCKS, 'reserve_first': RESERVE,
            'preserve_blocks': sorted(bad),
            'erase_blocks': [b for b in range(1024) if b not in bad],
            'boot_replacements': {str(b): p for b, p in mapping.items() if b != p}}
    return raw, plan


def write_new(path, data):
    # Publish verified local artifacts without overwriting existing files.
    path = Path(path)
    if path.exists():
        raise FileExistsError(path)
    partial = Path(str(path)+'.partial')
    fd = os.open(partial, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, 'wb') as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    os.link(partial, path)
    partial.unlink()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    inv = commands.add_parser('inventory')
    inv.add_argument('--backup', type=Path, required=True)
    inv.add_argument('--out', type=Path, required=True)
    pack = commands.add_parser('pack')
    for name in ('logical', 'backup', 'inventory', 'out', 'plan'):
        pack.add_argument('--'+name, type=Path, required=True)
    pack.add_argument('--userdata', type=Path, default=ROOT/'userdata')
    args = parser.parse_args()
    try:
        if args.command == 'inventory':
            write_new(args.out, (json.dumps(inspect(args.backup), indent=2)+'\n').encode())
            return
        if args.out.exists() or args.plan.exists():
            raise ValueError('output/plan already exists; choose new paths')
        inventory = json.loads(args.inventory.read_text())
        observed = inspect(args.backup)
        if inventory != observed:
            raise ValueError('inventory differs from the current backup scan')
        bitmap = bytearray(128)
        for b in inventory['quarantined']:
            if FS_FIRST <= b < RESERVE:
                rel = b-FS_FIRST
                bitmap[rel//8] |= 1 << (rel%8)
        with tempfile.TemporaryDirectory(prefix='glaze-physical-') as tmp:
            tmp = Path(tmp)
            badfile, fsfile, candidate = tmp/'bad.bin', tmp/'fs.img', tmp/'candidate.raw'
            badfile.write_bytes(bitmap)
            seed = tmp/'net.entropy'; seed.write_bytes(os.urandom(32))
            subprocess.run([str(MKFS), str(args.userdata), str(FS_BLOCKS),
                            str(fsfile), '--bad-blocks='+str(badfile),
                            '/Library/Preferences/net.entropy='+str(seed)], check=True)
            raw, plan = build(args.logical.read_bytes(), args.backup.read_bytes(), inventory, fsfile.read_bytes())
            candidate.write_bytes(raw)
            native(['--check-boot', candidate])
            write_new(args.out, raw)
            write_new(args.plan, (json.dumps(plan, indent=2)+'\n').encode())
        print(f'Created {args.out}: {plan["physical_sha256"]}; no device writes performed')
    except (ValueError, OSError, subprocess.CalledProcessError, KeyError) as exc:
        parser.exit(1, f'physical-nand: {exc}\n')


if __name__ == '__main__':
    main()
