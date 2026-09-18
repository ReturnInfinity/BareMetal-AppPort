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
- **`window`** -- an alias for the global object itself
  (`JS_SetPropertyStr(ctx, global, "window", JS_DupValue(ctx, global))`),
  matching a real browser's `window === globalThis`, not a separate
  object with its own copies of every global. `window.document`/
  `window.console` and `typeof window !== 'undefined'` work for free
  this way, and `window.foo = ...` assignments succeed as plain
  property sets -- there's still no event loop to ever act on them.
  See "Window stub" below for what boot-testing this against real
  pages actually found.

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
- **`<script src="...">` (external scripts)** -- now fetched and run in
  `browser_fetch.c` (lexbor's `url` module resolves `src` against the
  page's own URL, `fetch_url()` GETs it, the result runs the same way
  an inline script does). **But this triggers a reproducible VM crash
  on every real page tried that actually has one** -- see "External
  script fetching" below. Left in place, not reverted, because the
  crash is a real, characterized finding in its own right, not a
  reason to hide the code that found it.
- **No event loop, no `setTimeout`/`setInterval`, no `fetch()`/XHR
  exposed to JS, and `window` has no methods/properties of its own
  beyond what's a plain global** (`window.document` works because
  `document` is global; `window.addEventListener` does not exist and
  throws `TypeError: not a function` if called). This is synchronous
  load-and-run only: parse once, run every inline `<script>` once,
  exit. A real page's `<script>` that expects event listeners, async
  APIs, or DOM properties beyond `querySelector`/`textContent`/
  `tagName`/`getAttribute` (e.g. `document.documentElement`) will
  throw a `ReferenceError`/`TypeError` on first use -- expected under
  this scope, not a bug to chase.
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
  skipped (printed as such); the one inline `<script>` (Swagger UI's
  bootstrap) originally threw `ReferenceError: window is not defined`
  on its first statement. After the window stub (see "Window stub"
  below), this script now runs to completion with **no exception at
  all** -- whatever it does with `window` didn't need anything beyond
  a plain object.
- **`https://www.iana.org/domains/reserved`** -- same shape, but a
  *different* real failure: the inline `<script>` throws
  `ReferenceError: $ is not defined` -- it assumes jQuery, which was
  loaded via a skipped `<script src>`, so the global it expects was
  never defined. Unaffected by the window stub (re-verified) -- this
  is a missing-external-script problem, not a missing-`window`
  problem.
- **`https://www.wikipedia.org/`** -- originally found the fixed
  32KB `RESPONSE_BUF_SIZE` cap silently truncating this 119KB page
  mid-document, producing a misleading `TypeError` that was really an
  artifact of the truncation, not a real page failure. Fixed (see
  "Growable fetch buffer" below): both `fetch.c` and this example now
  fetch the full `body: 119573 byte(s)` and correctly parse `<title>:
  Wikipedia` / count 383 `<a>` tags. Re-run against the untruncated
  document (before the window stub), its scripts threw *real* errors
  instead -- `TypeError: cannot read property 'className' of
  undefined` and `ReferenceError: window is not defined`. After the
  window stub (see below), the `window` error is gone, replaced by a
  *new* distinct one -- `TypeError: not a function` -- alongside the
  same `className` error, both now isolated to real, separate DOM
  gaps rather than a missing global.

### Window stub

Added `window` as an alias for the global object (see "What's bound"
above) and re-ran all three real pages that had thrown against it:

- **`httpbin.org`** -- the `window is not defined` error is gone, and
  the script now runs with **no exception at all**.
