# Headless browser: the DOM<->QuickJS binding layer

This is step three -- the one that actually makes "QuickJS + lexbor" a
browser instead of two separately-working libraries (see `QUICKJS.md`
for the JS engine, `LEXBOR.md` for the HTML/CSS/DOM parser). There is
no upstream to vendor here: this is hand-written glue against QuickJS's
embedding C API (`JS_NewClassID`/`JS_NewClass`/`JS_CGETSET_DEF`) and
lexbor's DOM API, the same way a small C `jsdom` would be built.

## What's bound

- **`console.log(...args)`** -- joins every argument's `JS_ToCString`
  with a space and prints via this port's normal `printf`/
  `posix_shim.c` stdout path. Without this, JS code has no way to
  produce its own output -- `QUICKJS.md`'s example only ever printed a
  C-side `JS_ToCString` of an `JS_Eval` call's *return value*, never
  had JS itself call out. This is the first example where JS output
  reaches the console on its own.
- **`document.querySelector(selector)`** -- parses `selector` with
  lexbor's CSS parser and runs `lxb_selectors_find()` against the
  parsed document's root (same API `LEXBOR.md`'s examples already
  use), keeping only the first match (lexbor's selector-find API has
  no early-stop signal, so every match is still walked, matching how
  every existing example here already handles this -- see the
  `qs_find_cb` comment in `browser.c`). Returns an `Element` wrapper
  object, or JS `null` if nothing matched -- deliberately `null`, not
  `undefined`, matching real DOM `querySelector()` semantics. An
  invalid selector string throws a real `TypeError` rather than
  silently returning `null`.
- **`Element.textContent`** (getter) -- `lxb_dom_node_text_content()`,
  the same descendant-text-concatenation helper a real DOM's
  `textContent` implements.
- **`Element.tagName`** (getter) -- `lxb_dom_element_qualified_name_upper()`,
  matching real DOM `tagName`'s uppercase convention (not
  `lxb_dom_element_qualified_name()`'s as-parsed case).
- **`Element.getAttribute(name)`** -- `lxb_dom_element_get_attribute()`;
  returns `null` when the attribute is absent, same as the real DOM
  method.
- **Inline `<script>` execution** -- after parsing an HTML document,
  every `<script>` element without a `src` attribute is found (again
  via `lxb_selectors_find()`, query `"script"`) in document order, its
  text content extracted via the same `lxb_dom_node_text_content()`
  helper `Element.textContent` uses, and `JS_Eval`'d against a context
  that already has `console`/`document` bound. `<script src="...">`
  elements are skipped outright -- external script fetching is a
  follow-up (see "Not yet done" below), not silently pretended to
  work.
- **Exception reporting** -- a script that throws prints
  `Uncaught exception: <message>` via the same stdout path and
  execution moves on to the next `<script>` tag, matching how a real
  browser keeps loading the rest of the page after one failing script
  block, rather than aborting the whole run.

An `Element` wrapper's opaque pointer is the raw `lxb_dom_node_t*` it
wraps; its finalizer is a no-op because the `lxb_html_document_t`
itself owns every node's lifetime in this single-document, run-to-
completion design -- there is nothing for QuickJS's GC to free when an
`Element` object is collected.

## Explicit non-goals / follow-ups

Kept deliberately small for a first pass -- each of these is a real,
addable follow-up, not a discovered blocker:

- **`getElementById`/`querySelectorAll`** -- `querySelector` (single
  result) covers the binding-layer plumbing; both would reuse the same
  `lxb_selectors_find()` call, just with a different callback
  (collect-all instead of keep-first) or a direct `id` attribute
  lookup instead of a full selector parse.
- **DOM mutation** -- no `createElement`/`appendChild`/`setAttribute`/
  `remove` exposed to JS. lexbor's DOM API supports all of this; none
  of it is wired up yet.
- **`<script src="...">` (external scripts)** -- detected and skipped,
  not fetched. Wiring this in means threading a base URL through for
  relative `src` resolution, which needs lexbor's `url` module (not
  built -- see `LEXBOR.md`'s "What's vendored"), plus routing the
  fetch through the same `libcurl` pattern `examples/lexbor/fetch/
  fetch.c` already established.
- **No event loop, no `setTimeout`/`setInterval`, no `fetch()`/XHR
  exposed to JS.** This is synchronous load-and-run only: parse once,
  run every inline `<script>` once, exit. A real page's `<script>`
  that expects any of `window`, event listeners, or async APIs will
  simply throw a `ReferenceError`/`TypeError` on first use -- expected
  under this scope, not a bug to chase.
