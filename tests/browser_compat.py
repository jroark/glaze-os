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

import gzip
import http.server
import os
import re
import subprocess
import sys
import threading

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
    # Methods and properties pages call without testing for them.
    "dom_api": ("dom_api.html",
                [r"^browser JS: api matches=true$", r"^browser JS: api closest=boxtrue$", r"^browser JS: api dataset=42ztrue$", r"^browser JS: api classList=e\ c\ d/3true$", r"^browser JS: api hidden=true,false$", r"^browser JS: api append=headBafterbefore$", r"^browser JS: api adjacent=2>$", r"^browser JS: api siblings=two4p$", r"^browser JS: api attrs=id1truetrue$", r"^browser JS: api consts=13816$", r"^browser JS: api collections=1111$", r"^browser JS: api form=2x@example\.orgf$", r"^browser JS: api events=7pingpong5$", r"^browser JS: api impl=2$", r"^browser JS: api console=ok$", r"^browser JS: api dialogs=true,null$", r"^browser JS: api window=booleantruefalsenumberfunction10$", r"^browser JS: api scroll=ok$", r"^browser JS: api geometry=truetruetrue1$", r"^browser JS: api doc=CSS1Compatvisiblefalsetruetrue$", r"^browser JS: api strtimer=ok$", r"^browser JS: api micro=ok$", r"^browser JS: api idle=true$"],
                [r"^browser JS: api \\w+!"], 300),
    # XMLHttpRequest, as old sites and jQuery's $.ajax use it.
    "xhr": ("http:xhr.html",
            [r'^browser JS: xhr get 200 4 \{"answer": 42\} application/json 42$',
             r"^browser JS: xhr post 200 posted a=1&b=2 as application/x-www-form-urlencoded$",
             r"^browser JS: xhr missing 404 load$", r"^browser JS: xhr states 2,3,4$"],
            [r"browser JS: .*Error"], 600),
    # Referer on a page's requests, and Origin with it on a form post.
    "headers": ("http:headers.html",
                [r"^browser JS: referer: SELF/headers\.html$", r"^browser JS: no content-type on get$",
                 r'item text .*"formpost origin=SELF"', r'item text .*"referer=SELF/headers\.html"', r'item text .*"body=a=1"'],
                [], 300),
    # A document past the size limit is shown cut short, however it arrives.
    "big": ("http:big", [r'"startmark"', r'"This page is too large to show in full'], [r'"endmark"'], 0),
    "big_chunked": ("http:big-chunked", [r'"startmark"', r'"This page is too large to show in full'], [r'"endmark"'], 0),
    "big_gzip": ("http:big-gzip", [r'"startmark"', r'"This page is too large to show in full'], [r'"endmark"'], 0),
    "bomb": ("http:bomb", [r'"startmark"', r'"This page is too large to show in full'], [r'"endmark"'], 0),
    # What is not a page is shown as text or refused, not parsed as one.
    "type_json": ("http:json", [r'item text .* <pre> "\{"answer":"', r'<pre> "42\}"'], [], 0),
    "type_xml": ("http:feed", [r'item text .* <pre> "<\?xml"', r'<pre> "itle>feedtitle</title>'], [], 0),
    "type_binary": ("http:blob", [r"This kind of file can't be shown\."], [], 0),
    "type_svg": ("http:svg", [r"This kind of file can't be shown\."], [r'"svgwords"'], 0),
    "type_sniffed": ("http:mislabelled", [r'<p> "markupanyway"'], [], 0),
    "type_missing": ("http:untyped", [r'<p> "notypegiven"'], [], 0),
    "coding_br": ("http:br", [r'"The data could not be decoded\."'], [r"brotlinoise"], 0),
    "status_503": ("http:down", [r"The server reported an error\. Try"], [], 0),
    "status_403": ("http:denied", [r"The server refused to show this"], [], 0),
    "status_404": ("http:lost", [r'"The page could not be found\."'], [], 0),
    "many_nodes": ("http:many", [r'"startmark"', r'"This page is too large to show in full'], [r'"endmark"'], 0),
}


