# Bounded browser Web APIs

Glaze's JerryScript browser includes asynchronous fetch, URL and UTF-8 utilities,
small origin-scoped storage, additional DOM/event/layout interfaces, and ES
modules. These APIs target small applications on a 16 MiB BE-300; they remain a
subset of the web platform. The VM still shares a 1 MiB heap across documents.
See [BROWSER.md](BROWSER.md) for the underlying browser and execution limits.

Open **Web APIs** on the welcome page (`res:apis.html`) for an offline example of
text conversion, URLs, fragments, cloning, layout, animation, and modules.

## Requests

```js
const controller = new AbortController();
fetch('/api/status', {signal: controller.signal})
  .then(response => {
    if (!response.ok) throw Error('HTTP ' + response.status);
    return response.json();
  })
  .then(status => {
    document.getElementById('status').textContent = status.message;
  })
  .catch(error => console.error(error));
// controller.abort();
```

- `fetch(url, options)` accepts URL strings or URL objects, resolves relative to
  the document base, and returns a promise. It uses the existing nonblocking
  HTTP/HTTPS loader; no socket operation blocks the JavaScript VM.
- GET and POST are supported. POST bodies are strings or `URLSearchParams`, at
  most 16 KiB encoded as UTF-8; embedded NULs and binary request bodies are
  rejected. A form body defaults to `application/x-www-form-urlencoded`, and a
  string to `text/plain;charset=UTF-8`.
- `Content-Type` is the only supported explicit request header. Other explicit
  headers and unsupported methods/options such as `mode: 'no-cors'` or a
  redirect mode other than `follow` reject the request. The HTTP client still
  supplies its normal browser request headers.
- HTTP errors resolve with a response: check `ok` or `status`. Network failures,
  policy failures, limits, and unsupported requests reject with `TypeError`.
- Responses expose `status`, `ok`, `url`, `redirected`, `type`, `headers`,
  `bodyUsed`, `text()`, `json()`, `arrayBuffer()`, and `clone()`. Bodies are
  buffered and capped at 64 KiB, including after decompression. Reading consumes
  a body once; clone it before reading if two consumers need it. Only the
  response's Content-Type header is exposed in this version. `Headers` supports
  append/set/get/has/delete, iteration, and `forEach`.
- Eight requests may be outstanding per document, including completed responses
  awaiting JavaScript delivery. Each request has a 30-second total lifetime.
  Native connection and loader limits are shared with other page resources.
- `AbortController`, `AbortSignal`, and `EventTarget` support cancellation and
  abort listeners. An already-aborted signal rejects immediately. Retiring a
  document or discarding the VM cancels its native requests and frees queued
  bodies; it does not deliver callbacks into the replacement document.

### Origins and policy

Fetch requires a non-opaque HTTP(S) document origin and an HTTP(S) destination.
HTTPS documents cannot fetch HTTP resources. Local and bundled pages cannot use
fetch to read private files or bypass origin checks.

The default mode is `cors`; `same-origin` rejects foreign destinations.
Cross-origin responses require one valid `Access-Control-Allow-Origin` matching
this document's canonical origin, or `*` for uncredentialed requests. Duplicate
allow-origin headers fail closed. `credentials` supports `omit`, `same-origin`
(the default), and `include`. Cross-origin `include` also requires an exact
allow-origin and one `Access-Control-Allow-Credentials: true` header; wildcard
origins are insufficient. Cookie handling follows the selected credentials
mode, and API responses bypass the ordinary resource cache.

CORS preflight is not implemented. Cross-origin POSTs with a non-safelisted
Content-Type, including JSON POSTs, are rejected before transmission. This
version accepts the exact cross-origin types `text/plain`,
`text/plain;charset=UTF-8`, and `application/x-www-form-urlencoded`. Same-origin
JSON POSTs work. Use a same-origin application endpoint when preflight would
otherwise be required. Same-origin redirects are followed within the loader's
existing limit; origin-changing redirects are rejected before the next request.

CSP `connect-src`, falling back to `default-src`, is enforced using the browser's
existing conservative source-expression subset. Unsupported policy expressions
grant no permission. This does not make the browser a complete CSP/CORS or
Fetch-standard implementation.

## URL and text utilities

`URL` provides HTTP(S) URL resolution, canonical host/default-port handling,
component getters/setters, `origin`, a live `searchParams`, string/JSON conversion,
and `canParse`. It uses Glaze's existing RFC 3986 URL parser rather than a full
WHATWG URL implementation; credentials, IDNA conversion, and arbitrary URL
schemes are not provided by this interface. Existing parser component limits
still apply.

`URLSearchParams` accepts strings, records, and iterable pairs. It supports
append/set/get/getAll/has/delete/sort, size, iteration, `forEach`, form encoding,
duplicate keys, and replacement decoding of invalid UTF-8. There are at most
128 pairs, with a 4,096-character limit on each appended key/value.

`TextEncoder` encodes UTF-8, replacing unpaired surrogates. `TextDecoder` decodes
UTF-8 buffers/views with `fatal`, `ignoreBOM`, and streaming support. Inputs and
encoded output are bounded at 64 KiB. Other encodings and `encodeInto` are not
implemented. Network JSON/text stays Unicode in JavaScript; rendering still
uses Glaze's existing font and charset mappings.

