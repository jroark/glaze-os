#!/usr/bin/env python3
"""Import locally supplied Calculator/Notepad assets into Glaze packages.

Usage: tools/import_wince.py --image /path/to/beshell.bin --output out/wince
Use --audit-only to extract and inventory without building install packages.
"""

import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

from wince_assets import AssetError, CpkDecoder, Fat16, NkImage, cstr, exports, span, u16, u32


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


def pe_report(data, ordinals):
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
                resolved = ordinals.get(ordinal) if dll.lower() == "coredll.dll" else None
                if resolved is None:
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
    args = parser.parse_args()
    nand = args.image.read_bytes()
    volume, nk = Fat16(nand), NkImage(nand)
    ccfs = nk.module("ccfs.dll")
    decoder = CpkDecoder(ccfs)
    ordinals = exports(nk.module("coredll.dll"), "coredll.dll")
    payloads, report = {}, {"source_sha256": hashlib.sha256(nand).hexdigest(),
                            "ccfs_sha256": hashlib.sha256(ccfs).hexdigest(), "programs": {}}
    for name in ("calc.exe", "notepad.exe", "LC.dll"):
        payloads[name] = decoder.unpack(volume.read("Program Files/" + name + ".cpk"))
        report["programs"][name] = pe_report(payloads[name], ordinals)
    runner = None if args.audit_only else args.runner.read_bytes()
    if runner is not None:
        validate_runner(runner)
    ordinal_text = "".join(f"{key} {value}\n" for key, value in sorted(ordinals.items())).encode()
    root = Path(__file__).resolve().parent.parent
    outputs = {"audit.json": (json.dumps(report, indent=2) + "\n").encode()}
    for name, payload in payloads.items():
        outputs[name] = payload
    for stem, title, icon, executable in (("calculator", "CE Calculator", "calculator", "calc.exe"),
                                          ("notepad", "CE Notepad", "notes", "notepad.exe")):
        if runner is None:
            continue
        entries = {"Info": f"id=wince.{stem}\nname={title}\nversion=0.1\nexec=app\n".encode(),
                   "app": runner, "res/icon.gzi": (root / f"assets/gen/icon_{icon}.gzi").read_bytes(),
                   "res/program.exe": payloads[executable], "res/ordinals.txt": ordinal_text}
        if stem == "calculator":
            entries["res/LC.dll"] = payloads["LC.dll"]
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
