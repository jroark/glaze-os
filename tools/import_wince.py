#!/usr/bin/env python3
"""Import selected locally supplied BE-300 assets into Glaze packages.

Usage: tools/import_wince.py --image /path/to/beshell.bin --output out/wince
Use --audit-only to extract and inventory without building install packages.
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import sys

from wince_assets import (AssetError, CpkDecoder, Fat16, NkImage, cstr, encode_gzce_registry, exports, span,
                          u16, u32)

APPLICATIONS = {
    "tweak": ("CE PalmTweak", "Tweak.exe", ()),
    "regedit": ("CE RegEdit", "RegEdit.exe", ()),
    "calculator": ("CE Calculator", "calc.exe", ("LC.dll",)),
    "notepad": ("CE Notepad", "notepad.exe", ()),
    "owner": ("CE Owner", "Owner.exe", ()),
    "ownerinfo": ("CE Owner Info", "OwnerInfo.exe", ()),
    "clock": ("CE Clock", "clock.exe", ()),
    "internet": ("CE Internet", "Internet.exe", ("IMGDECMP.DLL",)),
}
# Original data files packaged with an app: FAT files or folders, served at
# their WinCE paths through res/files.txt.
DATA = {
    "clock": ("Program Files/TIMEZONEINFO.txt",),
    "internet": ("My Documents/WebPage", "My Documents/Internet"),
}
# Registry values an app's private registry starts with (key, name, REG_SZ
# text). WorldTALK otherwise opens file:///windows/default.htm, which is not
# in this ROM; the image's saved registry holds this HomePage.
REGISTRY = {
    "internet": ((r"HKCU\Software\Tenik\WorldTALK\Common", "HomePage",
                  "file:///Nand Disk/My Documents/WebPage/Welcome to the Internet.html"),),
}
BAYER = ((0, 8, 2, 10), (12, 4, 14, 6), (3, 11, 1, 9), (15, 7, 13, 5))
RESERVED_RES = {"icon.gzi", "program.exe", "ordinals.txt", "source.txt", "registry", "files.txt"}


def sanitize_res_name(filename, used):
    """Map an arbitrary filename to a valid Glaze res/<name> entry (<= 43 chars, no spaces)."""
    base = filename.rsplit("/", 1)[-1].rsplit("\\", 1)[-1]
    safe = re.sub(r"[^A-Za-z0-9._-]+", "_", base).strip(".")
    while ".." in safe:
        safe = safe.replace("..", "_")
    if not safe:
        safe = "file"
    if "." in safe:
        stem, ext = safe.rsplit(".", 1)
        ext = "." + ext[:12]
    else:
        stem, ext = safe, ""
    max_stem = max(1, 43 - len(ext))
    candidate = stem[:max_stem] + ext
    idx = 2
    while candidate.casefold() in used or candidate.casefold() in RESERVED_RES:
        suffix = f"_{idx}"
        candidate = stem[: max(1, max_stem - len(suffix))] + suffix + ext
        idx += 1
    used.add(candidate.casefold())
    return candidate


def canonical_path(fat_path):
    """The runtime's canonical form of a FAT path: My Documents is /Documents."""
    parts = fat_path.strip("/").split("/")
    if parts[0].casefold() == "my documents":
        parts[0] = "Documents"
    return "/" + "/".join(parts)


def data_files(volume, decoder, path):
    """(FAT path, bytes) for a file or every file below a folder. Casio
    .cpk files are decompressed and lose the suffix, as the CCFS driver does."""
    parts = path.strip("/").split("/")
    directory, attr, cluster, size = volume.root, 16, 0, 0
    for i, part in enumerate(parts):
        match = next((e for e in volume.entries(directory)
                      if part.casefold() in (e[0].casefold(), e[1].casefold())
                      or part.casefold() + ".cpk" == e[0].casefold()), None)
        if match is None:
            raise AssetError(f"not found in local NAND: {path}")
        name, _, attr, cluster, size = match
        parts[i] = name
        if attr & 16:
            directory = volume.chain(cluster)
        elif i < len(parts) - 1:
            raise AssetError("non-directory in FAT path")
    if not attr & 16:
        data = span(volume.chain(cluster), 0, size) if size else b""
        name = "/".join(parts)
        if name.casefold().endswith(".cpk"):
            return [(name[:-4], decoder.unpack(data))]
        return [(name, data)]
    found = []
    for entry, _, child_attr, _, _ in volume.entries(directory):
        found += data_files(volume, decoder, "/".join(parts) + "/" + entry)
    return found


