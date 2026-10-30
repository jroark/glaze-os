#!/usr/bin/env python3
"""Compare kernel register snapshots (kernel/hwdump.c, HWDUMP=1 builds).

    tools/hwdump_diff.py COLD.BIN WARM.BIN [--bediag be300/docs/hardware] [--all]

Each dump holds slot 0 (kernel entry) and slot 1 (after driver setup). The
table lists every register whose values differ between the columns; the
"cold.1 != warm.1" rows are what Windows CE leaves set that Glaze does not.
With --bediag, values from BEDiag's post-CE text dumps are added as a
reference column (hw_dump_vr4131.txt, hw_dump_vrc4173.txt).
"""

import argparse
import os
import re
import struct
import sys

MAGIC, SLOT = 0x57485A47, 0x544F4C53


def parse(path):
    data = open(path, "rb").read()
    words = struct.unpack("<%dI" % (len(data) // 4), data[: len(data) // 4 * 4])
    if len(words) < 4 or words[0] != MAGIC or words[1] != 1:
        sys.exit("%s: not a version 1 hardware dump" % path)
    boot = words[3]
    slots, i = {}, 4
    while i < len(words):
        if words[i] != SLOT or i + 5 > len(words):
            sys.exit("%s: bad slot header at word %d" % (path, i))
        index = words[i + 1]
        regs = {"cp0.status": words[i + 2], "cp0.config": words[i + 3]}
        i += 5
        while i < len(words) and words[i] != SLOT:
            pa, desc = words[i], words[i + 1]
            if desc & 0x80000000:               # skipped: its clock gate may be off
                regs["skip.%08X" % pa] = 1
                i += 2
                continue
            width, count = (desc >> 16) & 0x7FFF, desc & 0xFFFF
            stride = 4 if width in (1, 4) else 2
            vals = words[i + 2 : i + 2 + count]
            if len(vals) != count:
                sys.exit("%s: truncated window %08X" % (path, pa))
            if pa == 0x0A008680 and count == 2:     # SIU divisor latch
                regs["siu.dll"], regs["siu.dlm"] = vals
            else:
                for j, v in enumerate(vals):
                    regs[pa + j * stride] = v
            i += 2 + count
        slots[index] = regs
    return boot, slots


def bediag(folder):
    """Last value BEDiag printed for each word address (32-bit reads)."""
    out = {}
    for name in ("hw_dump_vr4131.txt", "hw_dump_vrc4173.txt"):
        path = os.path.join(folder, name)
        if not os.path.exists(path):
            continue
        for line in open(path, errors="replace"):
            m = re.match(r"0x([0-9A-Fa-f]{8}):((?:\s+[0-9A-Fa-f]{8})+)", line)
            if not m:
                continue
            base = int(m.group(1), 16)
            for j, v in enumerate(m.group(2).split()):
                out[base + 4 * j] = int(v, 16)
    return out


def bediag_value(ref, key):
    if not isinstance(key, int):
        return None
    word = ref.get(key & ~3)
    if word is None:
        return None
    if key >= 0x0F000000 and key < 0x0F001000:      # 16-bit register in a 32-bit read
        return (word >> (8 * (key & 2))) & 0xFFFF
    return word


def describe(boot):
    if not boot:
        return "SPL/NAND boot"
    flags = boot & 0x7FFFFFFF
    return "RAM boot, flags 0x%x%s" % (flags, " (from ROM)" if flags & 8 else "")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("cold")
    ap.add_argument("warm", nargs="?")
    ap.add_argument("--bediag", help="folder holding BEDiag's hw_dump_*.txt")
    ap.add_argument("--all", action="store_true", help="also print unchanged registers")
    args = ap.parse_args()

    dumps = [("cold", args.cold)] + ([("warm", args.warm)] if args.warm else [])
    cols, keys = [], []
    for label, path in dumps:
        boot, slots = parse(path)
        print("%s: %s, %s, slots %s" % (label, path, describe(boot), sorted(slots)))
        for s in sorted(slots):
            cols.append(("%s.%d" % (label, s), slots[s]))
            keys += [k for k in slots[s] if k not in keys]
    ref = bediag(args.bediag) if args.bediag else {}
    if ref:
        cols.append(("bediag", None))

    def value(col, key):
        name, regs = col
        v = bediag_value(ref, key) if regs is None else regs.get(key)
        return "-" if v is None else "%X" % v

    def keyname(k):
        return "%08X" % k if isinstance(k, int) else k

    keys.sort(key=lambda k: (isinstance(k, int), k if isinstance(k, int) else 0, str(k)))
    print("\n%-10s " % "register" + " ".join("%9s" % c[0] for c in cols) + "  note")
    for k in keys:
        vals = [value(c, k) for c in cols]
        note = ""
        named = dict((c[0], c) for c in cols)
        if "cold.1" in named and "warm.1" in named:
            if value(named["cold.1"], k) != value(named["warm.1"], k):
                note = "cold.1 != warm.1"
        if not args.all and len(set(v for v in vals if v != "-")) <= 1:
            continue
        print("%-10s " % keyname(k) + " ".join("%9s" % v for v in vals) + "  " + note)


if __name__ == "__main__":
    main()
