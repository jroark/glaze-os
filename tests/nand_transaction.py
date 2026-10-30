#!/usr/bin/env python3
"""Offline install/restore plan binding and preservation rules."""
import importlib.util
import struct
import unittest
import zlib
from pathlib import Path
s=importlib.util.spec_from_file_location('transaction',Path(__file__).parents[1]/'tools/nand_transaction.py')
t=importlib.util.module_from_spec(s);s.loader.exec_module(t)
n=t.n

class Transaction(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.factory=b'\xff'*n.RAW_SIZE
        cls.inventory={'version':1,'source_sha256':n.sha(cls.factory),'quarantined':[20]}
        target=bytearray(cls.factory)
        target[528:1040]=n.manifest([20],'00'*32)
        target[33*n.BLOCK]=123;cls.target=bytes(target)
        cls.placement={'backup_sha256':n.sha(cls.factory),'physical_sha256':n.sha(cls.target),
                       'preserve_blocks':[20],'erase_blocks':[b for b in range(1024) if b!=20]}

    def test_install_and_restore(self):
        plan,review=t.prepare(self.factory,self.target,self.factory,self.inventory,self.placement)
        self.assertEqual(len(plan),204)
        self.assertEqual(struct.unpack_from('<I',plan,200)[0],zlib.crc32(plan[:200]))
        self.assertEqual(review['write_order'],[33,0])
        self.assertEqual(plan[8:40],bytes.fromhex(n.sha(self.factory)))
        _,review=t.prepare(self.target,self.factory,self.factory,self.inventory,restore=True)
        self.assertEqual(review['write_order'],[33,0]);self.assertEqual(review['operation'],'restore')

    def test_source_target_and_inventory_binding(self):
        with self.assertRaises(ValueError):t.prepare(self.target,self.target,self.factory,self.inventory,self.placement)
        with self.assertRaises(ValueError):t.prepare(self.factory,self.target,self.factory,self.inventory,restore=True)
        inv={**self.inventory,'source_sha256':'00'*32}
        with self.assertRaises(ValueError):t.prepare(self.factory,self.target,self.factory,inv,self.placement)
        placement={**self.placement,'physical_sha256':'00'*32}
        with self.assertRaises(ValueError):t.prepare(self.factory,self.target,self.factory,self.inventory,placement)
        broken=bytearray(self.target);broken[20*n.BLOCK]=0
        placement={**self.placement,'physical_sha256':n.sha(broken)}
        with self.assertRaises(ValueError):t.prepare(self.factory,broken,self.factory,self.inventory,placement)

    def test_manifest_and_noop(self):
        broken=bytearray(self.target);broken[528+508]^=1
        placement={**self.placement,'physical_sha256':n.sha(broken)}
        with self.assertRaises(ValueError):t.prepare(self.factory,broken,self.factory,self.inventory,placement)
        _,review=t.prepare(self.factory,self.factory,self.factory,self.inventory,restore=True)
        self.assertEqual(review['write_order'],[])

if __name__=='__main__':unittest.main()
