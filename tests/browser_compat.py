#!/usr/bin/env python3
"""Compatibility checks for the browser engine, natively.

usage: tests/browser_compat.py out/host/browse [case ...]

Each case renders a small page from tests/web/compat/ with host/browse.c and
checks its output: the display items (BROWSE_ITEMS: "item <kind> x,y wxh
<tag> ..."), the document as scripts left it (BROWSE_HTML: "html ...") and
the script console ("browser JS: ..."). `want` patterns must each match a
line, `never` patterns must match none. The pages are original, written to
show one thing a real page does that the engine once dropped.
"""

import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PAGES = os.path.join(ROOT, "tests", "web", "compat")

# name: (page, want, never, settle_ms)
CASES = {
    # display:block on a replaced element still draws it; auto margins centre it.
    "block_replaced": ("block_replaced.html",
                       [r"item image \d+,\d+ 24x24 <img>", r"item ctl .* <input>", r"item ctl .* <select>",
                        r"item ctl .* <textarea>", r"item image 108,"],
                       [], 0),
    # :not() is evaluated, with :is(), :nth-child() and :empty.
    "selectors": ("selectors.html",
                  [r'item text .*"linked" link', r'"shownpanel"', r'"kept"', r"browser JS: l1=rgb\(0, 0, 238\) q=2 n=1",
                   r'item text .* <li> "two"', r'"greenhead"', r'"plainhead"'],
                  [r'"hiddenpanel"', r'"gone"'], 0),
    # media= on a sheet, @layer blocks and range media queries.
    "sheets": ("sheets.html",
               [r'"deskvisible"', r'"printvisible"', r'"widevisible"',
                r"browser JS: rgb\(0, 128, 0\) rgb\(255, 0, 0\) rgb\(0, 0, 255\) rgb\(128, 0, 128\)"],
               [], 0),
}


def run(browse, name):
    page, want, never, settle = CASES[name]
    env = dict(os.environ, BROWSE_ITEMS="1", BROWSE_HTML="1", BROWSE_SETTLE_MS=str(settle),
               ASAN_OPTIONS="detect_leaks=0")
    p = subprocess.run([os.path.abspath(browse), os.path.join(PAGES, page), os.devnull],
                       cwd=os.path.join(ROOT, "host"), env=env, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, text=True, errors="replace", timeout=120)
    lines = p.stdout.splitlines()
    bad = []
    if p.returncode:
        bad.append(f"exit status {p.returncode}")
    for pat in want:
        if not any(re.search(pat, line) for line in lines):
            bad.append(f"missing: {pat}")
    for pat in never:
        hit = [line for line in lines if re.search(pat, line)]
        if hit:
            bad.append(f"unwanted: {pat} ({hit[0][:80]})")
    if bad:
        print(f"browser compat: FAIL {name}")
        for b in bad:
            print(f"    {b}")
        if os.environ.get("COMPAT_VERBOSE"):
            print(p.stdout)
    return not bad


def main():
    browse = sys.argv[1]
    names = sys.argv[2:] or list(CASES)
    failed = [n for n in names if not run(browse, n)]
    print(f"browser compat: {len(names) - len(failed)}/{len(names)} cases pass"
          + ("" if failed else " PASS"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