- **`www.wikipedia.org`** -- the `window is not defined` error is
  gone, replaced by two *different* real errors: the pre-existing
  `TypeError: cannot read property 'className' of undefined` (almost
  certainly `document.documentElement.className` -- `documentElement`
  isn't implemented, a separate DOM gap from `window`) and a new
  `TypeError: not a function` (a later statement calling some
  `window.<method>` that doesn't exist -- `window` is just a plain
  object alias, it has none of a real `Window` interface's methods
  like `addEventListener`). Progress, not a full fix: the page still
  fails, but now for its *next* real, distinct gap instead of the
  first one.
- **`www.iana.org/domains/reserved`** -- unchanged, still
  `ReferenceError: $ is not defined`, confirming the window stub has
  no effect on the separate missing-external-script problem.
- Regression check: the static `examples/lexbor/browser/browser.c`
  example's boot output is byte-for-byte unchanged.

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

### External script fetching

The last of the three follow-ups above -- actually wired up, and it
found a real crash, not just another missing-API `ReferenceError`.

**What got built:** lexbor's `url` module (plus its real dependency
graph, `encoding`/`unicode`/`punycode` -- see `LEXBOR.md`) is now
vendored. `browser_fetch.c` no longer skips `<script src="...">`:
it resolves `src` against the page's own final, post-redirect URL
(`CURLINFO_EFFECTIVE_URL`, captured by a new `fetch_url()` helper
factored out of the page-fetch/script-fetch duplication that would
otherwise exist) using `lxb_url_parse()` -- which transparently
handles both relative (`/static/foo.js`) and already-absolute
(`https://ajax.googleapis.com/...`) `src` values, the same WHATWG
algorithm every real browser uses, so no separate cases were needed --
then fetches the resolved URL with the same `growable_buf`/`write_cb`
pattern the page fetch uses, and runs the result the same way an
inline `<script>` runs, in document order, naming the resolved URL as
the `JS_Eval` filename so exceptions point at the real source. A
failed fetch/resolve is reported and skipped, same "don't abort the
rest of the page" policy a throwing script already has.

One real bug was found and fixed along the way, independent of the
crash below: `main()` originally called `curl_global_cleanup()` right
after the page fetch, but `run_scripts()` (called afterward) now calls
`fetch_url()` again for every external script -- calling
`curl_easy_init()` after `curl_global_cleanup()` without a fresh
`curl_global_init()` is undefined behavior per libcurl's own contract.
Fixed by moving `curl_global_cleanup()` to bracket the whole of
`main()` instead of just the first fetch. This is a real, permanent
fix, kept regardless of the crash below.

**The crash, found boot-testing against three real pages:**

- `https://www.iana.org/domains/reserved` -- jQuery (`src="/static/js/
  jquery.a8e7cabd4d49.js"`, correctly resolved to an absolute URL)
  fetches (78748 bytes, HTTP 200) and runs; it throws a real
  `TypeError: cannot read property 'matches' of undefined` (a further,
  more specific DOM-gap symptom of the same "no `documentElement`"-
  class limitation `BROWSER.md` already tracks). Then, deterministically,
  every time: `CPU 0x00000000 - Exception 0x13(GP)`, VM halted, no
  further output.
- `https://httpbin.org/` -- Swagger UI's real bundle (`flasgger_static/
  swagger-ui-bundle.js`, 1,428,809 bytes -- 18x larger, and it actually
  finished fetching and running) throws a different exception
  (`TypeError: not a function`, expected -- it exercises far more than
  this scope's DOM surface) and then hits the **exact same** `Exception
  0x13(GP)` crash.
- `https://www.wikipedia.org/` -- has an external script after all
  (`portal/wikipedia.org/assets/js/index-7ecb9c7f8e.js`, missed by
  earlier phases because the old skip-and-print code never printed a
  URL to notice it by); it fetches and runs (throwing `ReferenceError:
  Element is not defined`, another real DOM gap), and then the same
  `Exception 0x13(GP)` crash follows.