- **No CSS cascade/computed style/layout, ever** -- this stays a
  DOM+JS headless browser, not a pixel-rendering one (see `QUICKJS.md`/
  `LEXBOR.md`'s framing).

## Boot-testing note: MEMSIZE

Same story as `QUICKJS.md`/`LEXBOR.md`/`PYTHON.md`: needed
`BareMetal-Firecracker`'s `baremetal.sh` `MEMSIZE` bumped from its
4MiB default (32MiB was used here) to boot at all -- both engines'
compiled code/tables plus this example's own DOM live in the same flat
binary. Bumped, boot-tested, then reverted -- not a permanent change to
that repo.

## Example

`examples/lexbor/browser/browser.c` parses a small static HTML string
(no network -- kept hermetic, see `LEXBOR.md`'s `fetch.c` for the
curl+lexbor pipeline this would combine with for a real fetched page)
containing four `<script>` tags, each exercising one thing:

1. `null.foo;` -- an intentional `TypeError`, proving a throwing
   script doesn't abort the rest of the page.
2. `console.log("DOM says: " + document.querySelector("#msg").textContent)`
   -- the actual point of this phase: JS code reading the DOM and
   printing on its own, not a C-side print of an eval's return value.
3. `.tagName`/`.getAttribute("class")` on the same element.
4. `document.querySelector("#nope")` -- proves a non-matching selector
   resolves to JS `null` rather than throwing or crashing.

Verified booting for real through the actual `build-app.sh` /
`BareMetal-Firecracker` pipeline, first attempt:

```
Uncaught exception: TypeError: cannot read property 'foo' of null
DOM says: Hello
tagName=P class=greeting
missing is null
```

## Live fetch + live script execution: `examples/lexbor/browser-fetch/browser_fetch.c`

The stretch goal flagged above -- actually tried, not just reasoned
about. Combines `fetch.c`'s curl-fetch (same CA-bundle/`write_cb`
buffer handling, same argv[1]-URL-with-fallback pattern) with this
file's binding layer (`console.log`/`document.querySelector`/inline-
`<script>` execution) into one new example. Neither `fetch.c` nor
`browser.c` was modified -- this is new glue combining both, reusing
their logic verbatim rather than changing either's proven behavior.

Boot-tested for real (`tap0` up, real DHCP, real TLS) against four
different live URLs, on purpose picked to see what actually happens
against the real web rather than a single cherry-picked success:

- **`https://example.com/`** (the default, no arg needed) -- no inline
  `<script>` at all, so nothing runs; only `title: Example Domain`
  prints. Confirms the pipeline does nothing observable (correctly)
  when a page has no script content, rather than erroring.
- **`https://httpbin.org/`** -- three `<script src=...>` tags correctly
  skipped (printed as such), then the one inline `<script>` (Swagger
  UI's bootstrap) throws `ReferenceError: window is not defined` on
  its first statement. Real, expected, matches the "no `window`
  object" non-goal above exactly.
- **`https://www.iana.org/domains/reserved`** -- same shape, but a
  *different* real failure: the inline `<script>` throws
  `ReferenceError: $ is not defined` -- it assumes jQuery, which was
  loaded via a skipped `<script src>`, so the global it expects was
  never defined. A different missing-global reason than the `window`
  case, both equally expected under this scope.
- **`https://www.wikipedia.org/`** -- originally found the fixed
  32KB `RESPONSE_BUF_SIZE` cap silently truncating this 119KB page
  mid-document, producing a misleading `TypeError` that was really an
  artifact of the truncation, not a real page failure. Fixed (see
  "Growable fetch buffer" below): both `fetch.c` and this example now
  fetch the full `body: 119573 byte(s)` and correctly parse `<title>:
  Wikipedia` / count 383 `<a>` tags. Re-run against the untruncated
  document, its scripts now throw *real* errors instead --
  `TypeError: cannot read property 'className' of undefined` and
  `ReferenceError: window is not defined` -- the same "no `window`
  object" non-goal every other real page hit, not a buffer artifact.

### Growable fetch buffer

`fetch.c` and `browser_fetch.c` originally shared a fixed
`static char response_buf[32 * 1024]` -- found by the Wikipedia case
above to silently truncate anything bigger, with no error, handing
lexbor a cut-off document. Both were changed to a `realloc`-doubling
`struct growable_buf { char *data; size_t len; size_t cap; }` passed
through `CURLOPT_WRITEDATA` instead of a file-scope global, starting
at 16KB and doubling as needed. `write_cb()` now follows the standard
libcurl growable-buffer contract: returning anything other than the
full byte count on a failed `realloc` tells libcurl to abort the
transfer (`CURLE_WRITE_ERROR`) rather than silently handing back a
truncated body the way the fixed-cap version did. Boot-tested against
Wikipedia's full 119KB body (above) and re-verified `example.com`'s
default case still prints identically (`body: 559 byte(s)`, `title:
Example Domain`) as a regression check.

## Honest assessment: how close is this to "a minimal headless browser"?

Close, for toy/simple pages: fetch (curl), parse (lexbor), and run
scripts against a real DOM with real output -- the full pipeline exists
and works end to end for a page whose scripts only touch
`document.querySelector`/`textContent`/`tagName`/`getAttribute` and
plain JS, live network fetch included, verified above with
`browser_fetch.c`.

Far, for anything resembling a real-world page -- now confirmed against
four actual live sites, not just reasoned about: every one of them
failed, each for a *different* concrete reason (no `window`, missing
`$` because its defining external script was correctly not fetched,
and a fixed-size fetch buffer truncating a larger real page mid-
document). No event handling (`DOMContentLoaded`, click handlers --
there's no event loop to dispatch them from), no `fetch`/XHR so a page
can't make its own follow-up requests, no external `<script src>`
(confirmed above to be exactly where most real sites' actual logic
lives, not inline), no `getElementById`/`querySelectorAll`/DOM
mutation, and a fixed 32KB fetch buffer that silently truncates
anything larger. Each failure mode is a distinct, addable follow-up
(external script fetching, a real `window` stub, a growable fetch
buffer) rather than one big blocker -- but real-world pages currently
fail for real, varied, and now-documented reasons rather than
hypothetical ones.
