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


class CbeaArchive:
    """Casio BE-300 application installer archive (.cbea)."""

    def __init__(self, data):
        if len(data) < 256:
            raise AssetError("CBEA archive is smaller than 256-byte header")
        self.version = u16(data, 0)
        count = u32(data, 2)
        if not self.version or not 1 <= count <= 4096:
            raise AssetError(f"invalid CBEA header: version={self.version:#x}, count={count}")
        try:
            self.target = bytes(span(data, 8, 72)).decode("utf-16le").split("\0", 1)[0]
        except UnicodeDecodeError as exc:
            raise AssetError("invalid CBEA target string") from exc
        self.entries = []
        pos = 256
        for _ in range(count):
            hdr = span(data, pos, 8)
            next_off = u32(hdr, 0)
            kind = bytes(hdr[4:7])
            name_len = hdr[7]
            pos += 8
            if not name_len:
                raise AssetError("empty CBEA entry name")
            raw_name = bytes(span(data, pos, name_len + 1))
            if raw_name[-1] != 0 or b"\0" in raw_name[:-1]:
                raise AssetError("malformed CBEA entry name")
            name = raw_name[:-1].decode("latin1")
            if any(ord(c) < 0x20 or c in "/\\" for c in name) or name in (".", ".."):
                raise AssetError(f"unsafe CBEA entry name: {name}")
            pos += name_len + 1
            size = u32(data, pos)
            pos += 4
            payload = bytes(span(data, pos, size))
            if next_off != 4 + name_len + 1 + 4 + size:
                raise AssetError(f"CBEA entry offset mismatch for {name}")
            pos += size
            self.entries.append({"name": name, "kind": kind, "size": size, "data": payload})

    def find(self, name):
        key = name.casefold()
        for entry in self.entries:
            if entry["name"].casefold() == key:
                return entry
        return None


def _replace_icase(text, token, replacement):
    out = []
    pos = 0
    low = text.casefold()
    tlow = token.casefold()
    tlen = len(token)
    while pos < len(text):
        idx = low.find(tlow, pos)
        if idx < 0:
            out.append(text[pos:])
            break
        out.append(text[pos:idx])
        out.append(replacement)
        pos = idx + tlen
    return "".join(out)


def canonicalize_ce_path(raw_path, install_dir=None):
    """Map a WinCE / CBEA destination path to a canonical '/' path."""
    s = raw_path.strip()
    if install_dir:
        s = _replace_icase(s, "%INSTALL_DIR%", install_dir)
    s = _replace_icase(s, "?Drive?", "\\Nand Disk")
    s = s.replace("\\", "/")
    parts = []
    for part in s.split("/"):
        if not part:
            continue
        if part in (".", "..") or ":" in part or any(ord(c) < 0x20 for c in part):
            raise AssetError(f"unsafe WinCE path: {raw_path}")
        parts.append(part)
    if not parts:
        raise AssetError(f"empty WinCE path: {raw_path}")
    first = parts[0].casefold()
    if first in ("nand disk", "storage card"):
        parts = parts[1:]
    elif first == "program disk":
        parts = ["Program Files"] + parts[1:]
    if not parts:
        raise AssetError(f"empty WinCE target path: {raw_path}")
    if parts[0].casefold() == "my documents":
        parts = ["Documents"] + parts[1:]
    return "/" + "/".join(parts)


def _split_inf_fields(line):
    fields = []
    pos = 0
    n = len(line)
    while pos < n:
        while pos < n and line[pos] in " \t":
            pos += 1
        if pos >= n:
            fields.append("")
            break
        if line[pos] == '"':
            pos += 1
            start = pos
            while pos < n and line[pos] != '"':
                pos += 1
            val = line[start:pos]
            if pos < n and line[pos] == '"':
                pos += 1
            while pos < n and line[pos] != ",":
                pos += 1
        else:
            start = pos
            while pos < n and line[pos] != ",":
                pos += 1
            val = line[start:pos].rstrip(" \t")
        fields.append(val)
        if pos < n and line[pos] == ",":
            pos += 1
            if pos == n:
                fields.append("")
                break
    return fields


def _unquote(s):
    s = s.strip()
    if len(s) >= 2 and s[0] == '"' and s[-1] == '"':
        return s[1:-1]
    return s