**Why this looks structural, not data-dependent:** across all three
runs -- 78748 bytes vs. 1,428,809 bytes vs. Wikipedia's script,
completely different content, completely different JS exceptions --
`RSP` in the fault dump is **bit-for-bit identical every single time**
(`00000000001CFF90`), and `RIP` lands within 0x40 bytes of the same two
values across all runs. Ordinary heap corruption from a bad `realloc`/
`free` produces increasingly *varied* garbage as payload size and
content change; a fixed, repeatable `RSP` and near-identical `RIP`
regardless of what was fetched or evaluated points at something
structural in what's actually new here -- performing a *second* real
`curl_easy_init()` → DNS → TCP → TLS → HTTP round trip within the same
process, after the first one already completed, while a QuickJS
runtime/lexbor DOM from the first fetch are still live. Every single-
fetch example in this project (`fetch.c`, `browser.c`, and
`browser_fetch.c` against pages with no external scripts) has always
worked; this is the first code path to ever do two.

**Deliberately not chased further here.** Diagnosing this fully would
mean instrumenting or stepping through this port's syscall/interrupt
boundary (`posix_shim.c`'s `sys_mmap`/`sys_brk`/network syscalls, or
possibly `BareMetal-Firecracker`'s own interrupt handling under this
specific "two full network round trips interleaved with heavy engine
activity in one process" load shape, which nothing in this project has
exercised before) -- real, valuable follow-up work, but a different
scope than this task, and explicitly not something to touch
kernel-side without a separate, deliberate investigation. The code
implementing external-script fetching is kept as-is (not reverted or
stubbed back to skip-and-print) because it is correct as written and
the crash is a genuine, now well-characterized finding about this
port's current limits, not a defect in this feature's own logic.
Regression-verified: `example.com` (no scripts at all) and the static
`examples/lexbor/browser/browser.c` test are both unaffected --
the crash only reproduces on the code path that performs a second
real network fetch.

## Honest assessment: how close is this to "a minimal headless browser"?

Close, for toy/simple pages: fetch (curl), parse (lexbor), and run
scripts against a real DOM with real output -- the full pipeline exists
and works end to end for a page whose scripts only touch
`document.querySelector`/`textContent`/`tagName`/`getAttribute` and
plain JS, live network fetch included, verified above with
`browser_fetch.c`.

Far, for anything resembling a real-world page -- now confirmed against
four actual live sites, iterated on three times (a growable fetch
buffer, a `window` stub, and external script fetching). One of the
four (`httpbin.org`'s *inline* script) still runs clean with zero
exceptions. But the picture changed with this round: wiring up
external scripts is real, working, WHATWG-correct URL resolution and
fetch logic -- verified fetching and running two genuinely large real
scripts (jQuery, and Swagger UI's 1.4MB bundle) -- and it surfaced a
**new class of finding**, a reproducible VM crash, not just another
missing-API exception. Every one of the three pages with an external
script (`iana.org`, `httpbin.org`, `wikipedia.org`) now fetches and
runs that script successfully, throws an honest, expected JS-level
error from it (a further DOM/API gap, same category as before), and
*then* crashes the VM outright the first time a second real network
fetch happens in one process -- a limit this project had never
exercised before external scripts existed to trigger it.

So the assessment splits in two: for the DOM+JS binding layer itself,
the trend from earlier rounds holds -- `$ is not defined` is
unaffected by any of the three fixes (a missing-external-script
problem, now finally addressed at the URL/fetch level, though masked
by the crash before the page's own inline script gets to prove it);
`wikipedia.org` and `iana.org` both progressed to new, more specific
DOM-gap exceptions once their real external scripts actually ran,
exactly the "failing later, for narrower reasons" pattern every prior
round showed. But there's now a harder floor underneath all of that:
**no real page with an external script can currently finish running
in this project without crashing the VM**, which is a more fundamental
limit than any single missing DOM property or `Window` method. No
event handling, no `fetch`/XHR from JS, no `getElementById`/
`querySelectorAll`/DOM mutation, and `window` is a bare alias with none
of a real `Window` interface's methods remain the other honest gaps.
Each of those is still a distinct, addable follow-up; the network-fetch
crash is the one item on this list that isn't yet characterized well
enough to call addable -- it needs real investigation before it can be
called a follow-up rather than an open question.
