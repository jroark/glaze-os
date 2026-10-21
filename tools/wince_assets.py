#!/usr/bin/env python3
"""Read local BE-300 assets without modifying the source NAND image.

CPK observations come from CCFS.DLL in the supplied image: ReadFileWithSeek
dispatches block types 0..16 through its UnCompFunc table. Huffman's word
dictionary belongs to that driver and is read locally, never redistributed.
No third-party Python packages are required.
"""

import struct


class AssetError(ValueError):
    pass


def span(data, off, size):
    if off < 0 or size < 0 or off > len(data) - size:
        raise AssetError(f"range outside asset: offset={off:#x}, size={size:#x}")
    return data[off:off + size]


def u16(data, off):
    return int.from_bytes(span(data, off, 2), "little")


def u32(data, off):
    return int.from_bytes(span(data, off, 4), "little")


def cstr(data, off):
    span(data, off, 1)
    end = data.find(b"\0", off, min(off + 256, len(data)))
    if end < 0:
        raise AssetError("unterminated asset string")
    try:
        return data[off:end].decode("ascii")
    except UnicodeDecodeError as exc:
        raise AssetError("non-ASCII asset string") from exc


def lzss(data, size=None, limit=32 * 1024 * 1024):
    """Casio SPL/CPK LZSS, zero history, absolute 12-bit ring references."""
    ring = bytearray(4096)
    out = bytearray()
    cursor, flags, pos = 0xfee, 0, 0
    while pos < len(data) and (size is None or len(out) < size):
        flags >>= 1
        if not flags & 0x100:
            flags = data[pos] | 0xff00
            pos += 1
        if flags & 1:
            values = span(data, pos, 1)
            pos += 1
            ref = None
        else:
            lo, hi = span(data, pos, 2)
            pos += 2
            ref = lo | (hi & 0xf0) << 4
            values = range((hi & 15) + 3)
        for i in values:
            value = i if ref is None else ring[(ref + i) & 4095]
            out.append(value)
            ring[cursor] = value
            cursor = (cursor + 1) & 4095
            if len(out) > limit or (size is not None and len(out) > size):
                raise AssetError("LZSS output exceeds declared size")
    if size is not None and len(out) != size:
        raise AssetError("truncated LZSS block")
    return bytes(out)


