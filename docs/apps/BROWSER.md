# Browser JavaScript and frames

Glaze uses [JerryScript](https://github.com/jerryscript-project/jerryscript),
with a small native DOM facade. JerryScript's C99 implementation and explicit
heap/stack limits fit Glaze's freestanding MIPS runtime and 16 MiB RAM better
than Escargot's C++ runtime and garbage collector port. This is a lightweight
browser for classic pages, not a Chromium-compatible application platform.

Open **JavaScript & frames** on the bundled start page (`res:script.html`) for
an offline demonstration of DOM updates, buttons, timers, child scripts,
same-origin DOM access and named frame navigation. `res:frames.html` demonstrates
legacy framesets. Drag inside a long frame to scroll its contents.

## Supported interfaces

- Inline and external classic scripts, executed once in DOM order, with syntax
  and runtime errors logged to the browser console. External scripts use the
  existing HTTP/HTTPS/file/res/data loader, redirects and cookies.
- JerryScript's JavaScript language and standard objects, including numbers,
  Math, Date, JSON, arrays, typed arrays, Map/Set, promises and functions.
  ES modules are enabled; BigInt is disabled. SharedArrayBuffer and Atomics are hidden;
  there are no browser workers.
- `window`, `self`, `globalThis`, `document`, `location`, console logging,
  `alert` (console output), function-based timers, and event listeners.
- `performance.now()` and `timeOrigin`, binary-string `atob`/`btoa`, location
  URL components, viewport/screen dimensions, and detached `Image` objects with load/error events and natural
  dimensions. Function `toString()` preserves JavaScript source.
- `document.cookie` shares the HTTP cookie jar (64 cookies; those with an
  expiry are kept in `/Browser/cookies` between runs), with path, Secure,
  expiry and HttpOnly handling. Script-set Domain attributes are conservatively
  limited to the document hostname; opaque and non-HTTP origins expose no cookies.
- A per-document [`navigator`](https://html.spec.whatwg.org/multipage/system-state.html#the-navigator-object)
  (`clientInformation`) with read-only identity,
  language, cookie-support, processor-count and touch-point properties. Its
  user agent and language match the HTTP request headers. Unsupported methods
  such as `sendBeacon` are absent, allowing site feature checks and polyfills.
- Node creation, insertion/removal, traversal, attributes, selectors, IDs,
  text/HTML replacement, classes, common style properties, form values,
  checked/disabled state, form submission/reset and `document.write`.
- Click bubbling, inline handlers, `preventDefault`, `stopPropagation`, input,
  change, submit/reset, `DOMContentLoaded` and load events.
- `iframe` (`src` and `srcdoc`) and `frameset`/`frame` (rows, columns, nesting),
  clipped painting, independent scrolling, child links/forms, named targets,
  `_self`, `_parent` and `_top`. `_blank`, and a target no frame is named,
  open in the current top-level view.
- Per-document globals. Same-origin frames expose `contentDocument`,
  `contentWindow`, parent/top and named entries in `frames`; foreign or
  sandboxed contexts return null. Native node references expire on unload.

The bounded Web APIs also include fetch, AbortController, URL/URLSearchParams,
UTF-8 text conversion, local/session storage, fragments, cloning, richer events,
layout measurements, animation callbacks, and static/dynamic ES modules.
See [BROWSER_WEB_APIS.md](BROWSER_WEB_APIS.md) for interfaces, quotas, security
checks, examples, and deliberate compatibility limits. `res:apis.html` is an
offline demonstration linked from the welcome page.

## Limits and compatibility

Scripts run after HTML parsing, in document order. An external script marked
`async`, or created by a script and inserted, runs when it arrives and holds
nothing up; `defer` scripts and modules run in order after the plain ones;
`nomodule` scripts are skipped. `document.write` inserts after the executing script,
or appends to the body outside script loading. Scripts inserted through
`innerHTML` stay inert. Node lists are snapshots. The DOM and CSS interfaces
are a subset; layout remains Glaze's integer layout engine. There is no
canvas scripting, WebAssembly or IndexedDB, and no synchronous XHR
(`XMLHttpRequest` is asynchronous only, on fetch and within its limits).
`history.pushState`/`replaceState` keep their state and length but do not
change the address shown. `MutationObserver` and `IntersectionObserver` are
absent on purpose: pages test for them and fall back, which a stub that never
called back would break. Fetch and origin-scoped storage are bounded subsets described above. Child navigation does not create top-level history entries. Unicode
text entering the rendered DOM uses Glaze's existing font/charset mappings.

With scripting enabled, `noscript` contents are inert raw text in both the head
and body, including HTML inserted through DOM edits or `document.write`.
Sandboxed child documents parse and display fallback HTML with scripting
disabled. A later script exception or VM shutdown does not change the document's
parsing mode or activate its fallback metadata and resources.

The shared VM has a 1 MiB heap and a 24 KiB MIPS stack budget (native ASan tests
allow 128 KiB for larger C frames). Document startup scripts have a 20-second
deadline to accommodate compilation on MIPS; interactive scripts, events and
timers have a 750 ms deadline. A work counter also bounds each entry, checked
at VM loop jumps, native regular-expression steps and promise jobs. Timers are
capped at 32 per document. Heap exhaustion discards
the VM and disables JavaScript in its active documents; the DOM remains
readable and subsequent navigation can create a fresh VM. Top-level navigation
retires the old tree's script contexts while leaving its rendered page visible
until the replacement is ready.

Resource bounds are 32 scripts per document, eight child browsing contexts
across the tree, three embedding levels, 128 image slots per document and a
shared 3 MiB decoded-image budget. Child documents are cut short at 64 KiB
and top-level ones at 1 MiB (see below); external scripts are capped at 128 KiB.
A document's scripts can hold 4096 node handles, and `querySelectorAll`
returns up to 1024 nodes. DOM edits are bounded by the node/depth limits,
64 KiB per edit and a 4 MiB arena ceiling per document.

HTTP script-src/default-src policies are checked for supported source forms
(self, exact origins, schemes, nonce and unsafe-inline/unsafe-eval). Unsupported
expressions grant no permission. X-Frame-Options and CSP frame-ancestors are
checked against embedding ancestors. This is not a complete CSP implementation.
Any sandbox attribute conservatively disables child JavaScript and parent DOM
access; sandbox tokens do not grant exceptions. Network pages cannot navigate
or load resources from file/res schemes, including through HTTP redirects.
File documents have distinct origins; about:blank/srcdoc inherit their creator.

## Build and test

`make -j4` builds the browser and `out/glaze.bin`. The engine, fdlibm math
routines and selected LLVM soft-float helpers are browser-private archives;
other apps do not acquire a JavaScript runtime dependency. The factory romfs
(LZ4-compressed) occupies about two thirds of its slot without the opt-in
drivers; its slot is separate from user file storage. Build provenance
and local vendor changes are recorded in `third_party/*/UPSTREAM.md`.

`make -C host browser-test` runs ASan/UBSan integration tests against a local
HTTP fixture server: DOM/events/timers, script order, errors and execution
budgets, allocation failure/restart, frame navigation, origin isolation,
sandboxing, CSP/XFO, framesets and embedding limits, plus the Web API, CORS,
storage persistence and module tests described in the companion document.

`make -C host browser-compat-test` runs `tests/browser_compat.py`: one small
original page per compatibility fix (`tests/web/compat/`), rendered by
`out/host/browse` and checked on its display items, the DOM after scripts, the
console, or what a local HTTP/HTTPS server was sent. See
[Page compatibility](#page-compatibility) for what it covers and how to add a case.

`make -C host browser-google` builds an opt-in live workflow check. Run
`(cd host && ../out/host/browser-google)` to open Google, submit the default
`example domain` query, locate the example.com result and open it. Optional
arguments select the query and expected result hostname. Exit code 3 means
Google requires human verification; it does not count as a compatibility pass.
The live check is separate from the deterministic integration tests.

`make INPUT_SCRIPT=tests/ui/t_browser_script.txt` builds the offline device smoke
test. Run a copy of the NAND under be300 for 36 seconds and check its console
for `browser demo:` messages, including math/JSON, click, timer, child script
and same-origin frame results. The test also opens `res:script-limits.html`
to exercise MIPS parser jumps, an infinite loop, heap exhaustion and fresh-VM
recovery on navigation. Rebuild without INPUT_SCRIPT for normal use.

## Page compatibility

What the engine does with things ordinary pages rely on, beyond the script
interfaces above. Each item has a case in `tests/browser_compat.py`; the
commit that added it says what a page looked like before.

**Style and layout**

- `<img>`, `<input>`, `<select>` and `<textarea>` with `display: block` are
  drawn on their own line, centred by auto margins (CSS frameworks set this on
  every form control and image).
- Selectors: `:not()`, `:is()` and `:where()` with lists of compound
  selectors, `:nth-child()`, `:first-child`, `:last-child`, `:empty`,
  `:checked` and `:disabled`. `:lang()`, `:dir()`, `:enabled` and `:scope`
  always match; `:has()`, the `-of-type` and `nth-last-` forms, `:hover` and
  the rest never do.
- `media=` on `<link>` and `<style>`, range media queries (`width >= 600px`),
  and the contents of `@layer` blocks. The screen is 320 CSS px wide, portrait.
  `@import` is not followed.
- `var()` with fallbacks, and `calc()`, `min()`, `max()` and `clamp()` of
  lengths in one unit, and of percentages. Custom properties declared on
  `:root`, `html` or `body` are settled when a sheet is parsed. Ones set by
  other rules (a component's variant class setting what its base class uses)
  are scoped: the declarations that use them are kept as text and parsed in
  the cascade for each element, against the custom properties of the rules
  matching it, its `style` attribute and its ancestors. A name must be set in
  the sheet that uses it to be recognized as scoped, and an element takes
  custom properties from at most six of its rules.
- `rem` follows the root font size; `font: inherit` keeps weight and style.
- A background given only as a gradient uses its first opaque colour stop.
- Positioning and floats are still ignored, so things that are off screen in a
  real browser are hidden outright: positioned boxes moved off canvas with
  `transform: translate*(-100%)`, `left`/`right` of -100% or far negative
  lengths, and positioned boxes with `opacity: 0`.
- Tables: `rowspan` keeps later rows in their columns (the spanning cell is
  drawn in its first row), cells with no row form one, and content loose in a
  table is laid out above its rows. Wide tables are still linearized.
- `<dialog open>` and `<object>` fallback content are shown; a `<template>`'s
  contents are inert (no scripts run, nothing fetched).
- Flex and grid containers lay their children out as blocks.

**Forms and images**

- A control belongs to the form named by `form=`, else an ancestor form, else
  the last empty `<form>` before it in its table (`<table><form><tr>`, where
  the parser closes the form early). A tap on a `<label>` acts on its control.
- Image source: the narrowest `srcset` candidate at least 240 px wide (or the
  lowest density), `<picture>` sources by media and type, formats without a
  decoder (WebP, AVIF, SVG) last, and `data-src`/`data-original`/
  `data-lazy-src` when `src` is missing or a short `data:` placeholder.

**Network and content**

- Requests carry `Referer` (the whole address within a site, the origin across
  sites, nothing from https to http); form POSTs also carry `Origin`.
- A document over its size limit (1 MiB, frames 64 KiB) or the 30000-node
  limit is shown up to there with a line saying it was cut. Inflated bodies
  are bounded by the same limit.
- A response is a page, text shown as written (`text/*`, JSON, XML, scripts),
  a picture, or "This kind of file can't be shown" (binary types, and SVG,
  WebP and AVIF pictures). With no useful type the first bytes decide.
  `br` and `zstd` bodies, which are never asked for, are an error. An error
  status with no body says whether the server failed, refused, or has no such page.
- Text: UTF-8 and Windows-1252 as before, plus Windows-1250, ISO-8859-2,
  Windows-1251, KOI8-R/U, ISO-8859-5, ISO-8859-7, Windows-1253 and
  Windows-1254 when the page names them. The fonts are Latin-1, so Cyrillic
  and Greek are written out in Latin letters. There is no detection of an
  unlabelled legacy encoding, and no CJK, Arabic or Hebrew.
- https: a context that failed to start is retried; an unset clock (before
  2025) gives the date message, and setting the clock is enough to recover.
- Cookies with an expiry persist in `/Browser/cookies`; `Expires` is read in
  every common spelling; a full jar drops the cookie unused longest.

**Using it**

- The address bar takes words: anything with a space, or without a host
  (`example.org`, an IPv4 address, `localhost`, `name:port`, `name/path`), is
  a search with its words encoded.
- `mailto:`, `tel:` and other schemes with nowhere to go leave the page up and
  say so; so does running out of memory while laying a page out.

### Not done, and what each would take

| Gap | Cost or reason |
| --- | --- |
| SVG, WebP, AVIF images | A decoder each, 25-100 KB of code |
| Large JPEGs (over 1.9 megapixels) | A decoder that scales while decoding; stb decodes at full size |
| Missing intermediate certificates (AIA fetching) | An HTTP fetch inside certificate verification |
| Keep-alive connections | Connection pooling in `http.c`/`conn.c`; every request opens its own |
| CSS positioning, floats, real flex and grid | A new layout pass; the largest item here |
| `@import` | Sheets fetched from sheets, within the 24-sheet limit |
| RegExp named groups and lookbehind, `Intl` | JerryScript features, not the facade |
| Trying https first for a typed host | A behaviour change with a fallback delay on a slow link |
| A JavaScript on/off setting | A Settings entry; script-heavy pages can exhaust the 1 MiB heap |
| Detecting an unlabelled legacy encoding | A byte-frequency guess per language |

### Adding a case

Write a small page under `tests/web/compat/` that shows one thing (original
content only), add an entry to `CASES` in `tests/browser_compat.py` with the
lines that must and must not appear, and run
`python3 tests/browser_compat.py out/host/browse <case>` with
`COMPAT_VERBOSE=1` to see the tool's output. `http:` and `https:` pages come
from the driver's own servers, which can also answer with headers, statuses
and bodies no file can give.