def validate_runner(data):
    """Installed apps must use Glaze's interpreter, including the PE runner."""
    if (span(data, 0, 7) != b"\x7fELF\x01\x01\x01" or u16(data, 16) != 3 or
            u16(data, 18) != 8 or u16(data, 42) != 32 or not 0 < u16(data, 44) <= 32):
        raise AssetError("runner must be a dynamic Glaze MIPS ELF; build with DYNAMIC=1")
    interpreter = None
    for i in range(u16(data, 44)):
        ph = span(data, u32(data, 28) + 32 * i, 32)
        if u32(ph, 0) == 3:
            if interpreter is not None:
                raise AssetError("runner contains multiple ELF interpreters")
            interpreter = span(data, u32(ph, 4), u32(ph, 16))
    if interpreter != b"/lib/ld-glaze.so.1\0":
        raise AssetError("runner must use the Glaze dynamic linker; build with DYNAMIC=1")


def pe_sections(data):
    if span(data, 0, 2) != b"MZ":
        raise AssetError("extracted executable has no DOS signature")
    off = u32(data, 60)
    if span(data, off, 4) != b"PE\0\0":
        raise AssetError("extracted executable has no PE signature")
    machine, count, optional = u16(data, off + 4), u16(data, off + 6), u16(data, off + 20)
    if machine not in (0x166, 0x169) or not 1 <= count <= 32:
        raise AssetError("executable is not BE-300 MIPS")
    header = span(data, off + 24, optional)
    if u16(header, 0) != 0x10b or u16(header, 68) != 9:
        raise AssetError("requires PE32 Windows CE GUI subsystem")
    sections = []
    for i in range(count):
        s = span(data, off + 24 + optional + i * 40, 40)
        sections.append({"name": s[:8].rstrip(b"\0").decode("ascii"), "rva": u32(s, 12),
                         "size": u32(s, 8), "offset": u32(s, 20), "bytes": u32(s, 16),
                         "flags": u32(s, 36)})
        span(data, sections[-1]["offset"], sections[-1]["bytes"])

    def rva(address, size):
        for section in sections:
            relative = address - section["rva"]
            if 0 <= relative <= section["bytes"] and size <= section["bytes"] - relative:
                return section["offset"] + relative
        raise AssetError(f"PE RVA outside raw section: {address:#x}")

    return machine, header, sections, rva


def pe_report(data, ordinals, dll_ordinals=None, bundled=()):
    """Inventory a PE. Ordinal imports from a packaged library need no name:
    the runtime resolves them from that library's own export table."""
    machine, header, sections, rva = pe_sections(data)
    imports = []
    start, length = u32(header, 104), u32(header, 108)
    terminated = not length
    for i in range(0, length - 19, 20):
        d = rva(start + i, 20)
        lookup, name, first = u32(data, d), u32(data, d + 12), u32(data, d + 16)
        if not (lookup | name | first):
            terminated = True
            break
        dll = cstr(data, rva(name, 1))
        symbols = []
        for j in range(4096):
            thunk = u32(data, rva((lookup or first) + j * 4, 4))
            if not thunk:
                break
            if thunk & 0x80000000:
                ordinal = thunk & 65535
                names = ordinals if dll.lower() == "coredll.dll" else (dll_ordinals or {}).get(dll.lower(), {})
                resolved = names.get(ordinal)
                if resolved is None and dll.lower() not in bundled:
                    raise AssetError(f"cannot identify {dll} ordinal {ordinal} in matching image")
                symbols.append({"ordinal": ordinal, "name": resolved})
            else:
                symbols.append({"name": cstr(data, rva(thunk + 2, 1))})
        else:
            raise AssetError("unterminated PE import table")
        imports.append({"dll": dll, "symbols": symbols})
    if not terminated:
        raise AssetError("unterminated PE import descriptors")
    return {"sha256": hashlib.sha256(data).hexdigest(), "bytes": len(data), "machine": machine,
            "base": u32(header, 28), "entry": u32(header, 16), "image_size": u32(header, 56),
            "sections": sections, "imports": imports}