class Fat16:
    def __init__(self, image):
        self.image = image
        start, count = u32(image, 3 * 16 + 8), u32(image, 3 * 16 + 12)
        self.volume = span(image, start * 512, count * 512)
        b = self.volume
        if span(b, 510, 2) != b"\x55\xaa" or span(b, 54, 8) != b"FAT16   ":
            raise AssetError("partition 3 is not a BE-300 FAT16 volume")
        sector, cluster = u16(b, 11), b[13]
        reserved, fats, roots, fat_size = u16(b, 14), b[16], u16(b, 17), u16(b, 22)
        total = u16(b, 19) or u32(b, 32)
        if (sector != 512 or not cluster or cluster & (cluster - 1) or
                fats not in (1, 2) or not reserved or not roots or not fat_size):
            raise AssetError("invalid FAT16 geometry")
        span(b, 0, total * sector)
        self.fat = span(b, reserved * sector, fat_size * sector)
        root_off = (reserved + fats * fat_size) * sector
        root_size = ((roots * 32 + sector - 1) // sector) * sector
        self.root = span(b, root_off, roots * 32)
        self.data = root_off + root_size
        self.cluster_size = sector * cluster
        self.clusters = (total * sector - self.data) // self.cluster_size
        if self.clusters < 1 or self.clusters + 2 > len(self.fat) // 2:
            raise AssetError("FAT16 allocation table does not cover volume")

    def chain(self, cluster):
        out = bytearray()
        seen = set()
        while cluster < 0xfff8:
            if not 2 <= cluster < self.clusters + 2 or cluster in seen:
                raise AssetError("invalid or cyclic FAT chain")
            seen.add(cluster)
            out += span(self.volume, self.data + (cluster - 2) * self.cluster_size,
                        self.cluster_size)
            cluster = u16(self.fat, cluster * 2)
        return bytes(out)

    def entries(self, directory):
        long_parts = []
        for off in range(0, len(directory), 32):
            e = span(directory, off, 32)
            if e[0] == 0:
                break
            if e[0] == 0xe5:
                long_parts = []
                continue
            if e[11] == 15:
                long_parts.append((e[0] & 31, e[1:11] + e[14:26] + e[28:32]))
                continue
            short = e[:8].decode("ascii", "replace").rstrip()
            ext = e[8:11].decode("ascii", "replace").rstrip()
            short += "." + ext if ext else ""
            name = short
            if long_parts:
                try:
                    name = b"".join(p[1] for p in sorted(long_parts)).decode("utf-16le")
                    name = name.split("\0")[0].rstrip("\uffff")
                except UnicodeDecodeError as exc:
                    raise AssetError("invalid FAT long name") from exc
            long_parts = []
            if not e[11] & 8 and short not in (".", ".."):
                yield name, short, e[11], u16(e, 26), u32(e, 28)

    def read(self, path):
        directory = self.root
        parts = path.replace("\\", "/").strip("/").split("/")
        for i, part in enumerate(parts):
            match = next((e for e in self.entries(directory)
                          if part.casefold() in (e[0].casefold(), e[1].casefold())), None)
            if match is None:
                raise AssetError(f"not found in local NAND: {path}")
            _, _, attr, cluster, size = match
            if i < len(parts) - 1:
                if not attr & 16:
                    raise AssetError("non-directory in FAT path")
                directory = self.chain(cluster)
            else:
                if attr & 16:
                    raise AssetError("expected a FAT file")
                return span(self.chain(cluster), 0, size) if size else b""


class NkImage:
    """Local ROM module metadata, used for CCFS tables and export names."""

    def __init__(self, nand):
        start, count = u32(nand, 40), u32(nand, 44)
        logical = lzss(span(nand, start * 512, count * 512))
        if span(logical, 0, 7) != b"B000FF\n":
            raise AssetError("NK partition does not contain a Casio B000FF stream")
        records = []
        off = 15
        while True:
            addr, length, checksum = struct.unpack("<III", span(logical, off, 12))
            off += 12
            if not addr:
                break
            payload = span(logical, off, length)
            if sum(payload) & 0xffffffff != checksum:
                raise AssetError("NK record checksum mismatch")
            records.append((addr, payload))
            off += length
        if not records:
            raise AssetError("NK contains no records")
        self.base = min(a for a, _ in records) & ~4095
        size = max(a + len(d) for a, d in records) - self.base
        if size > 16 * 1024 * 1024:
            raise AssetError("NK exceeds BE-300 memory")
        self.flat = bytearray(size)
        for addr, payload in records:
            self.flat[addr - self.base:addr - self.base + len(payload)] = payload

    def module(self, name):
        b = self.flat
        search = 0
        while True:
            name_off = b.lower().find(name.lower().encode() + b"\0", search)
            if name_off < 0:
                raise AssetError(f"ROM module missing: {name}")
            search = name_off + 1
            pointer = struct.pack("<I", self.base + name_off)
            pos = b.find(pointer)
            while pos >= 0:
                try:
                    e32 = u32(b, pos + 4) - self.base
                    o32 = u32(b, pos + 8) - self.base
                    sections, size = u16(b, e32), u32(b, e32 + 20)
                    if not 1 <= sections <= 32 or not 4096 <= size <= 4 * 1024 * 1024:
                        raise AssetError("not a ROM module entry")
                    result = bytearray(size)
                    for i in range(sections):
                        vs, rva, ps, ptr, _, flags = struct.unpack("<6I", span(b, o32 + i * 24, 24))
                        if rva + max(vs, ps) > size:
                            raise AssetError("ROM section exceeds image")
                        # The text section contains both CCFS's dictionary and
                        # COREDLL's export tables. Compressed writable data is
                        # deliberately not presented as executable module data.
                        if not flags & 0x2000:
                            result[rva:rva + ps] = span(b, ptr - self.base, ps)
                    return bytes(result)
                except AssetError:
                    pos = b.find(pointer, pos + 1)


class CpkDecoder:
    def __init__(self, ccfs):
        # Locate CCFS's canonical Huffman metadata, independent of link VA.
        signature = struct.pack("<7i", 0, 0, 0, 0, 0, -2, -7)
        off = ccfs.find(signature)
        if off < 4 or ccfs.find(signature, off + 1) >= 0:
            raise AssetError("unrecognized CCFS Huffman dictionary")
        self.driver, self.table = ccfs, off - 4

    def block(self, data, kind, size):
        if kind == 0:
            return span(data, 0, size)
        if 1 <= kind <= 8:
            nibble = kind >= 4
            threshold = kind - 2 if nibble else kind + 1
            seq = [v for x in data for v in (x & 15, x >> 4)] if nibble else data
            out, pos, run, previous = [], 0, 0, -1
            target = size * (2 if nibble else 1)
            while len(out) < target:
                if pos >= len(seq):
                    raise AssetError("truncated CPK run")
                value = seq[pos]
                pos += 1
                out.append(value)
                run = run + 1 if previous == value else 1
                previous = value
                if run == threshold:
                    if pos >= len(seq):
                        raise AssetError("truncated CPK run count")
                    out.extend([value] * seq[pos])
                    pos += 1
                    run = 0
            if len(out) != target:
                raise AssetError("CPK run exceeds block")
            return bytes(out[i] | out[i + 1] << 4 for i in range(0, target, 2)) if nibble else bytes(out)
        if 9 <= kind <= 14:
            nibble = kind >= 13
            bits = kind - (12 if nibble else 8)
            unit = 4 if nibble else 8
            mask = (1 << bits) - 1
            count = size * (2 if nibble else 1)
            index = mask * unit
            escape = (1 << bits) * unit + ((count * bits + unit - 1) // unit) * unit - unit

            def read(bit, width):
                n = (bit % 8 + width + 7) // 8
                return int.from_bytes(span(data, bit // 8, n), "little") >> (bit % 8) & ((1 << width) - 1)

            out = []
            for _ in range(count):
                value = read(index, bits)
                index += bits
                if value == mask:
                    out.append(read(escape, unit))
                    escape += unit
                else:
                    out.append(read(value * unit, unit))
            return bytes(out[i] | out[i + 1] << 4 for i in range(0, count, 2)) if nibble else bytes(out)
        if kind == 15:
            out, pos = bytearray(), 0
            driver, table = self.driver, self.table

            def read(bits):
                nonlocal pos
                value = 0
                for _ in range(bits):
                    value = value * 2 + ((span(data, pos // 8, 1)[0] >> (7 - pos % 8)) & 1)
                    pos += 1
                return value

            if size & 1:
                raise AssetError("odd CPK Huffman output size")
            while len(out) < size:
                code = 0
                for width in range(1, 17):
                    code = code * 2 + read(1)
                    if code < u32(driver, table + 0x84 + 4 * width):
                        break
                else:
                    raise AssetError("invalid CPK Huffman code")
                base = struct.unpack("<i", span(driver, table + 4 + width * 4, 4))[0]
                value = u16(driver, table + 0x104 + 2 * (base + code))
                if value == u32(driver, table):
                    value = read(16)
                out += struct.pack("<H", value)
            return bytes(out)
        if kind == 16:
            return lzss(data, size)
        raise AssetError(f"unsupported CPK block type {kind}")

    def unpack(self, data):
        if span(data, 0, 12) != b"CASIO.$&%-Z\x01":
            raise AssetError("unsupported CPK signature/version")
        size, word = u32(data, 44), u32(data, 48)
        count, stride = word & 0xffffff, ((word >> 24) or 1) * 512
        if not 0 < size <= 4 * 1024 * 1024 or count != (size + stride - 1) // stride:
            raise AssetError("invalid CPK output size/block count")
        payload = 52 + count * 4
        span(data, 0, payload)
        out = bytearray()
        for i in range(count):
            word = u32(data, 52 + 4 * i)
            lo = word & 0xffffff
            hi = u32(data, 56 + 4 * i) & 0xffffff if i + 1 < count else len(data) - payload
            if hi < lo or (i == 0 and lo != 0):
                raise AssetError("invalid CPK block offsets")
            out += self.block(span(data, payload + lo, hi - lo), word >> 24, stride)
        return bytes(out[:size])


def exports(image, module_name):
    """Find a ROM export directory by its validated module-name pointer."""
    name = image.lower().find(module_name.lower().encode() + b"\0")
    if name < 0:
        raise AssetError(f"no export name for {module_name}")
    for off in range(0, len(image) - 40, 4):
        if u32(image, off + 12) != name:
            continue
        base, functions, names, ft, nt, ot = struct.unpack("<6I", span(image, off + 16, 24))
        if not 0 < names <= functions <= 65536:
            continue
        result = {}
        for i in range(names):
            ordinal = u16(image, ot + i * 2)
            if ordinal >= functions:
                raise AssetError("ROM export ordinal out of range")
            span(image, ft + ordinal * 4, 4)
            result[base + ordinal] = cstr(image, u32(image, nt + i * 4))
        return result
    raise AssetError(f"no valid export directory for {module_name}")
