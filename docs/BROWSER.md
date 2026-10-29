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
- `document.cookie` shares the session HTTP cookie jar, with path, Secure,
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
  `_self`, `_parent` and `_top`. `_blank` opens in the current top-level view.
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

Scripts run after HTML parsing; `async` and `defer` currently use the same
ordered execution model. `document.write` inserts after the executing script,
or appends to the body outside script loading. Scripts inserted through
`innerHTML` stay inert. Node lists are snapshots. The DOM and CSS interfaces
are a subset; layout remains Glaze's integer layout engine. There is no
XHR, canvas scripting, WebAssembly, IndexedDB or full browser history
API. Fetch and origin-scoped storage are bounded subsets described above. Child navigation does not create top-level history entries. Unicode
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
shared 3 MiB decoded-image budget. Child documents are capped at 64 KiB;
external scripts at 128 KiB. DOM edits are bounded by the node/depth limits,
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
currently occupies about 96% of its slot; its slot is separate from user file storage. Build provenance
and local vendor changes are recorded in `third_party/*/UPSTREAM.md`.

`make -C host browser-test` runs ASan/UBSan integration tests against a local
HTTP fixture server: DOM/events/timers, script order, errors and execution
budgets, allocation failure/restart, frame navigation, origin isolation,
sandboxing, CSP/XFO, framesets and embedding limits, plus the Web API, CORS,
storage persistence and module tests described in the companion document.

`make -C host browser-google` builds an opt-in live workflow check. Run
`(cd host && ../out/host/browser-google)` to open Google, submit the default
`example domain` query, locate the example.com result and open it. Optional
arguments select the query and expected result hostname. Exit code 3 means
Google requires human verification; it does not count as a compatibility pass.
The live check is separate from the deterministic integration tests.

`make INPUT_SCRIPT=tests/t_browser_script.txt` builds the offline device smoke
test. Run a copy of the NAND under be300 for 36 seconds and check its console
for `browser demo:` messages, including math/JSON, click, timer, child script
and same-origin frame results. The test also opens `res:script-limits.html`
to exercise MIPS parser jumps, an infinite loop, heap exhaustion and fresh-VM
recovery on navigation. Rebuild without INPUT_SCRIPT for normal use.
