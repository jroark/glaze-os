#!/usr/bin/env python3
"""Convert Casio BE-300 .cbea installer archives into Glaze .gzp app packages.

Usage:
  tools/cbea2gzp.py /path/to/pkg.cbea [--image ../be300/nand/beshell.bin] [-o out/cbea]
  tools/cbea2gzp.py --list /path/to/pkg.cbea
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import sys

from import_wince import BAYER, corner_alpha, package, pe_icon, pe_sections, validate_runner
from wince_assets import (
    AssetError,
    CbeaArchive,
    CpkDecoder,
    NkImage,
    canonicalize_ce_path,
     cstr,
    encode_gzce_registry,
    exports,
    parse_cbea_inf,
    span,
    u16,
    u32,
)

RESERVED_RES = {"icon.gzi", "program.exe", "ordinals.txt", "source.txt", "registry", "files.txt"}
BUILTIN_CONFLICTS = {
    "calendar", "contacts", "mail", "notes", "tasks", "clock", "calculator",
    "settings", "photos", "music", "videos", "browser", "files", "appstore",
}


def default_icon():
    """Fallback 44x44 GZI icon when a PE executable has no icon resource."""
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
            if 10 <= x < 34 and 10 <= y < 34:
                if y < 16:
                    pr, pg, pb = 0x18, 0x4a, 0x9c
                elif x in (10, 33) or y == 33:
                    pr, pg, pb = 0x4a, 0x55, 0x68
                else:
                    pr, pg, pb = 0xff, 0xff, 0xff
            t = BAYER[y & 3][x & 3]
            r5 = min(31, (pr + (t >> 1)) >> 3)
            g6 = min(63, (pg + (t >> 2)) >> 2)
            b5 = min(31, (pb + (t >> 1)) >> 3)
            px += struct.pack("<H", (r5 << 11) | (g6 << 5) | b5)
            al.append(a)
    return b"GZI1" + struct.pack("<HHHH", 44, 44, 1, 0) + px + al


def is_pe_binary(data):
    """Return (is_pe, is_dll) for a byte buffer."""
    if len(data) < 64 or data[:2] != b"MZ":
        return False, False
    off = u32(data, 60)
    if off < 0 or off > len(data) - 24 or data[off:off + 4] != b"PE\0\0":
        return False, False
    characteristics = u16(data, off + 22)
    return True, bool(characteristics & 0x2000)


def pe_inspect(data, ordinals=None, dll_ordinals=None):
    """Inspect a PE32 MIPS module, tolerating unknown ROM ordinals with warnings."""
    machine, header, sections, rva = pe_sections(data)
    imports = []
    unresolved_ordinals = []
    needs_ordinals = False
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
                needs_ordinals = True
                ordinal = thunk & 65535
                names = (
                    (ordinals or {})
                    if dll.lower() == "coredll.dll"
                    else (dll_ordinals or {}).get(dll.lower(), {})
                )
                resolved = names.get(ordinal)
                if resolved is None:
                    unresolved_ordinals.append(f"{dll}!#{ordinal}")
                    resolved = f"#{ordinal}"
                symbols.append({"ordinal": ordinal, "name": resolved})
            else:
                symbols.append({"name": cstr(data, rva(thunk + 2, 1))})
        else:
            raise AssetError("unterminated PE import table")
        imports.append({"dll": dll, "symbols": symbols})
    if not terminated:
        raise AssetError("unterminated PE import descriptors")
    return {
        "sha256": hashlib.sha256(data).hexdigest(),
        "bytes": len(data),
        "machine": machine,
        "base": u32(header, 28),
        "entry": u32(header, 16),
        "image_size": u32(header, 56),
        "sections": sections,
        "imports": imports,
        "needs_ordinals": needs_ordinals,
        "unresolved_ordinals": unresolved_ordinals,
    }


def sanitize_pkg_id(raw, prefix="cbea."):
    slug = re.sub(r"[^a-z0-9.]+", "-", raw.lower()).strip(".-")
    while ".." in slug:
        slug = slug.replace("..", ".")
    if not slug:
        slug = "app"
    full = f"{prefix}{slug}" if prefix else slug
    full = full[:24].rstrip(".-")
    if not full or full[0] in ".-":
        full = "cbea.app"
    return full


def sanitize_display_name(raw, multi_exe=False):
    cleaned = "".join(c if 0x20 <= ord(c) <= 0x7e else " " for c in raw.strip())
    cleaned = " ".join(cleaned.split())
    if not cleaned:
        cleaned = "WinCE App"
    if multi_exe or cleaned.casefold() in BUILTIN_CONFLICTS:
        if not cleaned.upper().startswith("CE "):
            cleaned = f"CE {cleaned}"
    return cleaned[:31].rstrip()


def sanitize_version(raw):
    raw = (raw or "").strip()
    parts = re.findall(r"\d+", raw)
    if not parts:
        return "1.0"
    ver = ".".join(str(int(p[:6])) for p in parts[:4])
    return ver[:15] or "1.0"


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


def load_rom_Support(image_path):
    """Load CPK decoder and ROM export ordinal maps from a BE-300 NAND image."""
    nand = image_path.read_bytes()
    nk = NkImage(nand)
    ccfs = nk.module("ccfs.dll")
    decoder = CpkDecoder(ccfs)
    ordinals = exports(nk.module("coredll.dll"), "coredll.dll")
    dll_ordinals = {"commctrl.dll": exports(nk.module("commctrl.dll"), "commctrl.dll")}
    ordinal_text = "".join(f"{k} {v}\n" for k, v in sorted(ordinals.items())).encode("ascii")
    ordinal_text += "".join(
        f"{dll} {k} {v}\n"
        for dll, names in sorted(dll_ordinals.items())
        for k, v in sorted(names.items())
    ).encode("ascii")
    return {
        "source_sha256": hashlib.sha256(nand).hexdigest(),
        "ccfs_sha256": hashlib.sha256(ccfs).hexdigest(),
        "decoder": decoder,
        "ordinals": ordinals,
        "dll_ordinals": dll_ordinals,
        "ordinal_text": ordinal_text,
    }


def convert_cbea(
    cbea_bytes,
    cbea_name="package.cbea",
    runner=None,
    rom=None,
    selected_exes=None,
    override_id=None,
    override_name=None,
    override_version=None,
):
    """Convert a .cbea archive into one or more Glaze .gzp packages and an audit report."""
    archive = CbeaArchive(cbea_bytes)
    inf_entry = archive.find("Install.inf")
    if inf_entry is None:
        raise AssetError(f"{cbea_name}: CBEA archive has no Install.inf")
    inf = parse_cbea_inf(inf_entry["data"].decode("latin1"))

    if runner is not None:
        validate_runner(runner)

    resolved_files = []
    seen_dests = {}
    for copy in inf["copy_files"]:
        entry = archive.find(copy["src"])
        if entry is None:
            raise AssetError(f"{cbea_name}: Install.inf references missing entry {copy['src']}")
        payload = entry["data"]
        canonical = copy["canonical"]
        was_cpk = False
        if payload.startswith(b"CASIO.$&%-Z\x01"):
            if rom is None or rom.get("decoder") is None:
                raise AssetError(
                    f"{cbea_name}: entry {copy['src']} is CPK-compressed; supply --image /path/to/beshell.bin"
                )
            payload = rom["decoder"].unpack(payload)
            was_cpk = True
            if canonical.casefold().endswith(".cpk"):
                canonical = canonical[:-4]
        info = {
            "src": copy["src"],
            "canonical": canonical,
            "flags": copy["flags"],
            "was_cpk": was_cpk,
            "data": payload,
        }
        key = canonical.casefold()
        if key in seen_dests:
            if "COPYFLG_NOOVERWRITE" in copy["flags"].upper():
                continue
            resolved_files[seen_dests[key]] = info
        else:
            seen_dests[key] = len(resolved_files)
            resolved_files.append(info)

    exes = []
    dlls = []
    other_files = []
    for item in resolved_files:
        is_pe, is_dll = is_pe_binary(item["data"])
        if is_pe and not is_dll:
            exes.append(item)
        elif is_pe and is_dll:
            dlls.append(item)
        else:
            other_files.append(item)

    if not exes:
        raise AssetError(f"{cbea_name}: no WinCE MIPS executable found in [Copy Files]")

    # Place the primary ExeFile from [App Information] first.
    primary_canon = inf["exe_file"]
    if primary_canon.casefold().endswith(".cpk"):
        primary_canon = primary_canon[:-4]
    primary_base = primary_canon.rsplit("/", 1)[-1].casefold()
    exes.sort(
        key=lambda x: (
            0
            if x["canonical"].casefold() == primary_canon.casefold()
            or x["canonical"].rsplit("/", 1)[-1].casefold() == primary_base
            else 1
        )
    )

    total_exes = len(exes)
    if selected_exes:
        wanted = {s.casefold() for s in selected_exes}
        filtered = []
        for x in exes:
            bname = x["canonical"].rsplit("/", 1)[-1].casefold()
            stem = bname[:-4] if bname.endswith(".exe") else bname
            if bname in wanted or stem in wanted:
                filtered.append(x)
        if not filtered:
            available = ", ".join(x["canonical"].rsplit("/", 1)[-1] for x in exes)
            raise AssetError(f"{cbea_name}: --exe did not match any executable (available: {available})")
        exes = filtered

    if (override_id or override_name) and len(exes) > 1:
        raise AssetError("--id and --name overrides require a single executable (use --exe to select one)")

    ordinals = rom["ordinals"] if rom else None
    dll_ordinals = rom["dll_ordinals"] if rom else None
    ordinal_text = rom["ordinal_text"] if rom else b"coredll.dll 0 none\n"

    module_reports = {}
    for mod in exes + dlls:
        mname = mod["canonical"].rsplit("/", 1)[-1]
        rep = pe_inspect(mod["data"], ordinals, dll_ordinals)
        if rep["needs_ordinals"] and rom is None:
            raise AssetError(
                f"{cbea_name}: {mname} imports by ordinal; supply --image /path/to/beshell.bin"
            )
        module_reports[mname] = rep

    # Include launcher registry metadata alongside [Add Registry] entries.
    reg_items = list(inf["registry"])
    prog_name = inf["program"] or inf["caption"]
    if prog_name and inf["exe_file"]:
        menu_key = f"HKLM\\SOFTWARE\\MENU\\{prog_name}"
        win_install_dir = "\\Nand Disk" + inf["install_dir"].replace("/", "\\")
        reg_items.extend([
            {"key": menu_key, "name": "ANAME", "type": 1, "data": ((inf["caption"] or prog_name) + "\0").encode("utf-16le")},
            {"key": menu_key, "name": "APATH", "type": 1, "data": (inf["exe_file"].replace("/", "\\") + "\0").encode("utf-16le")},
            {"key": menu_key, "name": "INSTALL_DIR", "type": 1, "data": (win_install_dir + "\0").encode("utf-16le")},
        ])
    registry_blob = encode_gzce_registry(reg_items) if reg_items else None

    packages = {}
    package_meta = []
    cbea_stem = Path(cbea_name).stem

    for exe in exes:
        exe_bname = exe["canonical"].rsplit("/", 1)[-1]
        exe_stem = exe_bname[:-4] if exe_bname.casefold().endswith(".exe") else exe_bname
        if override_id:
            pkg_id = sanitize_pkg_id(override_id, prefix="")
        elif total_exes == 1:
            pkg_id = sanitize_pkg_id(cbea_stem)
        else:
            pkg_id = sanitize_pkg_id(exe_stem)

        if override_name:
            title = sanitize_display_name(override_name, multi_exe=False)
        elif total_exes == 1:
            title = sanitize_display_name(inf["caption"] or inf["program"] or exe_stem, multi_exe=False)
        else:
            title = sanitize_display_name(exe_stem, multi_exe=True)

        version = sanitize_version(override_version or inf["version"])
        try:
            icon_bytes = pe_icon(exe["data"])
        except AssetError:
            icon_bytes = default_icon()

        win_source = exe["canonical"].replace("/", "\\")
        entries = {
            "Info": f"id={pkg_id}\nname={title}\nversion={version}\nexec=app\n".encode("ascii"),
            "res/icon.gzi": icon_bytes,
            "res/program.exe": exe["data"],
            "res/ordinals.txt": ordinal_text,
            "res/source.txt": win_source.encode("ascii"),
        }
        if runner is not None:
            entries["app"] = runner
        if registry_blob is not None:
            entries["res/registry"] = registry_blob

        used_res = set(RESERVED_RES)
        manifest_lines = [f"program.exe {exe['canonical']}\n"]

        # Package DLLs and companion files up to the 64-entry Glaze package limit.
        companion_items = dlls + other_files
        max_companions = 64 - len(entries) - 1  # reserve 1 slot for res/files.txt
        for item in companion_items[:max_companions]:
            bname = item["canonical"].rsplit("/", 1)[-1]
            res_name = sanitize_res_name(bname, used_res)
            entries[f"res/{res_name}"] = item["data"]
            manifest_lines.append(f"{res_name} {item['canonical']}\n")

        entries["res/files.txt"] = "".join(manifest_lines).encode("utf-8")
        if runner is not None:
            packages[f"{pkg_id}.gzp"] = package(entries)
        package_meta.append({
            "id": pkg_id,
            "name": title,
            "version": version,
            "executable": exe["canonical"],
            "entry_count": len(entries),
        })

    report = {
        "cbea": cbea_name,
        "cbea_sha256": hashlib.sha256(cbea_bytes).hexdigest(),
        "version": archive.version,
        "target": archive.target,
        "inf": {
            "program": inf["program"],
            "caption": inf["caption"],
            "maker": inf["maker"],
            "version": inf["version"],
            "destination": inf["destination"],
            "install_dir": inf["install_dir"],
            "exe_file": inf["exe_file"],
            "copy_count": len(inf["copy_files"]),
            "registry_count": len(inf["registry"]),
        },
        "packages": package_meta,
        "modules": module_reports,
    }
    return packages, report, resolved_files


def dump_cbea_summary(cbea_path):
    """Print a human-readable dump of a .cbea archive and its Install.inf."""
    raw = cbea_path.read_bytes()
    archive = CbeaArchive(raw)
    print(f"=== {cbea_path.name}: version={archive.version:#x}, target={archive.target!r}, {len(archive.entries)} entries ===")
    for idx, entry in enumerate(archive.entries):
        kind_hex = entry["kind"].hex()
        cpk_tag = " [CPK]" if entry["data"].startswith(b"CASIO.$&%-Z\x01") else ""
        pe_tag = ""
        is_pe, is_dll = is_pe_binary(entry["data"])
        if is_pe:
            pe_tag = " [PE DLL]" if is_dll else " [PE EXE]"
        print(f"  [{idx:2d}] kind={kind_hex} size={entry['size']:8d}  {entry['name']}{cpk_tag}{pe_tag}")
    inf_entry = archive.find("Install.inf")
    if inf_entry:
        inf = parse_cbea_inf(inf_entry["data"].decode("latin1"))
        print(f"  Program:     {inf['program']!r} (Caption: {inf['caption']!r}, Version: {inf['version']!r})")
        print(f"  InstallDir:  {inf['install_dir']} (Destination: {inf['destination']})")
        print(f"  ExeFile:     {inf['exe_file']}")
        print(f"  Copy Files:  {len(inf['copy_files'])}, Registry Values: {len(inf['registry'])}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("cbea", nargs="*", type=Path, help=".cbea package file(s) to convert")
    parser.add_argument("--cbea", dest="cbea_opt", type=Path, action="append", default=[],
                        help=".cbea package file (repeatable)")
    parser.add_argument("-o", "--output", type=Path, default=Path("out/cbea"),
                        help="output directory or .gzp file path (default: out/cbea)")
    parser.add_argument("--image", type=Path,
                        help="local BE-300 NAND image for ROM ordinals and CPK decompression")
    parser.add_argument("--no-image", action="store_true",
                        help="do not auto-detect ../be300/nand/beshell.bin")
    parser.add_argument("--runner", type=Path, default=Path("out/romfs/bin/wince"),
                        help="Glaze WinCE runner ELF (default: out/romfs/bin/wince)")
    parser.add_argument("--exe", action="append",
                        help="executable name/stem to package (repeatable; default: all executables in CBEA)")
    parser.add_argument("--id", help="override Glaze package ID (single-executable conversion only)")
    parser.add_argument("--name", help="override home-screen label (single-executable conversion only)")
    parser.add_argument("--version", help="override package version")
    parser.add_argument("--list", action="store_true", help="list CBEA archive contents and metadata without building packages")
    parser.add_argument("--extract", nargs="?", const=Path(""), type=Path,
                        help="extract unpacked CBEA files into DIR (default: <output>/extracted)")
    parser.add_argument("--audit-only", action="store_true", help="inspect and write audit.json without building .gzp packages")
    args = parser.parse_args()

    inputs = list(args.cbea) + list(args.cbea_opt)
    if not inputs:
        parser.error("at least one .cbea file is required")

    if args.list:
        for path in inputs:
            dump_cbea_summary(path)
        return

    image_path = args.image
    if image_path is None and not args.no_image:
        default_nand = Path(__file__).resolve().parent.parent.parent / "be300/nand/beshell.bin"
        if default_nand.is_file():
            image_path = default_nand

    rom = load_rom_Support(image_path) if image_path is not None else None
    runner = None if args.audit_only else args.runner.read_bytes()

    all_packages = {}
    reports = []
    for path in inputs:
        pkgs, rep, resolved = convert_cbea(
            path.read_bytes(),
            cbea_name=path.name,
            runner=runner,
            rom=rom,
            selected_exes=args.exe,
            override_id=args.id,
            override_name=args.name,
            override_version=args.version,
        )
        all_packages.update(pkgs)
        reports.append(rep)
        if args.extract is not None:
            base_extract = (args.output / "extracted") if str(args.extract) in ("", ".") else args.extract
            out_extract = base_extract / path.stem if len(inputs) > 1 else base_extract
            for item in resolved:
                rel = item["canonical"].lstrip("/")
                target = out_extract / rel
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(item["data"])

    if args.output.suffix.lower() == ".gzp":
        if len(all_packages) != 1:
            raise AssetError("output path ends with .gzp, but conversion produced multiple packages")
        args.output.parent.mkdir(parents=True, exist_ok=True)
        _, gzp_bytes = next(iter(all_packages.items()))
        args.output.write_bytes(gzp_bytes)
        pkg_info = reports[0]["packages"][0]
        print(f"[cbea2gzp] {args.output}: id={pkg_info['id']} name={pkg_info['name']!r} version={pkg_info['version']} ({len(gzp_bytes) // 1024} KiB)")
        return

    args.output.mkdir(parents=True, exist_ok=True)
    for filename, gzp_bytes in all_packages.items():
        out_path = args.output / filename
        out_path.write_bytes(gzp_bytes)
    audit_data = reports[0] if len(reports) == 1 else {"archives": reports}
    (args.output / "audit.json").write_text(json.dumps(audit_data, indent=2) + "\n")

    for rep in reports:
        for mod_name, info in rep["modules"].items():
            count = sum(len(d["symbols"]) for d in info["imports"])
            unresolved = len(info["unresolved_ordinals"])
            extra = f" ({unresolved} unknown ROM ordinals)" if unresolved else ""
            print(f"{rep['cbea']}:{mod_name}: MIPS PE32, {info['bytes']} bytes, {count} imports{extra}")
        for pkg_info in rep["packages"]:
            print(f"  -> {pkg_info['id']}.gzp: name={pkg_info['name']!r}, version={pkg_info['version']}, entries={pkg_info['entry_count']}")
    print(f"Wrote {len(all_packages)} package(s) to {args.output}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, AssetError, UnicodeError) as exc:
        print(f"cbea2gzp: {exc}", file=sys.stderr)
        sys.exit(1)