def run(browse, name, port):
    page, want, never, settle = CASES[name]
    # "http:page" is fetched from the local server; scripts on file pages
    # have no origin to fetch or store with.
    target = (f"http://127.0.0.1:{port}/{page[5:]}" if page.startswith("http:")
              else os.path.join(PAGES, page))
    env = dict(os.environ, BROWSE_ITEMS="1", BROWSE_HTML="1", BROWSE_FORMS="1", BROWSE_FOLLOW="1", BROWSE_SETTLE_MS=str(settle),
               ASAN_OPTIONS="detect_leaks=0")
    p = subprocess.run([os.path.abspath(browse), target, os.devnull],
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


class Handler(http.server.SimpleHTTPRequestHandler):
    """The compat pages over HTTP, plus a few answers no file can give."""

    def __init__(self, *a, **kw):
        super().__init__(*a, directory=PAGES, **kw)

    def log_message(self, *a):
        pass

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length") or 0))
        if self.path == "/form":
            text = "formpost origin=%s referer=%s body=%s" % (self.headers.get("Origin"), self.headers.get("Referer"), body.decode())
            self.reply(200, "text/html", ("<p>%s</p>" % text.replace("http://127.0.0.1:%d" % self.server.server_address[1], "SELF")).encode())
            return
        self.reply(200, "text/plain", b"posted " + body + b" as " + (self.headers.get("Content-Type") or "").encode())

    def do_GET(self):
        path = self.path.split("?")[0]
        if path == "/echo-headers":
            lines = "".join(f"{k.lower()}: {v}\n" for k, v in self.headers.items())
            self.reply(200, "text/plain", lines.replace("http://127.0.0.1:%d" % self.server.server_address[1], "SELF").encode())
        elif path == "/json":
            self.reply(200, "application/json", b'{"answer": 42}')
        elif path == "/feed":
            self.reply(200, "application/rss+xml; charset=utf-8",
                       b"<?xml version='1.0'?><rss><channel><title>feedtitle</title></channel></rss>")
        elif path == "/blob":
            self.reply(200, "application/octet-stream", bytes(range(256)) * 8)
        elif path == "/mislabelled":
            self.reply(200, "application/octet-stream", b"<!doctype html><p>markupanyway</p>")
        elif path == "/untyped":
            self.reply(200, "", b"<p>notypegiven</p>")
        elif path == "/svg":
            self.reply(200, "image/svg+xml", b"<svg xmlns='http://www.w3.org/2000/svg'><text>svgwords</text></svg>")
        elif path == "/br":
            self.reply(200, "text/html", bytes([0x8b, 0x02, 0x80]) + b"<p>brotlinoise</p>" + bytes([3]),
                       [("Content-Encoding", "br")])
        elif path in ("/down", "/denied", "/lost"):
            self.reply({"/down": 503, "/denied": 403, "/lost": 404}[path], "text/html", b"")
        elif path in ("/big", "/big-chunked", "/big-gzip", "/bomb", "/many"):
            # Longer than the engine's 1 MiB document limit, four ways, and
            # one with more nodes than it keeps.
            filler = b"<p>filler %06d " + b"of a very long page " * 45 + b"</p>\n"
            n = 60000 if path == "/bomb" else 2000
            if path == "/many":
                filler, n = b"<p>n%06d</p>\n", 30000
            body = (b"<!doctype html><title>big</title><p>startmark</p>\n"
                    + b"".join(filler % i for i in range(n)) + b"<p>endmark</p>")
            if path in ("/big", "/many"):
                self.reply(200, "text/html", body)
            elif path == "/big-chunked":
                self.send_response(200)
                self.send_header("Content-Type", "text/html")
                self.send_header("Transfer-Encoding", "chunked")
                self.end_headers()
                try:
                    for i in range(0, len(body), 50000):
                        part = body[i:i + 50000]
                        self.wfile.write(b"%x\r\n" % len(part) + part + b"\r\n")
                    self.wfile.write(b"0\r\n\r\n")
                except OSError:
                    pass    # the browser stops reading at its limit
            else:
                self.reply(200, "text/html", gzip.compress(body, 6), [("Content-Encoding", "gzip")])
        else:
            super().do_GET()

    def reply(self, status, ctype, body, extra=()):
        self.send_response(status)
        if ctype:
            self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        for k, v in extra:
            self.send_header(k, v)
        self.end_headers()
        try:
            self.wfile.write(body)
        except OSError:
            pass            # the browser stops reading at its limit


def main():
    browse = sys.argv[1]
    names = sys.argv[2:] or list(CASES)
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    failed = [n for n in names if not run(browse, n, server.server_address[1])]
    server.shutdown()
    print(f"browser compat: {len(names) - len(failed)}/{len(names)} cases pass"
          + ("" if failed else " PASS"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