def parse_cbea_inf(text):
    """Parse a CBEA Install.inf into structured app info, file copies, and registry entries."""
    app = {}
    make_dirs = []
    copy_raw = []
    reg_raw = []
    section = ""
    for raw_line in text.splitlines():
        line = raw_line.strip()
        if not line or line.startswith(";"):
            continue
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1].strip().casefold()
            continue
        if section == "app information":
            if "=" not in line:
                continue
            k, v = line.split("=", 1)
            app[k.strip().casefold()] = _unquote(v)
        elif section == "make dirs":
            make_dirs.append(_unquote(line))
        elif section == "copy files":
            fields = _split_inf_fields(line)
            if len(fields) < 2 or not fields[0] or not fields[1]:
                raise AssetError(f"invalid [Copy Files] line: {raw_line}")
            copy_raw.append((fields[0], fields[1], fields[2] if len(fields) > 2 else ""))
        elif section == "add registry":
            fields = _split_inf_fields(line)
            if len(fields) < 5:
                raise AssetError(f"invalid [Add Registry] line: {raw_line}")
            reg_raw.append(fields)

    install_dir_raw = app.get("installdir", "")
    install_dir = canonicalize_ce_path(install_dir_raw) if install_dir_raw else "/Program Files"
    exe_raw = app.get("exefile", "")
    exe_path = canonicalize_ce_path(exe_raw, install_dir_raw or install_dir) if exe_raw else ""

    dirs = [canonicalize_ce_path(d, install_dir_raw or install_dir) for d in make_dirs]
    copies = []
    for dst_raw, src_name, flags in copy_raw:
        canonical = canonicalize_ce_path(dst_raw, install_dir_raw or install_dir)
        copies.append({"dest": dst_raw, "canonical": canonical, "src": src_name, "flags": flags})

    root_map = {
        "hkey_local_machine": "HKLM", "hklm": "HKLM",
        "hkey_current_user": "HKCU", "hkcu": "HKCU",
        "hkey_classes_root": "HKCR", "hkcr": "HKCR",
        "hkey_users": "HKU", "hku": "HKU",
    }
    registry = []
    for fields in reg_raw:
        root = root_map.get(fields[0].strip().casefold())
        if not root:
            raise AssetError(f"unsupported registry root: {fields[0]}")
        subkey = fields[1].strip().strip("\\")
        if not subkey or "\\\\" in subkey:
            raise AssetError(f"invalid registry key: {fields[1]}")
        full_key = f"{root}\\{subkey}"
        val_name = fields[2]
        vtype = fields[3].strip().casefold()
        if vtype in ("reg_sz", "reg_expand_sz"):
            if len(fields) < 6:
                raise AssetError(f"missing string value for {full_key}")
            sval = fields[5]
            if sval.endswith("\\\\"):
                sval = sval[:-1]
            data = (sval + "\0").encode("utf-16le")
            kind = 1 if vtype == "reg_sz" else 2
        elif vtype == "reg_dword":
            if len(fields) < 6 or not fields[5].strip():
                raise AssetError(f"missing DWORD value for {full_key}")
            try:
                dword = int(fields[5].strip(), 16)
            except ValueError as exc:
                raise AssetError(f"invalid DWORD value for {full_key}: {fields[5]}") from exc
            if not 0 <= dword <= 0xffffffff:
                raise AssetError(f"DWORD out of range for {full_key}")
            data = struct.pack("<I", dword)
            kind = 4
        elif vtype == "reg_binary":
            try:
                raw_bytes = bytes(int(b.strip(), 16) for b in fields[5:] if b.strip())
            except ValueError as exc:
                raise AssetError(f"invalid REG_BINARY data for {full_key}") from exc
            data = raw_bytes
            kind = 3
        elif vtype == "reg_multi_sz":
            parts = [p.encode("utf-16le") + b"\0\0" for p in fields[5:]]
            data = b"".join(parts) + b"\0\0"
            kind = 7
        else:
            raise AssetError(f"unsupported registry value type: {fields[3]}")
        registry.append({"key": full_key, "name": val_name, "type": kind, "data": data})

    return {
        "program": app.get("program", ""),
        "caption": app.get("caption", ""),
        "maker": app.get("maker", ""),
        "version": app.get("version", ""),
        "destination": app.get("destination", ""),
        "unload": app.get("unload", ""),
        "uninstall": app.get("uninstall", ""),
        "install_dir": install_dir,
        "exe_file": exe_path,
        "make_dirs": dirs,
        "copy_files": copies,
        "registry": registry,
    }


def encode_gzce_registry(items, extra_keys=()):
    """Encode registry items and keys into a Glaze GZCE/GZCK binary stream."""
    valid_roots = {"hkcr", "hkcu", "hklm", "hku"}
    keys = []
    seen_keys = set()

    def add_key(k):
        k = k.strip("\\")
        parts = k.split("\\")
        if not parts or parts[0].casefold() not in valid_roots or any(not p for p in parts):
            raise AssetError(f"invalid registry key path: {k}")
        if len(k.encode("utf-8")) >= 256:
            raise AssetError(f"registry key too long: {k}")
        for idx in range(2, len(parts) + 1):
            prefix = "\\".join(parts[:idx])
            folded = prefix.casefold()
            if folded not in seen_keys:
                seen_keys.add(folded)
                keys.append(prefix)

    values = []
    value_index = {}
    for item in items:
        key = item["key"]
        name = item.get("name", "")
        kind = int(item["type"])
        data = bytes(item["data"])
        if "\\".join(key.split("\\")[:1]).casefold() not in valid_roots:
            raise AssetError(f"invalid registry key root: {key}")
        if "\\" in key:
            add_key(key)
        if len(name.encode("utf-8")) >= 128 or len(data) > 65536:
            raise AssetError(f"registry value exceeds limits: {key}\\{name}")
        vkey = (key.casefold(), name.casefold())
        if vkey in value_index:
            values[value_index[vkey]] = (key, name, kind, data)
        else:
            value_index[vkey] = len(values)
            values.append((key, name, kind, data))

    for k in extra_keys:
        if "\\" in k:
            add_key(k)

    if len(keys) > 128 or len(values) > 128:
        raise AssetError("registry exceeds Glaze capacity (128 keys / 128 values)")

    out = bytearray(b"GZCE" + struct.pack("<I", len(values)))
    for key, name, kind, data in values:
        kb = key.encode("utf-8") + b"\0"
        nb = name.encode("utf-8") + b"\0"
        out += struct.pack("<IIII", len(kb), len(nb), kind, len(data))
        out += kb + nb + data
    out += b"GZCK" + struct.pack("<I", len(keys))
    for key in keys:
        kb = key.encode("utf-8") + b"\0"
        out += struct.pack("<I", len(kb)) + kb
    if len(out) > 256 * 1024:
        raise AssetError("encoded registry exceeds 256 KiB")
    return bytes(out)
