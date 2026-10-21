#!/usr/bin/env python3
"""Generate a synthetic MIPS PE that checks a short caller's saved return address.

No vendor assets are needed. Build Glaze, run this script, then:
  python3 tests/run_wince.py --package out/wince-abi.gzp --headless --seconds 14 --expect-exit 37
"""
from pathlib import Path
import struct
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'tools'))
from import_wince import package, validate_runner


def executable():
    pe = bytearray(1536)
    def u16(off, value):
        struct.pack_into('<H', pe, off, value)
    def u32(off, value):
        struct.pack_into('<I', pe, off, value)
    pe[:2] = b'MZ'
    u32(60, 128)
    pe[128:132] = b'PE\0\0'
    u16(132, 0x166)
    u16(134, 1)
    u16(148, 224)
    u16(150, 0x102)
    u16(152, 0x10b)
    u32(168, 0x1000)  # entry RVA
    u32(180, 0x10000)
    u32(184, 4096)
    u32(188, 512)
    u32(208, 8192)
    u32(212, 512)
    u16(220, 9)
    u32(224, 65536)
    u32(244, 16)
    u32(256, 0x1200)
    u32(260, 40)
    pe[376:384] = b'.text\0\0\0'
    u32(384, 4096)
    u32(388, 4096)
    u32(392, 1024)
    u32(396, 512)
    u32(412, 0x60000020)
    # The caller reserves only sixteen argument bytes, then its saved ra.
    # A tail call from SipSetInfo to the twelve-word SipShowIM adapter used
    # to overwrite ra with an unused zero argument, and jump to address zero.
    code = [0x27bdffe8, 0xafbf0014,
            0x3c040001, 0x348412e0,
            0x3c190001, 0x8f391260,
            0x0320f809, 0,
            0x8fbf0014, 0x27bd0018,
            0x03e00008, 0x24020025]
    struct.pack_into('<' + 'I' * len(code), pe, 512, *code)
    u32(1024, 0x1240)
    u32(1036, 0x1280)
    u32(1040, 0x1260)
    u32(1088, 0x12a0)
    u32(1120, 0x12a0)
    pe[1152:1164] = b'COREDLL.dll\0'
    pe[1186:1197] = b'SipSetInfo\0'
    u32(1248, 48)
    return bytes(pe)


def main():
    root = Path(__file__).resolve().parent.parent
    runner = (root / 'out/romfs/bin/wince').read_bytes()
    validate_runner(runner)
    icon = b'GZI1' + struct.pack('<4H', 44, 44, 0, 0) + b'\xff\xff' * (44 * 44)
    output = root / 'out/wince-abi.gzp'
    output.write_bytes(package({
        'Info': b'id=wince.abi\nname=CE ABI Test\nversion=1.0\nexec=app\n',
        'app': runner,
        'res/program.exe': executable(),
        'res/ordinals.txt': b'1 Unused\n',
        'res/icon.gzi': icon,
    }))
    print(output)


if __name__ == '__main__':
    main()