def decode_dib_icon(blob):
    header_size = u32(blob, 0)
    w, h2, planes, bpp, comp = struct.unpack_from("<iiHHI", span(blob, 4, 16), 0)
    clr_used = u32(blob, 32)
    h = abs(h2) // 2
    if (header_size < 40 or planes != 1 or bpp not in (1, 4, 8, 16, 24, 32) or
            comp not in (0, 3) or not (1 <= w <= 256 and 1 <= h <= 256)):
        raise AssetError("unsupported PE icon format")
    colors = (clr_used or (1 << bpp)) if bpp <= 8 else 0
    if colors > 256:
        raise AssetError("invalid PE icon palette")
    pal = span(blob, header_size, colors * 4)
    xor_off = header_size + colors * 4 + (12 if comp == 3 and header_size == 40 else 0)
    xor_stride = ((w * bpp + 31) // 32) * 4
    and_off = xor_off + xor_stride * h
    and_stride = ((w + 31) // 32) * 4
    xor_bytes = span(blob, xor_off, xor_stride * h)
    and_bytes = span(blob, and_off, and_stride * h)
    has_alpha = bpp == 32 and any(xor_bytes[r * xor_stride + x * 4 + 3] for r in range(h) for x in range(w))
    rgba = bytearray(w * h * 4)
    for y in range(h):
        srow = (h - 1 - y) if h2 > 0 else y
        xrow = xor_bytes[srow * xor_stride:(srow + 1) * xor_stride]
        arow = and_bytes[srow * and_stride:(srow + 1) * and_stride]
        for x in range(w):
            abit = (arow[x // 8] >> (7 - (x % 8))) & 1
            if bpp <= 8:
                idx = (xrow[x] if bpp == 8 else
                       (xrow[x // 2] >> (4 if x % 2 == 0 else 0)) & 15 if bpp == 4 else
                       (xrow[x // 8] >> (7 - (x % 8))) & 1)
                if idx >= colors:
                    raise AssetError("PE icon palette index out of range")
                b, g, r = pal[idx * 4:idx * 4 + 3]
                a = 0 if abit else 255
            elif bpp == 16:
                v = u16(xrow, x * 2)
                if comp == 3:
                    r, g, b = (v >> 11) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31
                else:
                    r, g, b = ((v >> 10) & 31) * 255 // 31, ((v >> 5) & 31) * 255 // 31, (v & 31) * 255 // 31
                a = 0 if abit else 255
            elif bpp == 24:
                b, g, r = xrow[x * 3:x * 3 + 3]
                a = 0 if abit else 255
            else:
                b, g, r, a8 = xrow[x * 4:x * 4 + 4]
                a = (0 if abit else a8) if has_alpha else (0 if abit else 255)
            rgba[(y * w + x) * 4:(y * w + x + 1) * 4] = bytes((r, g, b, a))
    return w, h, bpp, rgba


def corner_alpha(x, y, size=44, radius=8):
    cx = radius if x < radius else (size - radius if x >= size - radius else None)
    cy = radius if y < radius else (size - radius if y >= size - radius else None)
    if cx is None or cy is None:
        return 255
    inside = 0
    r2 = radius * radius * 64
    for sy in range(8):
        dy = (y * 8 + sy + 0.5) - cy * 8
        for sx in range(8):
            dx = (x * 8 + sx + 0.5) - cx * 8
            if dx * dx + dy * dy <= r2:
                inside += 1
    return (inside * 255 + 32) // 64


def pe_icon(data):
    _, header, _, rva = pe_sections(data)
    rsrc_rva, rsrc_size = u32(header, 112), u32(header, 116)
    if rsrc_size < 16:
        raise AssetError("PE executable has no resource directory")
    resources = {3: [], 14: []}
    d0 = span(data, rva(rsrc_rva, 16), 16)
    n0 = u16(d0, 12) + u16(d0, 14)
    if 16 + n0 * 8 > rsrc_size:
        raise AssetError("invalid PE resource root directory")
    for i in range(n0):
        e0 = span(data, rva(rsrc_rva + 16 + i * 8, 8), 8)
        kind, sub0 = u32(e0, 0), u32(e0, 4)
        if kind not in resources or not (sub0 & 0x80000000):
            continue
        off1 = sub0 & 0x7fffffff
        if off1 + 16 > rsrc_size:
            raise AssetError("invalid PE resource type directory")
        d1 = span(data, rva(rsrc_rva + off1, 16), 16)
        n1 = u16(d1, 12) + u16(d1, 14)
        if off1 + 16 + n1 * 8 > rsrc_size:
            raise AssetError("invalid PE resource ID directory")
        for j in range(n1):
            e1 = span(data, rva(rsrc_rva + off1 + 16 + j * 8, 8), 8)
            res_id, sub1 = u32(e1, 0), u32(e1, 4)
            if not (sub1 & 0x80000000):
                raise AssetError("invalid PE resource entry")
            off2 = sub1 & 0x7fffffff
            if off2 + 24 > rsrc_size:
                raise AssetError("invalid PE resource language directory")
            d2 = span(data, rva(rsrc_rva + off2, 16), 16)
            n2 = u16(d2, 12) + u16(d2, 14)
            if n2 < 1 or off2 + 16 + n2 * 8 > rsrc_size:
                raise AssetError("empty PE resource language directory")
            e2 = span(data, rva(rsrc_rva + off2 + 16, 8), 8)
            data_off = u32(e2, 4)
            if (data_off & 0x80000000) or data_off + 16 > rsrc_size:
                raise AssetError("invalid PE resource data entry")
            de = span(data, rva(rsrc_rva + data_off, 16), 16)
            blob_rva, blob_size = u32(de, 0), u32(de, 4)
            resources[kind].append((res_id, span(data, rva(blob_rva, blob_size), blob_size)))
    icons = dict(resources[3])
    if resources[14]:
        gdata = resources[14][0][1]
        if len(gdata) < 6 or u16(gdata, 0) != 0 or u16(gdata, 2) != 1:
            raise AssetError("invalid PE icon group")
        gcount = u16(gdata, 4)
        span(gdata, 6, gcount * 14)
        blobs = [icons[u16(gdata, 6 + i * 14 + 12)] for i in range(gcount)
                 if u16(gdata, 6 + i * 14 + 12) in icons]
    else:
        blobs = [blob for _, blob in resources[3]]
    if not blobs:
        raise AssetError("PE executable has no icon resource")
    candidates = [decode_dib_icon(b) for b in blobs]
    w, h, _, rgba = max(
        candidates,
        key=lambda c: (c[0] <= 36 and c[1] <= 36,
                       min(c[0], c[1]) if c[0] <= 36 and c[1] <= 36 else -max(c[0], c[1]),
                       c[2]),
    )
    if w > 36 or h > 36:
        nw, nh = 32, 32
        scaled = bytearray(nw * nh * 4)
        for y in range(nh):
            sy = y * h // nh
            for x in range(nw):
                sx = x * w // nw
                scaled[(y * nw + x) * 4:(y * nw + x + 1) * 4] = rgba[(sy * w + sx) * 4:(sy * w + sx + 1) * 4]
        w, h, rgba = nw, nh, scaled
    ox, oy = (44 - w) // 2, (44 - h) // 2
    px, al = bytearray(), bytearray()
    for y in range(44):
        r = (0xee * (43 - y) + 0xad * y) // 43
        g = (0xf2 * (43 - y) + 0xb6 * y) // 43
        b = (0xf7 * (43 - y) + 0xc4 * y) // 43
        for x in range(44):
            a = corner_alpha(x, y)
            pr, pg, pb = r, g, b
            if ((x - 22) / 40.0) ** 2 + ((y + 11) / 33.0) ** 2 <= 1.0:
                ga = (120 * (22 - y) + 25 * y) // 22 if y <= 22 else 25
                pr = (255 * ga + pr * (255 - ga)) // 255
                pg = (255 * ga + pg * (255 - ga)) // 255
                pb = (255 * ga + pb * (255 - ga)) // 255
            if ox <= x < ox + w and oy <= y < oy + h:
                si = ((y - oy) * w + (x - ox)) * 4
                sr, sg, sb, sa = rgba[si:si + 4]
                if sa:
                    pr = (sr * sa + pr * (255 - sa)) // 255
                    pg = (sg * sa + pg * (255 - sa)) // 255
                    pb = (sb * sa + pb * (255 - sa)) // 255
            t = BAYER[y & 3][x & 3]
            r5 = min(31, (pr + (t >> 1)) >> 3)
            g6 = min(63, (pg + (t >> 2)) >> 2)
            b5 = min(31, (pb + (t >> 1)) >> 3)
            px += struct.pack("<H", (r5 << 11) | (g6 << 5) | b5)
            al.append(a)
    return b"GZI1" + struct.pack("<HHHH", 44, 44, 1, 0) + px + al


def package(entries):
    entries = sorted(entries.items())
    offset = 8 + 56 * len(entries)
    table = bytearray(b"GZPK" + struct.pack("<I", len(entries)))
    blobs = bytearray()
    for name, data in entries:
        if len(name.encode()) >= 48:
            raise AssetError("package path too long")
        table += struct.pack("<48sII", name.encode(), offset + len(blobs), len(data))
        blobs += data
        blobs += b"\0" * (-len(blobs) % 4)
    return bytes(table + blobs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=Path("out/wince"))
    parser.add_argument("--runner", type=Path, default=Path("out/romfs/bin/wince"))
    parser.add_argument("--audit-only", action="store_true")
    parser.add_argument("--app", choices=APPLICATIONS, action="append",
                        help="app to import (repeatable; default: calculator, notepad, owner, and ownerinfo)")
    args = parser.parse_args()
    nand = args.image.read_bytes()
    volume, nk = Fat16(nand), NkImage(nand)
    ccfs = nk.module("ccfs.dll")
    decoder = CpkDecoder(ccfs)
    ordinals = exports(nk.module("coredll.dll"), "coredll.dll")
    dll_ordinals = {"commctrl.dll": exports(nk.module("commctrl.dll"), "commctrl.dll")}
    payloads, report = {}, {"source_sha256": hashlib.sha256(nand).hexdigest(),
                            "ccfs_sha256": hashlib.sha256(ccfs).hexdigest(), "programs": {}}
    selected = list(dict.fromkeys(args.app or ("calculator", "notepad", "owner", "ownerinfo")))
    required = dict.fromkeys(name for app in selected
                             for name in (APPLICATIONS[app][1], *APPLICATIONS[app][2]))
    bundled = {name.lower() for app in selected for name in APPLICATIONS[app][2]}
    for name in required:
        payloads[name] = decoder.unpack(volume.read("Program Files/" + name + ".cpk"))
        report["programs"][name] = pe_report(payloads[name], ordinals, dll_ordinals, bundled)
    runner = None if args.audit_only else args.runner.read_bytes()
    if runner is not None:
        validate_runner(runner)
    ordinal_text = "".join(f"{key} {value}\n" for key, value in sorted(ordinals.items())).encode()
    ordinal_text += "".join(f"{dll} {key} {value}\n" for dll, names in sorted(dll_ordinals.items())
                             for key, value in sorted(names.items())).encode()
    outputs = {"audit.json": (json.dumps(report, indent=2) + "\n").encode()}
    for name, payload in payloads.items():
        outputs[name] = payload
    for stem in selected:
        title, executable, libraries = APPLICATIONS[stem]
        if runner is None:
            continue
        entries = {"Info": f"id=wince.{stem}\nname={title}\nversion=0.2\nexec=app\n".encode(),
                   "app": runner, "res/icon.gzi": pe_icon(payloads[executable]),
                   "res/program.exe": payloads[executable], "res/ordinals.txt": ordinal_text,
                   "res/source.txt": ("\\Program Files\\" + executable).encode()}
        for library in libraries:
            entries["res/" + library] = payloads[library]
        manifest, used = [], set(RESERVED_RES) | {name.casefold() for name in libraries}
        for path in DATA.get(stem, ()):
            for fat_path, data in data_files(volume, decoder, path):
                res_name = sanitize_res_name(fat_path, used)
                entries["res/" + res_name] = data
                manifest.append(f"{res_name} {canonical_path(fat_path)}\n")
        if manifest:
            if len(manifest) > 63:
                raise AssetError(f"{stem} data has more than 63 files")
            entries["res/files.txt"] = "".join(manifest).encode("utf-8")
        if stem in REGISTRY:
            entries["res/registry"] = encode_gzce_registry(
                [{"key": key, "name": name, "type": 1, "data": (text + "\0").encode("utf-16le")}
                 for key, name, text in REGISTRY[stem]])
        outputs[f"wince.{stem}.gzp"] = package(entries)
    args.output.mkdir(parents=True, exist_ok=True)
    for name, data in outputs.items():
        (args.output / name).write_bytes(data)
    for name, info in report["programs"].items():
        count = sum(len(d["symbols"]) for d in info["imports"])
        print(f"{name}: MIPS PE32, {info['bytes']} bytes, {count} imports, sha256={info['sha256']}")
    print(f"Wrote local-only assets to {args.output}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, AssetError, UnicodeError) as exc:
        print(f"import-wince: {exc}", file=sys.stderr)
        sys.exit(1)
