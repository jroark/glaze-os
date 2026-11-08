#!/usr/bin/env python3
"""Audit which original BE-300 applications the WinCE runtime can bind.

Usage: tools/wince_audit.py --image /path/to/300.bin [--app Tasks.exe] [--json out.json]

For each executable in the image's Program Files, follows its imports as
apps/wince/main.c resolves them: a runtime adapter first, then a library in
Program Files, then a Casio application DLL from NK (apps/wince/assets.c's
list). Libraries are followed transitively. A name nothing provides is
reported: calling it stops the app with "Unsupported WinCE call". The image
is only read; nothing vendor-derived is written unless --json is given.
"""

import argparse
import json
from pathlib import Path
import re
import struct
import sys

from wince_assets import AssetError, CpkDecoder, Fat16, NkImage, cstr, exports, u16, u32

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ROOT / "apps/wince"
# apps/wince/*.c resolver groups, in main.c's native_symbol order.
CORE = ("core", "crt", "mapping", "database", "string", "thread", "time", "window", "gdi", "fpe", "data", "process")
FILES = {"core": ["core.c"], "crt": ["crt.c"], "mapping": ["mapping.c"], "database": ["database.c"],
         "string": ["string.c", "scan.c"], "thread": ["thread.c"], "time": ["time.c", "timezone.c"],
         "window": ["window.c"], "gdi": ["gdi.c"], "fpe": ["fpe.c"], "data": [], "process": ["process.c"], "property": ["property.c"], "shell": ["shell.c"],
         "ole": ["ole.c"], "winsock": ["winsock.c"]}


def table_names(text):
    return set(re.findall(r"CE_EXPORT\(\s*(\w+)\s*\)", text)) | \
        set(re.findall(r'\{\s*"([^"]+)"\s*,\s*\(void \*\)', text))


class Runtime:
    """The names apps/wince's native_symbol() resolves, per importing DLL."""

    def __init__(self, sources=SOURCES):
        def names(f):
            text = (sources / f).read_text()
            # Resolvers may also compare single names; shell.c's are per DLL.
            extra = set() if f == "shell.c" else set(re.findall(r'strcmp\(name, "(\w+)"\)', text))
            return table_names(text) | extra
        self.groups = {g: set().union(*(names(f) for f in files)) for g, files in FILES.items()}
        shell = (sources / "shell.c").read_text()
        body = shell[shell.index("void *ce_resolve_shell("):]
        self.shell_dlls = {}
        for m in re.finditer(r'if \(!strcasecmp\(dll, "([\w.]+)"\)\) \{(.*?)\n    \}', body, re.S):
            self.shell_dlls[m.group(1).lower()] = set(re.findall(r'strcmp\(name, "(\w+)"\)', m.group(2)))
        tail = body[body.index("if (strcasecmp(dll, \"coredll.dll\")"):]
        self.shell_general = {d.lower() for d in re.findall(r'strcasecmp\(dll, "([\w.]+)"\)', tail.split(")\n")[0] +
                                                             tail.split("return NULL;")[0])}
        assets = (sources / "assets.c").read_text()
        block = assets[assets.index("rom_libraries[] = {"):]
        self.rom_libraries = [n for n in re.findall(r'"([\w.]+)"', block[:block.index("};")])]

    def native(self, dll, name):
        dll = dll.lower()
        if dll == "coredll.dll" and any(name in self.groups[g] for g in CORE):
            return True
        if dll in ("commctrl.dll", "cso.dll") and (name in self.groups["window"] or name in self.groups["gdi"]):
            return True
        if name in self.groups["property"]:
            return True
        if dll == "coshellapi.dll" and name in self.groups["process"]:
            return True
        if dll in self.shell_dlls:
            return name in self.shell_dlls[dll]
        if dll in self.shell_general and name in self.groups["shell"]:
            return True
        if dll == "oleaut32.dll" and name in self.groups["ole"]:
            return True
        return dll in ("winsock.dll", "ws2.dll") and name in self.groups["winsock"]


def pe_layout(data):
    """(image laid out at RVAs, export dir (rva, size), import dir (rva, size))."""
    off = u32(data, 60)
    count, optional = u16(data, off + 6), u16(data, off + 20)
    o = off + 24
    size, headers = u32(data, o + 56), u32(data, o + 60)
    image = bytearray(size)
    image[:headers] = data[:headers]
    for i in range(count):
        s = o + optional + i * 40
        vs, rva, raw_size, raw = struct.unpack_from("<IIII", data, s + 8)
        image[rva:rva + min(raw_size, vs or raw_size)] = data[raw:raw + min(raw_size, vs or raw_size)]
    return bytes(image), struct.unpack_from("<II", data, o + 96), struct.unpack_from("<II", data, o + 104)