## Storage

```js
localStorage.setItem('theme', 'blue');
const theme = localStorage.getItem('theme') || 'default';
sessionStorage.setItem('lastQuery', 'Glaze');
```

Both stores support length, key/getItem/setItem/removeItem/clear and named
property access/enumeration. They are keyed by canonical HTTP(S) origin,
including scheme and effective port. Same-origin frames share the stores;
foreign, opaque, file, and bundled resource origins do not gain access.

Each store holds at most eight origins, 64 keys per origin, and 16 KiB of
serialized UTF-8 JSON per origin, including serialization overhead. Exceeding a
quota throws `QuotaExceededError` without changing the previous value. Clearing
an origin releases its slot. Storage read/write failures throw `UnknownError`.
Storage events and IndexedDB are not implemented.

`localStorage` uses numbered `/Browser/storageN` files. Each record includes its
exact origin, which is checked when read. Writes use a temporary file followed
by rename; a failed write does not replace the previous record. `sessionStorage`
is held in the browser process, survives page navigation, and ends when that
process closes. It is not durable across browser restarts.

Native previews write no storage by default. Set `BROWSE_STORAGE` to an existing
directory to enable persistent records. Tests use an isolated temporary directory
and separate browser processes to check persistence.

## DOM, events, and layout

- `document.createDocumentFragment()` creates a node of type 11. Inserting it
  moves its children in order and empties the fragment.
- `cloneNode(deep)` copies attributes and optionally descendants into the same
  document. Listener registrations are not copied. The existing node/depth/arena
  limits apply; processed script clones remain inert.
- Event listeners support capture, duplicate suppression, once, passive, and
  AbortSignal removal. Events traverse capture, target, and bubble phases and
  expose `eventPhase`, `currentTarget`, `timeStamp`, `composedPath`,
  `stopImmediatePropagation`, and the existing cancellation methods. Listener
  exceptions are logged while other listeners continue.
- `getBoundingClientRect()` synchronously updates layout when needed and returns
  CSS-pixel viewport coordinates. Block boxes use border-box geometry; inline
  geometry is the union of rendered descendant items. Detached/unrendered nodes
  return a zero rectangle. Coordinates account for the document's own scroll,
  including independent child-frame scrolling.
- `getComputedStyle()` returns a frozen snapshot for a styled element. Its subset
  contains color, backgroundColor, fontSize, fontWeight, display, visibility,
  width, height, and `getPropertyValue`. It is not a complete/live CSS declaration;
  the underlying integer layout and supported CSS rules are unchanged.
- `requestAnimationFrame` and `cancelAnimationFrame` use the browser's roughly
  16 ms script tick, with up to 32 queued callbacks per document. A batch shares
  a performance timestamp; callbacks scheduled by that batch wait for a later
  tick. This is not a hardware-vsync or frame-rate guarantee.

## ES modules

Inline and external `<script type="module">` support static imports, exports,
cyclic dependency linking, and per-document module caching. `import()` returns a
promise and `import.meta.url` reports the source URL. Relative references resolve
against the importing source, including its final URL after redirects. Classic
external scripts also retain their own source URL for dynamic imports.

Dependencies are fetched asynchronously before JerryScript links the graph;
its synchronous resolver never opens files or sockets. HTTP(S) modules require
CORS checks, successful status codes, and a JavaScript MIME type. Script CSP
applies to roots and dependencies, carrying the initiating script's nonce.
Network content cannot import file/res resources. Trusted local pages can import
local JavaScript resources through the existing loader.

The module map has 32 entries and a cumulative 256 KiB source budget per document;
each external source remains capped at 128 KiB. At most eight dynamic imports can
await completion per document. Repeated imports reuse the evaluated module.
Graphs, pending imports, and fetches are released when their document retires,
including after VM exhaustion.

Module scripts use Glaze's existing ordered post-parse script scheduling;
`async`/`defer` semantics are not a full browser implementation. Bare package
names, import maps, JSON modules/import attributes, and top-level await are not
supported. Use relative/absolute JavaScript URLs and small module graphs.

## Validation

`make -C host browser-test` runs the existing ASan/UBSan browser regressions plus
observable API tests, request/CORS/redirect/abort/size-limit fixtures, persistent
storage across processes, module cycles/cache/dynamic imports, nonce policy,
redirect resolution, MIME rejection, and graph limits. Test pages live under
`tests/web/`; `tests/browser_transport.py` serves them locally.

`make INPUT_SCRIPT=tests/t_browser_apis.txt` builds the offline MIPS smoke test.
Boot a copy of the NAND for 35 seconds and check for all five `browser APIs:`
PASS messages. `tests/web/browser_device.js`, served at `/device` by the HTTP
fixture, additionally exercises real guest lwIP, JSON GET/POST, flash storage,
and network module imports. Rebuild without INPUT_SCRIPT after testing.

Physical BE-300 latency, stylus behavior, and live-site compatibility still need
hardware testing; deterministic fixtures do not establish broad site support.
