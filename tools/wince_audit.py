#!/usr/bin/env python3
"""Audit which original BE-300 applications the WinCE runtime can bind.

Usage: tools/wince_audit.py --image /path/to/300.bin [--app Tasks.exe] [--json out.json]

For each executable on the image's FAT volume (or --app: a name in Program
Files, or a FAT path such as "Program Files/Extra Apps/Calc.exe"), follows its
imports as apps/wince resolves them: a runtime adapter first, then a library
in the program's folder or \Windows, a Casio application DLL from NK
(apps/wince/assets.c's list), the root, the device registry's
HKLM\Loader\SystemPath folders, and Program Files. Libraries are followed
transitively. A name nothing provides is
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
CORE = ("core", "crt", "mapping", "database", "string", "thread", "time", "window", "gdi", "fpe", "system", "ras", "data", "process")
FILES = {"core": ["core.c"], "crt": ["crt.c"], "mapping": ["mapping.c"], "database": ["database.c"],
         "string": ["string.c", "scan.c"], "thread": ["thread.c"], "time": ["time.c", "timezone.c"],
         "window": ["window.c"], "gdi": ["gdi.c"], "fpe": ["fpe.c"], "system": ["system.c"], "ras": ["ras.c"], "data": [], "process": ["process.c"], "property": ["property.c"], "shell": ["shell.c"],
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
        if dll == "getdisk.dll" and name in ("CGDSetUserNotification", "CGDClearUserNotification",
                                             "CGDGetUserNotification"):
            return True
        if dll == "coshellapi.dll" and name in self.groups["process"]:
            return True
        if dll in self.shell_dlls:
            return name in self.shell_dlls[dll]
        if dll in self.shell_general and name in self.groups["shell"]:
            return True
        if dll in ("oleaut32.dll", "ole32.dll") and name in self.groups["ole"]:
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


def system_path(volume):
    """HKLM\Loader\SystemPath from the device's Casio registry backup
    (System.reg, or System.$$$ on images taken mid-backup), as FAT folders."""
    data = None
    for name in ("Backup/System.reg", "Backup/System.$$$"):
        try:
            data = volume.read(name)
            break
        except AssetError:
            continue
    if not data or len(data) < 8 or u32(data, 0) != 0x1d8374b2:
        return []
    pos, key, out = 8, "", []
    while pos + 4 <= len(data):
        length, kind = u16(data, pos), u16(data, pos + 2)
        rec = data[pos + 4:pos + 4 + length]
        if kind == 1 and len(rec) >= 6:
            units = u16(rec, 2)
            key = ("HKLM\\" if u16(rec, 0) == 2 else "?") + rec[6:6 + units * 2].decode("utf-16le", "replace").rstrip("\0")
        elif kind == 2 and len(rec) >= 6 and key.lower() == "hklm\\loader":
            units, size = u16(rec, 2), u16(rec, 4)
            if rec[6:6 + units * 2].decode("utf-16le", "replace").rstrip("\0").lower() == "systempath":
                text = rec[6 + units * 2:6 + units * 2 + size].decode("utf-16le", "replace")
                out = [ce_folder(p) for p in text.split("\0") if p]
        pos += 4 + length
    return out


def ce_folder(path):
    """A CE folder as a FAT path, as apps/wince/core.c canonicalizes it."""
    parts = [p for p in path.replace("\\", "/").split("/") if p]
    if parts and parts[0].lower() in ("nand disk", "storage card"):
        parts = parts[1:]
    if parts and parts[0].lower() == "program disk":
        parts = ["Program Files"] + parts[1:]
    return "/".join(parts)


class Audit:
    def __init__(self, nand, runtime):
        self.runtime = runtime
        self.volume = Fat16(nand)
        self.nk = NkImage(nand, strict=False)
        self.decoder = CpkDecoder(self.nk.module("ccfs.dll"))
        self.ordinals = {d: exports(self.nk.module(d), d) for d in ("coredll.dll", "commctrl.dll")}
        self.files = {}  # lowercase FAT path -> its real path
        self.walk(self.volume.root, "")
        self.toc = self.nk.toc()
        self.modules = {}
        self.search_tail = ["", *system_path(self.volume), "Program Files"]
        self.programs = sorted((p[:-4] if p.lower().endswith(".cpk") else p for p in self.files.values()
                                if p.lower().endswith((".exe", ".exe.cpk"))), key=str.lower)

    def walk(self, directory, prefix):
        for name, _, attr, cluster, _ in self.volume.entries(directory):
            path = f"{prefix}/{name}" if prefix else name
            if attr & 16:
                if cluster:
                    self.walk(self.volume.chain(cluster), path)
            else:
                self.files[path.lower()] = path

    def read(self, path):
        """A FAT file's decoded bytes, also as name.cpk; None if absent."""
        for candidate in (path, path + ".cpk"):
            real = self.files.get(candidate.lower())
            if real:
                data = self.volume.read(real)
                return self.decoder.unpack(data) if real.lower().endswith(".cpk") else data
        return None

    def module(self, dll, folder):
        """('fat'|'nk', named exports, ordinal exports, imports) or None, in
        apps/wince/source.c's order: the program's folder and \Windows, NK's
        Casio libraries, then the root, SystemPath and Program Files."""
        key = dll.lower()
        rom = key in {n.lower() for n in self.runtime.rom_libraries} and key in self.toc
        for where in [folder, "Windows", None, *self.search_tail]:
            if where is None:
                if not rom:
                    continue
                cache = ("nk", key)
                if cache not in self.modules:
                    m = self.nk.rom_module(dll)
                    self.modules[cache] = ("nk",) + export_table(m["image"], m["units"][0]) + \
                        (import_table(m["image"], m["units"][1], self.ordinals),)
                return self.modules[cache]
            path = f"{where}/{dll}" if where else dll
            cache = ("fat", path.lower())
            if cache not in self.modules:
                data = self.read(path)
                if data is None:
                    self.modules[cache] = None
                else:
                    image, exp, imp = pe_layout(data)
                    self.modules[cache] = ("fat",) + export_table(image, exp) + \
                        (import_table(image, imp, self.ordinals),)
            if self.modules[cache]:
                return self.modules[cache]
        return None

    def app(self, name):
        path = name if "/" in name else "Program Files/" + name
        folder = path.rsplit("/", 1)[0] if "/" in path else ""
        image, _, imp = pe_layout(self.read(path))
        missing, loaded, seen = {}, {}, set()
        pending = [(path.rsplit("/", 1)[-1], import_table(image, imp, self.ordinals))]
        while pending:
            owner, imports = pending.pop()
            for dll, symbol, ordinal in imports:
                if symbol and self.runtime.native(dll, symbol):
                    continue
                module = self.module(dll, folder)
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
    names = args.app or audit.programs
    report, total = {}, 0
    for name in names:
        result = report[name] = audit.app(name)
        count = sum(len(v) for v in result["missing"].values())
        total += count
        rom = sorted(d for d, kind in result["loaded"].items() if kind == "nk")
        print(f"{name:52} {count:4} missing" + (f"  (NK: {', '.join(rom)})" if rom else ""))
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
