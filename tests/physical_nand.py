#!/usr/bin/env python3
"""Offline physical-image placement and preservation policy, real RS encoder."""
import copy
import importlib.util
import json
import os
import struct
import tempfile
import unittest
import zlib
from pathlib import Path
spec=importlib.util.spec_from_file_location('physical',Path(__file__).parents[1]/'tools/physical_nand.py')
n=importlib.util.module_from_spec(spec);spec.loader.exec_module(n)

class PhysicalNand(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp=tempfile.TemporaryDirectory(prefix='glaze-nand-test-')
        cls.path=Path(cls.tmp.name)/'backup.raw'
        cls.bad={1,321,350}; raw=bytearray(b'\xff'*n.RAW_SIZE)
        pages=[]; at=[]
        for b in range(1024):
            if b in cls.bad:
                for p in (0,1):raw[(b*32+p)*528+517]=0
            else:
                for p in range(5):
                    pages.append(b'\xff'*512+b'\xaa\x55\x0f\xff'+struct.pack('<I',b));at.append(b*32+p)
        encoded=n.encode(pages)
        for i,p in enumerate(at):raw[p*528:(p+1)*528]=encoded[i*528:(i+1)*528]
        cls.backup=bytes(raw);cls.path.write_bytes(raw);cls.inventory=n.inspect(cls.path)
        logical=bytearray(b'\xff'*(16*1024*1024))
        for i,(start,count) in enumerate([(0,32),(32,1),(160,1),(10240,22528),(512,8)]):
            struct.pack_into('<II',logical,i*16+8,start,count)
        record=struct.pack('<III',0x80f00000,4,0xffffffff)+bytes(4)+struct.pack('<IIII',0,0x80f00000,0,0xffffffff)
        spl=b'B000FF\n'+struct.pack('<II',0x80f00000,len(record))+record
        logical[0x4000:0x4000+len(spl)]=spl
        logical[0x40000:0x40008]=b'GZRF'+bytes(4)
        cls.logical=bytes(logical);cls.fs=b'\xff'*(n.FS_BLOCKS*n.DATA_BLOCK)

    @classmethod
    def tearDownClass(cls):cls.tmp.cleanup()

    def test_inventory_and_preservation(self):
        self.assertEqual(self.inventory['quarantined'],sorted(self.bad))
        raw,plan=n.build(self.logical,self.backup,self.inventory,self.fs)
        self.assertEqual(len(raw),n.RAW_SIZE)
        self.assertEqual(plan['boot_replacements'],{'1':1004})
        self.assertEqual(plan['fs_first']+plan['fs_blocks'],plan['reserve_first'])
        self.assertFalse(self.bad & set(plan['erase_blocks']))
        for b in self.bad:
            self.assertEqual(raw[b*n.BLOCK:(b+1)*n.BLOCK],self.backup[b*n.BLOCK:(b+1)*n.BLOCK])
        m=raw[528:528+512]
        self.assertEqual(m[:8],b'GZNAND1\0')
        self.assertEqual(struct.unpack_from('<I',m,508)[0],zlib.crc32(m[:508]))
        self.assertEqual(struct.unpack_from('<I',raw,3*16+12)[0],684*32)
        self.assertEqual(raw[1004*n.BLOCK+512:1004*n.BLOCK+520],b'\xaa\x55\x0f\xff'+struct.pack('<I',1))
        candidate=Path(self.tmp.name)/'candidate.raw';candidate.write_bytes(raw)
        n.native(['--check-boot',candidate])

    def test_fail_closed(self):
        for bad in ([0],[320,321],list(range(1,22))):
            inv=copy.deepcopy(self.inventory);inv['quarantined']=bad
            with self.assertRaises(ValueError):n.build(self.logical,self.backup,inv,self.fs)
        inv=copy.deepcopy(self.inventory);inv['source_sha256']='0'*64
        with self.assertRaises(ValueError):n.build(self.logical,self.backup,inv,self.fs)
        with self.assertRaises(ValueError):n.build(self.logical,self.backup,self.inventory,self.fs[:-1])
        fs=bytearray(self.fs);fs[(350-320)*n.DATA_BLOCK]=0
        with self.assertRaises(ValueError):n.build(self.logical,self.backup,self.inventory,fs)

    def test_logical_validation(self):
        n.validate_logical(self.logical)
        for offset in (0x4000,3*16+8,0x40004):
            broken=bytearray(self.logical);broken[offset]^=0x80
            with self.assertRaises(ValueError):n.validate_logical(broken)

    def test_private_no_overwrite(self):
        out=Path(self.tmp.name)/'private'
        n.write_new(out,b'one')
        self.assertEqual(os.stat(out).st_mode&0o777,0o600)
        with self.assertRaises(FileExistsError):n.write_new(out,b'two')
        self.assertEqual(out.read_bytes(),b'one')

if __name__=='__main__':unittest.main()
