#!/usr/bin/env python3
"""Production CF formatting, ROM selection quirks and exact payload placement."""
import importlib.util
import mmap
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
s=importlib.util.spec_from_file_location('cf_recovery',ROOT/'tools/cf_recovery.py')
c=importlib.util.module_from_spec(s);s.loader.exec_module(c)

class RecoveryCF(unittest.TestCase):
    def test_formatter_geometry_and_payload(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp=Path(tmp);payload=b'B000FF\n'+bytes(range(256))*500
            source=tmp/'RECOVER.BIN';source.write_bytes(payload)
            for sectors in (32768,2001888):
                target=tmp/str(sectors)
                subprocess.run([str(ROOT/'out/host/mkcf-recovery'),str(sectors),str(source),str(target)],check=True)
                with target.open('rb') as f,mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ) as data:
                    report=c.verify(data,payload,sectors)
                    self.assertEqual(report['selected_fat_entry'],2)
                    self.assertEqual(report['volume_sectors'],sectors)
                    self.assertEqual(report['partition_lba'],0)
                    self.assertTrue(report['verified_contiguous_clusters']>1)
                    # Boot metadata and the payload suffice for inspection.
                    extent=report['rom_stream_lba']*512+((len(payload)+511)//512)*512
                    prefix=bytearray(data[:extent])
                # Generic FatFs labels are unsuitable for the ROM.
                broken=prefix.copy();broken[54:62]=b'FAT     '
                with self.assertRaises(ValueError):c.inspect(broken,sectors)
                broken=prefix.copy();broken[report['rom_stream_lba']*512+10]^=1
                with self.assertRaises(ValueError):c.verify(broken,payload,sectors)
                # The second FAT controls ROM selection. EOC-only entries
                # 2..9 cannot boot, even if RECOVER.BIN exists elsewhere.
                broken=prefix.copy();fat=report['second_fat_lba']*512
                for cluster in range(2,10):struct.pack_into('<H',broken,fat+cluster*2,0xffff)
                self.assertIsNone(c.inspect(broken,sectors)['rom_stream_lba'])
                broken=prefix.copy();broken[fat-512]^=1
                with self.assertRaises(ValueError):c.verify(broken,payload,sectors)

if __name__=='__main__':unittest.main()
