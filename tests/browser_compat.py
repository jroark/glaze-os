#!/usr/bin/env python3
"""Compatibility checks for the browser engine, natively.

usage: tests/browser_compat.py out/host/browse [case ...]

Each case renders a small page from tests/web/compat/ with host/browse.c and
checks its output: the display items (BROWSE_ITEMS: "item <kind> x,y wxh
<tag> ..."), the document as scripts left it (BROWSE_HTML: "html ...") and
the script console ("browser JS: ..."). `want` patterns must each match a
line, `never` patterns must match none (the DOM dump aside, unless the
pattern starts with ^html). The pages are original, written to
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
    # var() from :root and from the rule's own block, fallbacks, calc() and clamp().
    "vars": ("vars.html",
             [r"browser JS: a rgb\(13, 110, 253\) b rgb\(220, 53, 69\) c rgb\(25, 135, 84\) e 14px "
              r"f rgb\(255, 255, 0\) g rgb\(13, 110, 253\) w 30\d"],
             [], 0),
    # Controls find their form through form=, and in <table><form><tr>; labels find controls.
    "forms": ("forms.html",
              [r"ctl q form search", r"ctl who form legacy", r"ctl send form legacy", r"ctl far form search",
               r"ctl loose form \(none\)", r"label -> agree", r"label -> far", r"label -> \(nothing\)"],
              [], 0),
    # rem follows the root font size; font: inherit keeps weight.
    "rem": ("rem.html", [r"browser JS: 20px 10px bold"], [], 0),
    # srcset, <picture> and lazy-load attributes choose what is fetched.
    "image_source": ("image_source.html",
                     [r"image .*/w480\.jpg$", r"image .*/one\.png$", r"image .*/fallback\.jpg$",
                      r"image .*/lazy\.jpg$", r"image .*/pic-small\.jpg$", r"image .*/plain\.gif$",
                      r"image .*/widest\.jpg$"],
                     [r"^image .*w1600", r"^image .*\.webp", r"^image data:", r"^image .*/two\.png", r"^image .*desktop"], 0),
    # rowspan keeps later rows in their columns; content outside rows and rowless cells show.
    "tables": ("tables.html",
               [r'"r1c2"', r'"loosenote"', r'"inrow"', r'"cellone"', r'"celltwo"', r"browser JS: true true true true"],
               [], 0),
    # Gradients fall back to a colour; off-canvas and transparent positioned boxes hide;
    # <dialog open> and <object> fallbacks show; <template> is inert.
    "hidden": ("hidden.html",
               [r'"heroband"', r'"slideon"', r'"fadein"', r'"centredbox"', r'"opendialog"', r'"objectfallback"',
                r"browser JS: rgb\(32, 64, 96\)"],
               [r'"drawerone"', r'"drawertwo"', r'"slideoff"', r'"rightpanel"', r'"closeddialog"',
                r'"templatetext"', r"^image .*template-only", r"^browser JS: template ran"], 0),
    # Async and script-inserted scripts hold nothing up; defer runs last; nomodule is skipped.
    "scripts_order": ("scripts_order.html",
                      [r"^browser JS: order inline1,typed,xjs,inline2,deferred$"], [r"NOMODULE", r"TEMPLATE,"], 0),
}


def run(browse, name):
    page, want, never, settle = CASES[name]
    env = dict(os.environ, BROWSE_ITEMS="1", BROWSE_HTML="1", BROWSE_FORMS="1", BROWSE_SETTLE_MS=str(settle),
               ASAN_OPTIONS="detect_leaks=0")
    p = subprocess.run([os.path.abspath(browse), os.path.join(PAGES, page), os.devnull],
                       cwd=os.path.join(ROOT, "host"), env=env, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, text=True, errors="replace", timeout=120)
    lines = p.stdout.splitlines()
    # The DOM dump runs from its "html " line to the summary line.
    in_dump, dump = False, set()
    for i, line in enumerate(lines):
        if line.startswith("html "):
            in_dump = True
        elif in_dump and re.match(r'^".*": \d+ items', line):
            in_dump = False
        if in_dump:
            dump.add(i)
    bad = []
    if p.returncode:
        bad.append(f"exit status {p.returncode}")
    for pat in want:
        if not any(re.search(pat, line) for line in lines):
            bad.append(f"missing: {pat}")
    for pat in never:
        # The DOM dump quotes the page's own source, so `never` patterns
        # leave it out unless they ask for it.
        hit = [line for i, line in enumerate(lines)
               if re.search(pat, line) and (pat.startswith("^html") or i not in dump)]
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
