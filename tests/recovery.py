#!/usr/bin/env python3
"""Reject recovery layouts that could overwrite the live ROM or copy stub."""
import importlib.util
import struct
import unittest
from pathlib import Path
spec=importlib.util.spec_from_file_location('recovery',Path(__file__).parents[1]/'tools/mkrecovery.py')
r=importlib.util.module_from_spec(spec);spec.loader.exec_module(r)

def kernel(load=0x80010000,entry=0x80010000,size=101,end=0x80100000):
    return struct.pack('<5I',int.from_bytes(b'GZKB','little'),load,entry,size,end)+bytes(4076+size)

class Recovery(unittest.TestCase):
    def test_staging_and_readonly_bootinfo(self):
        k=kernel();romfs=b'GZRF'+bytes(4)
        load,entry,padded,pa,staged=r.layout(k,romfs)
        self.assertEqual((load,entry,padded),(0x80010000,0x80010000,104))
        self.assertEqual(staged[:101],k[4096:])
        self.assertEqual(staged[101:104],bytes(3))
        magic,version,flags,rom_pa,rom_size,ram_pa,ram_blocks,reserved=struct.unpack('<8I',staged[104:])
        self.assertEqual((magic,version,flags,reserved),(0x42525a47,1,8,0))
        self.assertEqual((rom_pa,rom_size),(pa,len(romfs)))
        self.assertEqual((ram_pa,ram_blocks),(pa-2*1024*1024,128))
        self.assertEqual(struct.unpack_from('<I',r.layout(k,romfs,True)[4],104+8)[0],12)

    def test_unsafe_kernel_layout(self):
        for k in (kernel(load=0x80000000),kernel(entry=0x80010001),kernel(entry=0x80020000),
                  kernel(load=0x80200000,entry=0x80200000),kernel(end=0x81000001),
                  kernel(end=0x80010001),kernel()[:-1],bytes(4096)):
            with self.assertRaises(ValueError):r.layout(k,b'GZRF'+bytes(4))

    def test_invalid_or_overlapping_romfs(self):
        table=bytearray(64);table[:8]=b'GZRF'+struct.pack('<I',1)
        struct.pack_into('<II',table,56,0,1)
        for romfs in (b'GZRF',b'BAD!'+bytes(4),bytes(table),b'GZRF'+struct.pack('<I',257),
                      b'GZRF'+bytes(15*1024*1024)):
            with self.assertRaises(ValueError):r.layout(kernel(),romfs)

if __name__=='__main__':unittest.main()
