#!/usr/bin/env python3
"""Generate a synthetic PE covering the CE caller ABI, traps and API probes.

No vendor assets are needed. Build Glaze, run this script, then:
  python3 tests/run_wince.py --package out/wince-abi.gzp --headless --seconds 14 --expect-exit 37
"""
from pathlib import Path
import struct
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'tools'))
from import_wince import package, validate_runner


def executable():
    pe = bytearray(8704)
    def u16(off, value):
        struct.pack_into('<H', pe, off, value)
    def u32(off, value):
        struct.pack_into('<I', pe, off, value)
    pe[:2] = b'MZ'
    u32(60, 128)
    pe[128:132] = b'PE\0\0'
    u16(132, 0x166)
    u16(134, 2)
    u16(148, 224)
    u16(150, 0x102)
    u16(152, 0x10b)
    u32(168, 0x1000)  # entry RVA
    u32(180, 0x10000)
    u32(184, 4096)
    u32(188, 512)
    u32(208, 12288)
    u32(212, 512)
    u16(220, 9)
    u32(224, 65536)
    u32(244, 16)
    u32(256, 0x2000)
    u32(260, 40)
    pe[376:384] = b'.text\0\0\0'
    u32(384, 4096)
    u32(388, 4096)
    u32(392, 4096)
    u32(396, 512)
    u32(412, 0x60000020)
    pe[416:424] = b'.data\0\0\0'
    u32(424, 4096)
    u32(428, 8192)
    u32(432, 4096)
    u32(436, 4608)
    u32(452, 0xc0000040)
    cursor = 0x2100
    def data(payload):
        nonlocal cursor
        start = cursor
        pe[4608 + start - 8192:4608 + start - 8192 + len(payload)] = payload
        cursor = (cursor + len(payload) + 3) & ~3
        return 0x10000 + start
    def wide(text):
        return data((text + "\0").encode('utf-16le'))
    imports = ['SipSetInfo', 'GetModuleHandleW', 'GetProcAddressW', 'GetCurrentThreadId',
               'CreateDIBSection', 'GetObjectW', 'DeleteObject', 'CreateCompatibleDC',
               'SelectObject', 'PatBlt', 'DeleteDC']
    for i, name in enumerate(imports):
        hint = data(b'\0\0' + name.encode() + b'\0') - 0x10000
        u32(4608 + 0x40 + i * 4, hint)
        u32(4608 + 0x80 + i * 4, hint)
    u32(4608, 0x2040)
    u32(4608 + 12, data(b'COREDLL.dll\0') - 0x10000)
    u32(4608 + 16, 0x2080)
    sip = data(struct.pack('<I', 48) + bytes(44))
    core = wide('coredll.dll')
    absent = wide('NoSuchCEFunction')
    tick = wide('GetTickCount')
    # Nine monochrome pixels require a DWORD-aligned four-byte scan line.
    dib = data(struct.pack('<IiiHHIIIIII', 40, 9, -2, 1, 1, 0, 0, 0, 0, 0, 0) +
               b'\0\0\0\0\xff\xff\xff\0')
    bitmap = data(bytes(36))  # output bits pointer, handle, BITMAP, DC
    code, fixups, labels = [], [], {}
    def emit(*words):
        code.extend(words)
    def load(reg, value):
        emit(0x3c000000 | reg << 16 | value >> 16,
             0x34000000 | reg << 21 | reg << 16 | (value & 65535))
    def call(name):
        load(25, 0x12080 + imports.index(name) * 4)
        emit(0x8f390000, 0x0320f809, 0)
    def branch(op, rs, rt, label):
        fixups.append((len(code), label))
        emit(op << 26 | rs << 21 | rt << 16, 0)
    # Only sixteen argument bytes, then saved s0 and ra: sibling calls from
    # twelve-word adapters must never write unused args into this frame.
    emit(0x27bdffe8, 0xafb00010, 0xafbf0014)
    load(4, sip)
    call('SipSetInfo')
    load(4, core)
    call('GetModuleHandleW')
    branch(4, 2, 0, 'fail')
    emit(0x00408025)  # s0 = core handle
    emit(0x02002025)  # a0 = s0
    load(5, absent)
    call('GetProcAddressW')
    branch(5, 2, 0, 'fail')
    emit(0x02002025)
    load(5, tick)
    call('GetProcAddressW')
    branch(4, 2, 0, 'fail')
    emit(0x0040c825, 0x0320f809, 0)  # call returned tick bridge
    emit(0x02002025)
    load(5, 123)  # package's matching ordinal map
    call('GetProcAddressW')
    branch(4, 2, 0, 'fail')
    emit(0x0040c825, 0x0320f809, 0)
    call('GetCurrentThreadId')
    emit(0x8c0e5808)  # inline KData current-thread read
    branch(5, 2, 14, 'fail')
    load(4, 73)
    emit(0x240efaa2, 0x01c0f809, 0)  # embedded SetLastError
    emit(0x240efa9e, 0x01c0f809, 0)  # embedded GetLastError
    load(14, 73)
    branch(5, 2, 14, 'fail')
    # Six-argument API: explicitly reserve the two stack arguments.
    emit(0x27bdffe0, 0xafa00010, 0xafa00014)
    load(4, 0)
    load(5, dib)
    load(6, 0)
    load(7, bitmap)
    call('CreateDIBSection')
    branch(4, 2, 0, 'dib_fail')
    load(8, bitmap)
    emit(0xad020004, 0x8d090000)  # keep handle; returned writable pixels
    branch(4, 9, 0, 'dib_fail')
    emit(0x00402025)
    load(5, 24)
    load(6, bitmap + 8)
    call('GetObjectW')
    load(14, 24)
    branch(5, 2, 14, 'dib_fail')
    load(8, bitmap)
    emit(0x8d090014)  # bmWidthBytes
    load(14, 4)
    branch(5, 9, 14, 'dib_fail')
    emit(0x8d090000, 0x8d0e001c)  # bmBits must be the writable pointer
    branch(5, 9, 14, 'dib_fail')
    load(4, 0)
    call('CreateCompatibleDC')
    branch(4, 2, 0, 'dib_fail')
    load(8, bitmap)
    emit(0xad020020, 0x00402025, 0x8d050004)
    call('SelectObject')
    load(8, bitmap)
    emit(0x8d040020)
    load(5, 0)
    load(6, 0)
    load(7, 9)
    load(14, 2)
    emit(0xafae0010)
    load(14, 0x00ff0062)  # WHITENESS: exercises monochrome write and row padding
    emit(0xafae0014)
    call('PatBlt')
    branch(4, 2, 0, 'dib_fail')
    load(8, bitmap)
    emit(0x8d090000, 0x912a0000)
    load(14, 255)
    branch(5, 10, 14, 'dib_fail')
    emit(0x912a0001)
    load(14, 128)
    branch(5, 10, 14, 'dib_fail')
    emit(0x912a0004)
    load(14, 255)
    branch(5, 10, 14, 'dib_fail')
    emit(0x912a0002)
    branch(5, 10, 0, 'dib_fail')
    emit(0x8d040020)
    call('DeleteDC')
    load(8, bitmap)
    emit(0x8d040004)
    call('DeleteObject')
    branch(4, 2, 0, 'dib_fail')
    emit(0x27bd0020)
    load(2, 37)
    branch(4, 0, 0, 'return')
    labels['fail'] = len(code)
    load(2, 1)
    branch(4, 0, 0, 'return')
    labels['dib_fail'] = len(code)
    emit(0x27bd0020)
    load(2, 2)
    labels['return'] = len(code)
    emit(0x8fb00010, 0x8fbf0014, 0x27bd0018, 0x03e00008, 0)
    for i, label in fixups:
        code[i] |= (labels[label] - i - 1) & 65535
    assert len(code) * 4 <= 0x1000
    struct.pack_into('<' + 'I' * len(code), pe, 512, *code)
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
        'res/ordinals.txt': b'coredll.dll 123 GetTickCount\n',
        'res/icon.gzi': icon,
    }))
    print(output)


if __name__ == '__main__':
    main()