def export_table(image, directory):
    rva, size = directory
    if not size:
        return set(), set()
    base, functions, names, ft, nt, ot = struct.unpack_from("<6I", image, rva + 16)
    named = {cstr(image, u32(image, nt + i * 4)) for i in range(names)}
    return named, {base + i for i in range(functions) if u32(image, ft + i * 4)}


def import_table(image, directory, ordinals):
    rva, size = directory
    out = []
    for at in range(rva, rva + size - 19, 20):
        lookup, _, _, name, first = struct.unpack_from("<5I", image, at)
        if not (lookup | name | first):
            break
        dll = cstr(image, name)
        table = ordinals.get(dll.lower(), {})
        for j in range(4096):
            thunk = u32(image, (lookup or first) + j * 4)
            if not thunk:
                break
            if thunk & 0x80000000:
                ordinal = thunk & 0xffff
                out.append((dll, table.get(ordinal), ordinal))
            else:
                out.append((dll, cstr(image, thunk + 2), None))
    return out


class Audit:
    def __init__(self, nand, runtime):
        self.runtime = runtime
        self.volume = Fat16(nand)
        self.nk = NkImage(nand, strict=False)
        self.decoder = CpkDecoder(self.nk.module("ccfs.dll"))
        self.ordinals = {d: exports(self.nk.module(d), d) for d in ("coredll.dll", "commctrl.dll")}
        root = next(e for e in self.volume.entries(self.volume.root) if e[0] == "Program Files")
        self.programs = {e[0]: e for e in self.volume.entries(self.volume.chain(root[3])) if not e[2] & 16}
        self.toc = self.nk.toc()
        self.modules = {}

    def program(self, name):
        for candidate in (name, name + ".cpk"):
            match = next((n for n in self.programs if n.lower() == candidate.lower()), None)
            if match:
                data = self.volume.read("Program Files/" + match)
                return self.decoder.unpack(data) if match.lower().endswith(".cpk") else data
        return None

    def module(self, dll):
        """('fat'|'nk', named exports, ordinal exports, imports) or None."""
        key = dll.lower()
        if key in self.modules:
            return self.modules[key]
        result = None
        data = self.program(dll)
        if data is not None:
            image, exp, imp = pe_layout(data)
            result = ("fat",) + export_table(image, exp) + (import_table(image, imp, self.ordinals),)
        elif key in {n.lower() for n in self.runtime.rom_libraries} and key in self.toc:
            m = self.nk.rom_module(dll)
            result = ("nk",) + export_table(m["image"], m["units"][0]) + \
                (import_table(m["image"], m["units"][1], self.ordinals),)
        self.modules[key] = result
        return result

    def app(self, name):
        data = self.program(name)
        image, _, imp = pe_layout(data)
        missing, loaded, seen = {}, {}, set()
        pending = [(name, import_table(image, imp, self.ordinals))]
        while pending:
            owner, imports = pending.pop()
            for dll, symbol, ordinal in imports:
                if symbol and self.runtime.native(dll, symbol):
                    continue
                module = self.module(dll)
                label = symbol or f"#{ordinal}"
                if module and (symbol in module[1] if symbol else ordinal in module[2]):
                    loaded[dll.lower()] = module[0]
                    if dll.lower() not in seen:
                        seen.add(dll.lower())
                        pending.append((dll, module[3]))
                    continue
                missing.setdefault(dll, {}).setdefault(label, set()).add(owner)
        return {"loaded": loaded,
                "missing": {d: {s: sorted(o) for s, o in sorted(v.items())} for d, v in sorted(missing.items())}}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--app", action="append", help="executable to audit (repeatable; default: all)")
    parser.add_argument("--json", type=Path, help="write the full report (local use only)")
    parser.add_argument("--verbose", "-v", action="store_true", help="list each missing name")
    args = parser.parse_args()
    audit = Audit(args.image.read_bytes(), Runtime())
    names = args.app or sorted((n[:-4] if n.lower().endswith(".cpk") else n for n in audit.programs
                                if n.lower().endswith((".exe", ".exe.cpk"))), key=str.lower)
    report, total = {}, 0
    for name in names:
        result = report[name] = audit.app(name)
        count = sum(len(v) for v in result["missing"].values())
        total += count
        rom = sorted(d for d, kind in result["loaded"].items() if kind == "nk")
        print(f"{name:28} {count:4} missing" + (f"  (NK: {', '.join(rom)})" if rom else ""))
        if args.verbose:
            for dll, symbols in result["missing"].items():
                for symbol, owners in symbols.items():
                    print(f"    {dll}!{symbol}  <- {', '.join(owners)}")
    print(f"{total} missing imports in {len(names)} applications")
    if args.json:
        args.json.write_text(json.dumps(report, indent=1) + "\n")


if __name__ == "__main__":
    try:
        main()
    except (OSError, AssetError, KeyError) as exc:
        print(f"wince-audit: {exc}", file=sys.stderr)
        sys.exit(1)
