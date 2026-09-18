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
- **`Element.className`** (getter) -- also `lxb_dom_element_get_attribute()`
  for `"class"`, but returns `""` (not `null`) when the attribute is
  absent, matching real DOM `className` semantics -- unlike
  `getAttribute("class")`, a page doing `el.className.split(' ')` on a
  perfectly ordinary class-less element won't crash.
- **`document.querySelectorAll(selector)`** and
  **`Element.querySelectorAll(selector)`** -- share `querySelector`'s
  exact parse/init scaffolding (`query_selector_all()`), just with a
  collect-all callback (`qsa_find_cb`) instead of keep-first, and
  return a real JS `Array` (`JS_NewArray()` + `JS_SetPropertyUint32()`
  per match) -- verified with real `JS_Eval`'d test code that `.length`
  and numeric indexing both work correctly, not assumed. The
  element-scoped version passes that element itself as the search
  root: `lxb_selectors_find()` already excludes the root node from
  matching by default (confirmed by reading `lxb_selectors_tree()`'s
  `LXB_SELECTORS_OPT_MATCH_ROOT` check in lexbor's own source -- that
  option is never set here), which happens to be exactly real
  `querySelectorAll()`'s "descendants only, never the element itself"
  semantics, with no extra exclusion logic needed.
- **`document.getElementById(id)`** -- deliberately a direct
  depth-first tree walk (`find_by_id_recursive()`, matching by
  `id`-attribute string equality), not a `"#" + id` CSS-selector query.
  A bare `#id` selector can't represent every string a real `id`
  attribute can legally hold (a leading digit, a space, `.`/`:`, ...)
  without escaping this scope doesn't implement -- the tree walk
  matches real `getElementById()`'s exact-string-equality semantics
  with no selector-syntax edge cases to worry about at all.
- **`document.documentElement`/`.body`/`.head`** (getters) -- direct
  lexbor accessors (`lxb_dom_document_element()`,
  `lxb_html_document_body_element()`, `lxb_html_document_head_element()`),
  not a `querySelector("html"/"body"/"head")` CSS-selector query --
  lexbor already tracks these three as plain struct fields/inline
  accessors. Each resolves to JS `null` if the document is malformed
  enough not to have one, same convention `querySelector()` uses for
  "no match".
- **`document.createElement(tagName)`**, **`Element.appendChild(child)`**,
  **`Element.setAttribute(name, value)`**, **`Element.remove()`** -- real
  DOM mutation, not read-only accessors. See "DOM mutation" below for
  the full writeup, including the element-lifetime question this
  raised and how it was verified.
- **`window.addEventListener`/`removeEventListener`** -- honest no-op
  stubs (bound on the global object, so both `window.addEventListener`
  and a bare `addEventListener` work, matching how real pages use
  either form). A call is accepted and its listener silently never
  invoked -- there is genuinely no `DOMContentLoaded`/`load`/click
  event ever fired (no event loop, see non-goals below), so this isn't
  a faked event system, just a method that no longer throws
  `TypeError: not a function` purely because it didn't exist.
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

- **`getElementById`/`querySelectorAll`** -- now bound (see "What's
  bound" above): `document.querySelectorAll`/`Element.querySelectorAll`
  reuse `querySelector`'s exact scaffolding with a collect-all callback
  returning a real JS `Array`; `getElementById` is a direct tree walk
  rather than a CSS-selector query, to match exact id-string-equality
  semantics without selector-escaping edge cases.
- **DOM mutation** -- `document.createElement`/`Element.appendChild`/
  `.setAttribute`/`.remove` are now bound (see "DOM mutation" below).
  `Node.removeChild(child)` (the older, two-party form that must throw
  a real DOM exception for "child isn't actually a child of this
  node") is deliberately still not bound -- it needs real
  DOM-exception-code translation, not just a straight lexbor call the
  way the other four were.
- **`<script src="...">` (external scripts)** -- now fetched and run in
  `browser_fetch.c` (lexbor's `url` module resolves `src` against the
  page's own URL, `fetch_url()` GETs it, the result runs the same way
  an inline script does). **But this triggers a reproducible VM crash
  on every real page tried that actually has one** -- see "External
  script fetching" below. Left in place, not reverted, because the
  crash is a real, characterized finding in its own right, not a
  reason to hide the code that found it.
- **No event loop, no `setTimeout`/`setInterval`, no `fetch()`/XHR
  exposed to JS.** This is synchronous load-and-run only: parse once,
  run every inline/external `<script>` once, in document order, exit.
  `addEventListener` no longer throws (see "What's bound" above), but
  a registered listener is never called -- there is no
  `DOMContentLoaded`/`load`/click event to ever fire it.
- **`Node.removeChild` still not bound** (see above), and `window` has
  no method beyond the two event-listener stubs and `navigator` doesn't
  exist at all -- no `window.matchMedia`/`requestAnimationFrame`/etc. A
  real page's `<script>` that expects any of these, or DOM properties
  beyond `querySelector`/`querySelectorAll`/`getElementById`/
  `textContent`/`tagName`/`getAttribute`/`className`/`documentElement`/
  `body`/`head`/`createElement`/`appendChild`/`setAttribute`/`remove`,
  will throw a `ReferenceError`/`TypeError` on first use -- expected
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
curl+lexbor pipeline this would combine with for a real fetched page),
now up to seven `<script>` tags as later rounds added bindings to
exercise:

1. `null.foo;` -- an intentional `TypeError`, proving a throwing
   script doesn't abort the rest of the page.
2. `console.log("DOM says: " + document.querySelector("#msg").textContent)`
   -- the actual point of this phase: JS code reading the DOM and
   printing on its own, not a C-side print of an eval's return value.
3. `.tagName`/`.getAttribute("class")` on the same element.
4. `document.querySelector("#nope")` -- proves a non-matching selector
   resolves to JS `null` rather than throwing or crashing.
5. `typeof Element`/`document.body instanceof Element` -- the isolated
   check for the global `Element` constructor (see "Global `Element`
   constructor" below).
6. The full DOM-mutation round-trip: `createElement`, `setAttribute`,
   `appendChild`, then a fresh `querySelector()` proving the new
   element is genuinely in the tree.
7. `getElementById` (hit against a second static `<p id="msg2">`, and a
   miss) and `querySelectorAll("p")` (`.length` plus per-index
   `.tagName` against both `<p>` tags, and a zero-match selector
   proving a real empty array comes back, not `null`/`undefined`).

Verified booting for real through the actual `build-app.sh` /
`BareMetal-Firecracker` pipeline:

```
Uncaught exception: TypeError: cannot read property 'foo' of null
DOM says: Hello
tagName=P class=greeting
missing is null
typeof Element=function
body instanceof Element=true
created tagName=DIV
byId msg2 text=World
byId miss=null
qsa p length=2 tagNames=P,P
qsa none length=0
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

**The crash, found boot-testing against three real pages, root-caused
and fixed (see "The crash, root-caused" below) -- kept here as the
original discovery narrative, not a still-open problem:**

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

**The crash, root-caused.** The "identical RSP, near-identical RIP
across wildly different payloads" observation above turned out to
have two separate, unrelated explanations, only one of which was a red
herring:

- `RSP` being bit-for-bit identical (`00000000001CFF90`) every time
  really is just a debug-dump artifact -- `BareMetal-Firecracker`'s
  `exception_gate_main` (`src/BareMetal/interrupt.asm`) prints its
  *own* current stack pointer partway through pushing 16 registers for
  display, not the app's real stack pointer at the moment of the
  fault. It's always the same value because it's always captured at
  the same fixed depth into the kernel's own fixed-size crash-dump
  stack frame, regardless of what the app was doing.
- `RIP` clustering near the same address, though, was **not** a
  debug-dump artifact -- it was the dump correctly reporting that the
  crash is *always in the same instruction*, because it is: every
  crash was `lexbor_mraw_alloc` (lexbor's own memory-arena allocator)
  dereferencing a corrupted `mraw->mem` pointer, confirmed by
  disassembling the actual faulting address (`objdump -d` on the
  unstripped intermediate `build/*.app.elf` `build-app.sh` produces
  before flattening to a raw binary) against the exact fault RIP from
  the boot log.

That pointed at lexbor's `url` module, not networking, threading, or
this port's syscall layer at all -- confirmed by a minimal, hermetic
repro (`BareMetal-AppPort/urltest2.c`, no curl, no QuickJS, no real
network fetch) that resolves the same three relative URLs
`resolve_script_url()` resolves for `iana.org`, in sequence, against a
hardcoded base URL. It reproduced the identical crash on exactly the
*second* resolution, every time -- proving the "second real network
fetch" theory wrong: it was never about doing two fetches, or about
QuickJS/lexbor coexisting under memory pressure. It was about calling
`lxb_url_parse()` twice on the same reused `lxb_url_parser_t`.

**Root cause:** `resolve_script_url()` called `lxb_url_memory_destroy()`
on each resolved URL to free it -- but that function calls
`lexbor_mraw_destroy()`, which tears down lexbor's *entire* memory
arena (every chunk, and the arena object itself), not just the one
URL's own allocation. `g_url_parser` (and `g_base_url`, allocated from
the same arena) is deliberately reused across every `<script src>` on
the page -- so the first call destroyed the whole arena out from under
the still-live parser, leaving `g_url_parser.mraw` a dangling pointer.
The *second* `lxb_url_parse()` call then dereferenced that freed
memory inside `lexbor_mraw_alloc`, producing a deterministic crash on
exactly the second external script on any page, regardless of its
content or size -- exactly matching what was observed. This isn't
subtle or unique to this port: lexbor's own `url.h` documents the
gotcha verbatim, right next to the function: *"if you have a live
`lxb_url_parser_t` parsing object, you will have a pointer to garbage
after calling this function"*. `lxb_url_destroy()` is the correct
one-URL-at-a-time equivalent (`lexbor_mraw_free()` -- returns just
that object's memory to the arena's free list, leaving the arena
itself intact).

**Fix:** one line, `lxb_url_memory_destroy(resolved)` ->
`lxb_url_destroy(resolved)` in `resolve_script_url()`. No kernel
changes needed or made -- a kernel-side stack-layout theory was
investigated along the way (see below) and disproven, so
`BareMetal-Firecracker` ended this investigation with no changes at
all.

Boot-verified against everything that matters:

- The minimal `urltest2.c` repro: all three resolutions now succeed,
  no crash.
- `https://www.iana.org/domains/reserved`: all three external scripts
  (jQuery, `dtable.js`, `relative-time.js`) fetch and run with no
  crash. jQuery and `dtable.js` each throw a real, distinct DOM-gap
  exception (`cannot read property 'matches' of undefined`,
  `TypeError: not a function`); `relative-time.js` runs clean with no
  exception at all. The page's inline script still throws
  `$ is not defined` -- now for a *legitimate* reason (jQuery's own
  init code throws before it finishes assigning the `$`/`jQuery`
  globals, a `documentElement`-class gap, not "the script was never
  fetched").
- `https://httpbin.org/`: all three external scripts, including
  Swagger UI's real 1.4MB bundle, fetch and run with no crash, each
  throwing its own real exception.
- Regression checks, byte-for-byte/behavior-identical to every prior
  round: `https://example.com/` (no scripts), `https://www.wikipedia.org/`
  (now additionally fetches its 2 external scripts, which prior
  rounds never attempted, with no crash), and the static
  `examples/lexbor/browser/browser.c` hermetic test.

**A kernel-side theory, investigated and disproven.** Before finding
the real bug, the stack-layout reasoning above led to a hypothesis
that `exception_gate_main`'s RIP field was misreading the wrong stack
offset for exceptions with a hardware error code (GP/PF/DF/...) --
`BareMetal-Firecracker`'s `interrupt.asm` was temporarily patched to
read a different offset and dump more raw stack words to test this.
The wider raw dump proved the *original* code was already reading the
correct offset for this real GP fault in practice under
Firecracker/KVM (no separate error-code slot appears in the actual
stack layout observed, contrary to the plain-x86-SDM expectation) --
so the "fix" was backed out and `BareMetal-Firecracker` was left
completely unmodified. Recorded here so this dead end isn't
re-investigated blind next time: the exception dump's RIP field is
correct as originally written, at least for GP under this kernel/
hypervisor combination.

### DOM properties and Window stub methods

The next of the remaining follow-ups: `document.documentElement`/
`.body`/`.head`, `Element.className`, and
`window.addEventListener`/`removeEventListener`, closing the two real
gaps `wikipedia.org`'s scripts hit after the window-stub round
(`cannot read property 'className' of undefined` and
`TypeError: not a function`). Added identically to both `browser.c`
and `browser_fetch.c` (see "What's bound" above for the exact API).

Re-tested `https://www.wikipedia.org/` (the page these errors came
from) via `browser_fetch.c`:

```
title: Wikipedia

Uncaught exception (<script>): TypeError: not a function
Uncaught exception (https://www.wikipedia.org/portal/wikipedia.org/assets/js/index-7ecb9c7f8e.js): ReferenceError: Element is not defined
Uncaught exception (https://www.wikipedia.org/portal/wikipedia.org/assets/js/gt-ie9-507b16b6be.js): TypeError: not a function
```

The `className`/`documentElement` `TypeError` is gone -- the inline
script now runs further before hitting a *new*, later
`TypeError: not a function` (some other method call this scope
doesn't implement, not chased further -- that's the expected shape of
progress this project has shown every round). Its two external
scripts, never attempted by any prior round's write-up because the
crash predated getting this far, now also fetch and run with their
own distinct real errors and no crash: `Element is not defined` (a
page checking `typeof Element`/`instanceof Element` against a global
constructor this scope doesn't bind) and another `not a function`.

Full regression sweep, all unchanged in behavior from before this
round except where a page's own external scripts newly succeed at
fetching (they always could -- this round didn't touch fetching logic,
only DOM/window bindings):

- `https://example.com/` -- identical (`body: 559 byte(s)`, `title:
  Example Domain`, no scripts).
- `https://httpbin.org/` -- no crash; all three external scripts fetch
  and run: Swagger UI's bundle and standalone preset each throw
  `TypeError: not a function`, and jQuery now throws
  `TypeError: cannot read property 'createElement' of undefined` --
  a new, more specific gap (`document.createElement`, DOM mutation,
  still not bound, as documented) than whatever line it failed on
  before `documentElement` existed.
- `https://www.iana.org/domains/reserved` -- no crash; jQuery and
  `dtable.js` each throw `TypeError: not a function` (progressing
  further/differently than the pre-`documentElement` run); the page's
  own inline script still throws `$ is not defined`, for the same
  legitimate reason as before (jQuery's own init fails before
  assigning the `$`/`jQuery` globals).
- Static `examples/lexbor/browser/browser.c` test -- byte-for-byte
  identical boot output, since its fixture never exercises
  `documentElement`/`body`/`head`/`className`/`addEventListener`.

### Global `Element` constructor

The smaller of the two remaining gaps from the previous round:
`wikipedia.org`'s external script threw `ReferenceError: Element is
not defined` (real-world feature-detection code doing `typeof
Element`/`instanceof Element` against a global that didn't exist).
Bound identically in both `browser.c` and `browser_fetch.c`:

- `Element` is a real callable function object
  (`JS_NewCFunction(ctx, element_ctor_call, "Element", 0)`), so
  `typeof Element === 'function'`, matching a real DOM. Calling it
  (with or without `new`) throws `TypeError: Illegal constructor` --
  matching a real browser's behavior (there's no public `Element`
  constructor; instances only ever come from `document.querySelector`/
  `.documentElement`/`.body`/`.head`, never `new Element()`).
  `document.createElement()` remains unbound (see non-goals) so this
  doesn't create a false impression that construction works some other
  way.
- `Element.prototype` is set to the *exact same* prototype object
  every `Element` wrapper already has as its `[[Prototype]]`
  (`JS_GetClassProto(ctx, element_class_id)` reads back what
  `register_element_class()`'s `JS_SetClassProto()` stored), so
  `instanceof` works via ordinary reference-identity prototype-chain
  walking -- the same mechanism every real JS engine uses, not a
  special case.

**Isolated test** (a 5th `<script>` added to `browser.c`'s static
fixture, before touching a real page): `typeof Element` and
`document.body instanceof Element`. Boot-verified:

```
typeof Element=function
body instanceof Element=true
```

**Re-tested `https://www.wikipedia.org/`** (the page `Element is not
defined` came from): that error is gone, replaced by a *new* distinct
one on the same script -- `ReferenceError: navigator is not defined`
(the `navigator` global, a different, out-of-scope gap, not chased
here). The inline script and the other external script still throw
their own pre-existing `TypeError: not a function` (unrelated,
unaffected by this change).

**Regression sweep:**

- `https://example.com/` -- identical.
- `https://www.iana.org/domains/reserved` -- identical to the prior
  round's baseline (jQuery/`dtable.js` both `not a function`, inline
  `$ is not defined`).
- `https://httpbin.org/` -- identical to the prior round's baseline on
  2 of 3 attempts (Swagger bundle/standalone-preset both
  `not a function`, jQuery `createElement` of undefined). **One of the
  three attempts crashed instead**, partway through running the
  Swagger UI bundle, with `Exception 0x06 (UD)` -- a different fault
  type than the previously-fixed `#GP`, and at a very low `RIP`
  (`0x265`) suggestive of jumping into corrupted/incomplete data rather
  than anything related to the small, fixed `Element` global just
  added. Retried twice more, both clean and byte-for-byte matching the
  documented baseline -- not reproducible on demand. Recorded honestly
  as an observed anomaly while fetching a genuinely large (1.4MB) file
  over live internet from inside the test VM, not confirmed as caused
  by this change or root-caused further; worth watching for if it
  recurs, not chased blind on a single, non-reproducible occurrence.
- Static `examples/lexbor/browser/browser.c` test -- the original four
  scripts' output unchanged; the new 5th script's output shown above.

## DOM mutation

The remaining item from the original "Explicit non-goals" list:
`document.createElement`/`Element.appendChild`/`.setAttribute`/
`.remove`, added identically to both `browser.c` and `browser_fetch.c`.

**What's bound:**

- **`document.createElement(tagName)`** -- `lxb_html_document_create_element()`
  (the HTML-aware wrapper, not the lower-level
  `lxb_dom_document_create_element()` directly), so a created element
  goes through the same tag/interface-table lookup a parsed element
  already goes through. Not inserted into the tree by itself -- matches
  real DOM `createElement()` exactly.
- **`Element.appendChild(child)`** -- `lxb_dom_node_append_child()`,
  lexbor's own spec-shaped `Node.appendChild()` (its header explicitly
  contrasts it with `lxb_dom_node_insert_child()`'s unvalidated raw
  splice). Throws a real `TypeError` if `this` is a null/detached
  Element or the argument isn't a real `Element` wrapper -- the one
  mutation method that validates and throws, matching how
  `querySelector()` already throws on an invalid selector, rather than
  silently no-op'ing like the read accessors below.
- **`Element.setAttribute(name, value)`** -- `lxb_dom_element_set_attribute()`;
  a subsequent `getAttribute()`/`className` read reflects it. Stays
  permissive (returns `undefined`, doesn't throw) on missing args, kept
  consistent with `getAttribute()`'s existing soft-fail convention in
  this same file.
- **`Element.remove()`** -- the modern, argument-less `ChildNode.remove()`,
  via `lxb_dom_node_remove()`. `Node.removeChild(child)` (the older,
  two-party form) is deliberately not added -- unlike the four methods
  above, it needs real DOM-exception-code translation for "child isn't
  actually a child of this node," not just a straight lexbor call.

**Element lifetime, checked against lexbor's actual source rather than
assumed** (this project has already found one real bug from getting
exactly this kind of lifetime reasoning wrong -- see "External script
fetching"'s `lxb_url_memory_destroy` arena-teardown bug above): does a
JS-created-but-never-appended element leak or dangle, given
`element_finalizer` is a no-op? Traced `lxb_html_document_create_element()`
-> `lxb_dom_document_create_element()` -> `lxb_dom_element_create()`
-> `lxb_dom_document_create_interface()` -> (for the HTML document type)
`lxb_dom_document_create_struct()`, which is `lexbor_mraw_calloc(document->mraw, ...)`
-- the exact same long-lived memory arena every parsed node already
comes from. A created element is safe to leave un-freed with today's
no-op finalizer for the same reason every existing `Element` wrapper
already is: its lifetime is tied to the whole document's arena, torn
down only at `lxb_html_document_destroy()`, regardless of whether it
was ever attached to the tree.

**Hermetic round-trip test** -- a 6th `<script>` added to `browser.c`'s
static fixture: `document.createElement("div")`, `.setAttribute("id",
"made")`, `document.body.appendChild(made)`, then a *fresh*
`document.querySelector("#made")` call to prove the element is
genuinely in the tree afterward, not just a JS object floating on its
own. Boot-verified:

```
created tagName=DIV
```

**Regression sweep via `browser_fetch.c`:**

- `https://example.com/` -- identical (`body: 559 byte(s)`, `title:
  Example Domain`, no scripts).
- `https://www.iana.org/domains/reserved` -- identical to the prior
  round's documented baseline, no crash (jQuery/`dtable.js` both
  `not a function`, inline `$ is not defined`) -- none of `iana.org`'s
  scripts touch DOM mutation, so this is an unaffected-by-this-change
  check, not a new success.
- `https://httpbin.org/` -- mixed, and worth being precise about. On
  attempts that ran a script further before crashing (see below),
  Swagger UI's bundle progressed to a *new*, deeper error,
  `TypeError: cannot read property 'cssFloat' of undefined` (previously
  just `not a function`) -- real forward progress. jQuery's own error
  message is **unchanged** (`TypeError: cannot read property
  'createElement' of undefined`) -- on inspection this is *not* actually
  about `document.createElement` being missing (it's bound now); jQuery
  is reading `.createElement` off some other object this scope doesn't
  provide (most likely `elem.ownerDocument`, which isn't bound), so
  this specific error was always going to need a different fix than the
  one just added -- worth correcting here since the *previous* round's
  writeup assumed binding `createElement` would resolve it.
- **The already-documented, already-parked intermittent crash (see
  "Stack-depth crash investigation" below) recurred multiple times
  during this round's `httpbin.org` testing** -- roughly half of ~7
  attempts, `Exception 0x13 (GP)`, RSP at the same known debug-dump-
  artifact value documented below. Disassembling the unstripped
  `build/browser_fetch.app.elf` at the two distinct fault `RIP`s seen
  this round places both inside `js_free_value_rt` -- QuickJS-ng's own
  internal reference-counted value-freeing function, performing a
  doubly-linked-list unlink, the same family of internal GC/refcount
  bookkeeping as the previously-found `free_var_ref`/
  `remove_gc_object()` crash site, just a different nearby function.
  This is additional evidence the bug is a heap-layout-dependent
  corruption inside QuickJS-ng's object-list bookkeeping that can
  surface at more than one internal call site, not something specific
  to `free_var_ref` alone -- **not re-investigated further**, per the
  user's explicit decision to stop chasing this bug and move on to
  other follow-ups; recorded here only as a new data point for whoever
  eventually resumes that investigation. Confirms this crash is
  unrelated to the DOM-mutation code added this round (`iana.org`,
  whose scripts never call `createElement`/`appendChild`, saw zero
  crashes across its own regression runs).
- Static `examples/lexbor/browser/browser.c` test -- the original five
  scripts' output unchanged; the new 6th script's output shown above.

## `getElementById` / `querySelectorAll`

The last two items from the original DOM follow-up list, closing it
out entirely (only `Node.removeChild`, `navigator`, and most `Window`
methods remain as named gaps after this). Added identically to both
`examples/lexbor/browser/browser.c` and `examples/lexbor/browser-fetch/
browser_fetch.c`, same as every prior binding-layer change.

**`document.querySelectorAll(selector)`/`Element.querySelectorAll(selector)`**
share `document_querySelector`'s exact CSS-parser/selector-init
scaffolding via a new `query_selector_all()` helper -- only the
callback changes (`qsa_find_cb` collects every match instead of
keeping the first) and, for the element-scoped version, the search
root (that element instead of the whole document). Each match is
appended to a real JS `Array` via `JS_NewArray()` +
`JS_SetPropertyUint32()`; verified with real `JS_Eval`'d test code
(not assumed) that QuickJS maintains a correct `.length` and that
numeric indexing (`arr[0].tagName`) works. The element-scoped case's
"don't match the element itself, only its descendants" semantics come
for free: reading `lxb_selectors_tree()` in lexbor's own source shows
`root` is only included in matching when the caller explicitly sets
`LXB_SELECTORS_OPT_MATCH_ROOT`, which this port never does -- exactly
real `querySelectorAll()`'s scoping rule, no extra logic needed.

**`document.getElementById(id)`** is deliberately a direct recursive
tree walk (`find_by_id_recursive()`, matching by `id`-attribute string
equality via `lxb_dom_element_get_attribute()`), not a `"#" + id`
CSS-selector query the way `querySelector()` works. A bare `#id`
selector can't represent every string a real `id` attribute is legally
allowed to hold (a leading digit, an embedded space, `.`/`:`/etc.)
without CSS escaping this scope doesn't implement -- the tree walk
matches real DOM `getElementById()`'s exact-string-equality contract
with none of that edge-case surface.

**Hermetic test** (`browser.c`'s static fixture, 7th `<script>`):
`getElementById("msg2")` finds a second static `<p>` by id;
`getElementById("nope")` returns `null`; `querySelectorAll("p")`
returns `.length === 2` with correct per-index `.tagName`; a
zero-match selector (`.nope`) returns a real empty array
(`.length === 0`), not `null`/`undefined`. All passed on boot (see
"Example" above for the full transcript).

**Regression sweep via `browser_fetch.c`:**
- `example.com` -- identical to documented baseline.
- `iana.org` -- identical (`not a function` on jQuery's external
  script, `$ is not defined` on the inline one) -- neither script
  touches `getElementById`/`querySelectorAll`.
- `wikipedia.org` -- identical (`navigator is not defined`,
  `not a function`) -- same reason, unaffected.
- `httpbin.org` -- **the already-parked intermittent crash resurfaced,
  2 boots in a row this time, with new evidence worth recording even
  though it wasn't chased further** (per the standing decision to stop
  investigating it): both crashes now print
  `Assertion failed: list_empty(&rt->gc_obj_list) (quickjs.c:
  JS_FreeRuntime: 2704)` immediately before the register dump -- a
  QuickJS-ng-internal consistency check, run at `JS_FreeRuntime()`
  time, catching that its own GC object list is *not* empty when it
  should be. This is more specific than any prior evidence for this
  bug (previous rounds only had raw fault `RIP`s) and points concretely
  at a real reference/lifetime leak somewhere keeping a `JSValue` alive
  past when it should've been collected -- consistent with, but not
  proof of, the already-recorded hypothesis that this lives in the
  binding layer's interaction with QuickJS's GC rather than genuinely
  inside QuickJS-ng in isolation. Both crashes: identical `RIP`
  (`FFFF80000020501F`) and `RSP`, after both of `httpbin.org`'s
  external scripts (`swagger-ui-bundle.js`, `jquery.min.js`) had
  already printed their own real, distinct exceptions -- same
  "crashes only after scripts finish, during teardown" shape as every
  previous occurrence. **Not re-investigated further, per the user's
  explicit decision** -- recorded here as a new, sharper data point for
  whoever resumes that investigation, not chased into a fix.
- Static `browser.c` test -- see "Example" above for the full,
  unchanged-plus-new-7th-script transcript.

## Stack-depth crash investigation (partial progress, not fully fixed)

The "unreproduced anomaly" noted above (one `Exception 0x06 (UD)` on
`httpbin.org`, out of three attempts, during the global-`Element`
regression sweep) was investigated further at the user's request,
rather than left alone. This turned out to be two separate findings,
only one of which is resolved.

**Reproduction:** 16 repeated boots against `https://httpbin.org/`
reproduced the anomaly twice more (2/16, ~12%, roughly consistent with
the original 1-in-3 sample once averaged over a larger run) -- once as
`Exception 0x14 (PF)` with `RIP=CR2=0xFFFFFFFFFFFFFFFF` (an invalid,
all-ones instruction address), once as the original `Exception 0x06
(UD)` at a very low `RIP` (`0x215`, essentially matching the original
report's `0x265`). Both crashes happened **mid-script-execution** --
after only the *first* of `httpbin.org`'s three external scripts had
printed its exception, never reaching the second or third.

**Hypothesis tested:** this port's ring-3 app stack is a fixed 64KB
region with no guard page (`BareMetal-Firecracker`'s
`src/BareMetal/sysvar.asm`: `os_usr_stack_base` at `0x1D0000`-
`0x1DFFFF`, sitting directly below the kernel's own 64KB ring-0 stack
at `0x1C0000`-`0x1CFFFF`) -- an overflow past the low end wouldn't
fault cleanly, it would silently corrupt the kernel's own
interrupt/syscall stack, which is consistent with varying,
garbage-looking fault signatures. `run_external_script()` was, at the
time, called from *inside* `lxb_selectors_find()`'s own DOM-recursion
depth (`script_find_cb`), adding an unknown, page-structure-dependent
amount of stack on top of curl/mbedTLS's own famously large TLS-
handshake stack frames -- a call chain no single-fetch example in this
project had ever exercised before external-script fetching existed.

**Mitigation applied:** `run_scripts()` in `browser_fetch.c` now runs
in two passes -- pass 1 (`script_find_cb`, still inside
`lxb_selectors_find()`'s recursion) does nothing but append each
`<script>` node to a fixed `MAX_SCRIPTS_PER_PAGE`-sized array; pass 2
(a plain loop in `run_scripts()`'s own shallow frame, after the
selector-matching recursion has fully returned) does the actual
fetch/eval work. This removes lexbor's own selector-recursion depth
from the stack budget at the moment mbedTLS's handshake runs, without
changing any observable behavior (script execution order is preserved
-- `scripts.nodes[]` is filled in the same document-order
`lxb_selectors_find()` already visited them in).

**Result: inconclusive on its own, but revealed a second, distinct bug.**
20 more boots against `httpbin.org` with this change applied:
- The original mid-script-execution crash (garbage RIP, non-
  deterministic fault type) did **not** recur at all in these 20 runs.
  Consistent with a real fix, though not proven by sample size alone.
- A **different** crash appeared instead, at a higher observed rate
  (6/20, ~30%): `Exception 0x13 (GP)`, with **RIP identical across
  every occurrence** (`FFFF80000010CA6B`) -- and critically, happening
  only *after all three* external scripts had already printed their
  exceptions successfully, during `JS_FreeContext()`/`JS_FreeRuntime()`
  teardown in `main()`'s own (already shallow, unchanged-by-this-fix)
  stack frame. Disassembling the unstripped `build/browser_fetch.app.elf`
  at that address places the fault inside QuickJS-ng's own internal
  `free_var_ref()` function, at a doubly-linked-list unlink
  (`rcx->next = rdx; rdx->prev = rcx`, the classic `list_del()`
  pattern QuickJS uses pervasively for its GC object lists) --
  reference-counted closure-variable cleanup, not anything this app's
  own code directly touches.

**Why this looks like two separate, previously-entangled bugs rather
than one:** the teardown-time crash is called from `main()` in both
the old and new code -- the two-pass restructuring never changed its
call depth at all, so it can't be *caused* by that change. The most
coherent explanation: this QuickJS-internal heap/GC corruption bug
was already present before the stack-depth fix, but the *earlier*
mid-execution crash was killing the VM first in enough runs that the
later teardown-time bug rarely got the chance to manifest and be
observed. Fixing (or at least sharply reducing) the first crash let
more runs survive long enough to reach `JS_FreeContext()`, making the
second, previously-hidden bug more visible -- not introducing it.

**Not fixed. Investigated further at the user's explicit request,
narrowed down significantly, still not root-caused.** The disassembly
placed the fault inside QuickJS-ng's `free_var_ref()`, specifically at
`remove_gc_object()`'s `list_del(&h->link)` -- an intrusive doubly-
linked-list unlink against `rt->gc_obj_list`, the runtime's single
shared list of every GC-tracked object. A corrupted neighbor pointer
there is consistent with either a genuine QuickJS-ng bug, or heap
corruption from something else entirely that happens to land on a
`JSGCObjectHeader` in the shared musl heap (QuickJS's allocator, this
app's own `growable_buf`, and lexbor's `mraw` arena are all just
`malloc`/`realloc`/`free` underneath, sharing one heap) -- these have
different implications and different fixes, so which one matters.

Before assuming this was inside QuickJS-ng's own code and out of this
project's control, this project's own binding-layer code
(`browser_fetch.c`'s `setup_globals`/`register_element_global`/
`document_get_documentElement`/`.body`/`.head`/`element_get_className`/
the `Element` constructor binding, and every `JS_FreeValue`/
`JS_DupValue`/`JS_SetPropertyStr` ownership-transfer site in the file)
was audited by hand for a refcount mistake, since this project has
already found one real bug of exactly this shape
(`lxb_url_memory_destroy()` above). Nothing found: every `JS_NewCFunction`/
`JS_NewObject` result is consumed by exactly one `JS_SetPropertyStr` (which
takes ownership), and `JS_GetClassProto()` (used by the `Element`
constructor binding) already increments the class prototype's refcount
internally before handing it to `JS_SetPropertyStr` -- textbook-correct,
not a suspect.

**Four increasingly faithful standalone repros were built and boot-
tested, all with zero lexbor HTML/DOM/CSS/selectors involved** (57
combined boots, no crash in any of them):
1. Lexbor's `url` module (the newest, least-battle-tested vendored
   addition, used only by `browser_fetch.c` -- `resolve_script_url()`'s
   exact `lxb_url_parse()`/`lxb_url_destroy()` pattern) interleaved
   with trivial QuickJS closures that create real detached var-refs
   (the exact GC object type `free_var_ref()` operates on), no curl at
   all: **10/10 clean.**
2. Real curl/mbedTLS fetches (the exact URLs from the real crash --
   `httpbin.org`, `iana.org`, jQuery) interleaved with the same trivial
   closures, one shared `JSContext`, no lexbor `url` module at all:
   **15/15 clean.**
3. Real jQuery fetched and `JS_Eval`'d for real (not a synthetic
   snippet) against a *fresh* `JSRuntime`/`JSContext` created and torn
   down each iteration: **12/12 clean** (an earlier batch of this same
   test looked like 12/12 crashes, but that was a test-harness bug --
   the poll loop's `grep "Exception"` matched the substring inside the
   app's own printed `js exception: TypeError...` line and killed the
   VM mid-run, mistaking a normal caught JS exception for a kernel
   crash dump; fixed to match the actual `Exception 0x` crash-dump
   format and re-run clean).
4. The most faithful match yet to `browser_fetch.c`'s real structure:
   **one shared** `JSRuntime`/`JSContext` (not recreated per script,
   unlike test 3) with three different real, large, minified scripts
   (jQuery twice from different CDNs, plus lodash) fetched and
   `JS_Eval`'d sequentially into it, matching `run_scripts()`'s loop
   exactly: **20/20 clean.**

**What this rules out, and what it narrows the hypothesis to:** neither
the `url` module, nor real network I/O, nor running real large
minified JS against QuickJS -- alone or in combination with each other
-- reproduces this on their own. The crash appears to require the one
remaining untested combination: lexbor's actual parsed DOM tree and
`Element` wrapper `JSValue` objects (holding raw `lxb_dom_node_t*`
opaque pointers into that tree) coexisting with QuickJS's heap at the
same time real external scripts run -- i.e., something specific to
`browser_fetch.c`'s full binding layer, not genuinely inside QuickJS-ng
in isolation. Building a repro for *that* combination is substantial
(most of `browser_fetch.c` itself, minus only the `url` module) and
was not attempted in this pass, given how much ground the four lighter
repros already covered.

**Not chased further this round.** The two-pass restructuring is kept
(it's a real, independently-justified stack-safety improvement, and
correlates with eliminating the original stack-depth crash mode in
testing), but the honest state is: **running multiple external scripts
against one real page can still intermittently crash the VM**, via a
narrowed-down but not fully root-caused bug that most likely lives in
the DOM<->QuickJS binding layer's interaction with QuickJS's heap, not
in QuickJS-ng itself, curl, or the `url` module alone. Regression-
verified unaffected throughout this investigation: `example.com`,
`iana.org`, `wikipedia.org` (all producing identical output to their
documented baselines), and the static `browser.c` test (untouched by
this change).

### The leak, identified for real (still not root-caused)

Investigated further at the user's explicit request, after the
`getElementById`/`querySelectorAll` round's regression sweep produced a
sharper clue than any prior round: the crash now printed
`Assertion failed: list_empty(&rt->gc_obj_list)` before the register
dump -- a real quickjs-ng-internal consistency check, run inside
`JS_FreeRuntime()` after it has already drained every pending job
(`rt->job_list`, confirmed by reading quickjs.c: every queued job's
`argv[]` is freed before this check runs, ruling out an un-drained
Promise `.then()`/async callback as the mechanism) and run a full cycle
collection (`JS_RunGC(rt)`). It fires only when real objects are still
alive with nonzero refcount after all of that -- a genuine leak, not
this app forgetting to pump an event loop.

**Before assuming this was unfixable/unknowable, the by-hand audit of
this project's own newest binding functions from the previous round
was re-verified rather than repeated blind**
(`document_createElement`/`element_appendChild`/`element_setAttribute`/
`element_remove`/`query_selector_all`/`qsa_find_cb`/
`find_by_id_recursive`, plus `register_element_global`'s
`JS_GetClassProto()` usage): every `JS_NewObjectClass`/`JS_NewArray`/
`JS_NewCFunction` result is consumed by exactly one ownership-taking
call (`JS_SetPropertyStr`/`JS_SetPropertyUint32`/a function return),
`JS_SetPropertyUint32`'s documented "always consumes `val`, success or
failure" contract means `qsa_find_cb` never leaks even if a set
silently fails, and `element_appendChild`'s
`JS_DupValue(ctx, argv[0])` return is exactly balanced by the DOM's own
child insertion not touching JS refcounts at all (`lxb_dom_node_append_child`
operates purely on the `lxb_dom_node_t*` tree, never on the `JSValue`
wrapper). No app-side ownership mistake found here, same as the
previous round's conclusion -- confirmed again, not just re-asserted.

**What actually cracked it open: quickjs-ng's own leak-dump
infrastructure was already compiled into this vendored build, just
never turned on.** `ENABLE_DUMPS` is unconditionally `#define`'d near
the top of `quickjs.c`, gating a whole family of diagnostic dumps
(`JS_DUMP_LEAKS` among them) behind a runtime flag
(`rt->dump_flags`, set via the real, exported `JS_SetDumpFlags()` API)
that this app simply never called. Added one line --
`JS_SetDumpFlags(rt, JS_DUMP_LEAKS)` right after `JS_NewRuntime()` in
`browser_fetch.c` -- costing nothing on a clean run (the dump only
fires from inside the same leak-check `JS_FreeRuntime()` already runs)
and left in deliberately, not reverted, for whoever resumes this
investigation next.

Reproduced on the 2nd of 2 boots against `https://httpbin.org/` (using
completion-polling instead of a fixed sleep this time -- a first
attempt at 20 boots with a 5-second fixed sleep produced zero crashes,
but turned out to be invalid: every run was killed before the page's
scripts had even finished fetching, let alone reaching teardown).
**The actual leak dump:**

```
Uncaught exception (https://httpbin.org/flasgger_static/swagger-ui-bundle.js): TypeError: cannot read property 'cssFloat' of undefined
Uncaught exception (https://httpbin.org/flasgger_static/lib/jquery.min.js): TypeError: cannot read property 'createElement' of undefined
Object leaks:
       ADDRESS REFS SHRF          PROTO      CLASS PROPS
0xffff800001b15850    1   0* 0xffff8000005990e0     Object { isNothing: [Function ...], isObject: [Function ...], toArray: [Function ...], repeat: [Function ...], isNegativeZero: [Function ...], extend: [Function ...] }
0xffff800001b15b20    1   0  0xffff800000599130   Function { length: 2, name: "r", prototype: [Object ...] }
0xffff800001b15cb0    1   0  0xffff800000599130   Function { length: 5, name: "i", prototype: [Object ...] }
0xffff800001b1ba70    1   0* 0xffff800001b17830     Object { include: [Array ...], implicit: [Array ...], explicit: [Array ...], compiledImplicit: [Array ...], compiledExplicit: [Array ...], compiledTypeMap: [Object ...] }
0xffff800001b1d4d0    1   0* 0xffff800001b17830     Object { include: [Array ...], implicit: [Array ...], explicit: [Array ...], compiledImplicit: [Array ...], compiledExplicit: [Array ...], compiledTypeMap: [Object ...] }
Assertion failed: list_empty(&rt->gc_obj_list) (build/quickjs-ng-0.16.2/quickjs.c: JS_FreeRuntime: 2704)
```

**Identifying the leaked objects:** the property shapes are
unmistakable. `{ isNothing, isObject, toArray, repeat, isNegativeZero,
extend }` is `js-yaml`'s `lib/common.js` module exports verbatim; the
two `include`/`implicit`/`explicit`/`compiledImplicit`/
`compiledExplicit`/`compiledTypeMap` objects are `js-yaml`'s internal
`Schema` instances (its default schema singletons); the two anonymous
`r`/`i` functions are minified constructors from the same module.
Swagger UI's real bundle vendors `js-yaml` internally to parse
OpenAPI/Swagger specs written in YAML -- these are legitimate module-
level singleton objects the bundle's own code creates while loading,
before the script throws. Each shows exactly 5 external references
total across the whole dump, refcount 1 each, surviving a full
`JS_RunGC()` cycle-collection pass intact.

**Ruled out by reading quickjs.c directly, not assumed:** an
undrained Promise/async job queue entry (`rt->job_list` is fully
drained -- every entry's `argv[]` freed -- *before* `JS_RunGC()` and
the leak check run, so a `.then()` callback closure would already be
gone by this point, not a candidate).

**Not root-caused further this round.** What's left as the honest
remaining hypothesis: something in quickjs-ng's own bytecode-execution
machinery (most plausibly an inline-cache or shape-cache slot embedded
in compiled function bytecode, which lives outside the normal
refcounted object graph the cycle collector walks) retains a reference
to these specific module-singleton objects after the script that
created them stops executing, independent of whether `window`/
`document` or anything in this project's own binding layer still
points at them. This is consistent with, but does not prove, a genuine
quickjs-ng-internal bug rather than an application-level one -- the
previous round's four clean isolated repros (network alone, `url`
module alone, real large JS against fresh *and* shared contexts) never
included a script that actually creates several singleton objects with
this exact shape (a schema/registry pattern with cross-referencing
default instances) the way `js-yaml`'s module-load code does, so this
specific pattern was never actually tested in isolation -- the next
concrete step for whoever resumes this would be a standalone repro
running real `js-yaml` source (not just jQuery/lodash, which don't
have this module-singleton-schema shape) against QuickJS alone, no
lexbor DOM at all, to determine whether *that alone* -- with no DOM
binding layer involved -- reproduces the same leaked-object signature.

Regression-verified unaffected: `example.com` and `iana.org` (both
byte-for-byte/behavior-identical to their documented baselines with
`JS_DUMP_LEAKS` enabled) and the static `browser.c` test (untouched,
`JS_SetDumpFlags` was only added to `browser_fetch.c`).

## Honest assessment: how close is this to "a minimal headless browser"?

Close, for toy/simple pages: fetch (curl), parse (lexbor), and run
scripts against a real DOM with real output -- the full pipeline exists
and works end to end for a page whose scripts only touch
`document.querySelector`/`textContent`/`tagName`/`getAttribute` and
plain JS, live network fetch included, verified above with
`browser_fetch.c`.

Far, for anything resembling a real-world page -- now confirmed against
four actual live sites, iterated on four times (a growable fetch
buffer, a `window` stub, external script fetching -- which also found
and fixed a real crash bug along the way, see "External script
fetching" above -- and DOM properties/`Window` methods/the global
`Element` constructor). External-script fetching's own logic -- URL
resolution, HTTP fetch, running the result -- is real and WHATWG-
correct, verified fetching and running genuinely large real scripts
(jQuery, and Swagger UI's 1.4MB bundle). It is not, however, crash-free
end to end: see "Stack-depth crash investigation" above for a real,
partially-diagnosed, still-open QuickJS-internal crash that can occur
after running several scripts from one fetched page.

For the DOM+JS binding layer itself, the trend from earlier rounds
holds all the way through seven rounds now (growable buffer, window
stub, external script fetching, DOM properties/window stub methods,
global `Element`, DOM mutation, `getElementById`/`querySelectorAll`):
`httpbin.org`'s scripts run further with each round (a `not a function`
became a more specific `cssFloat`/`createElement`-of-undefined gap once
`documentElement`/`Element` existed); `$ is not defined` on `iana.org`
is resolved at the fetch level (jQuery genuinely loads and runs) but
still fails, now past `documentElement`-class checks and into a
*different* `not a function` gap; `wikipedia.org`'s inline script no
longer fails on `className`/`documentElement`/`Element` at all,
progressing each time to a later, more specific error
(`not a function`, then `navigator is not defined`); every external
script on every page now fetches and runs to its own real, distinct
exception. The pattern holds: each round's fix makes real pages fail
later, for narrower and more specific reasons, never the same wall
twice.

Every item from the original DOM/`Window` follow-up list is now bound
except `Node.removeChild` (deliberately deferred -- needs real
DOM-exception-code translation, not just a lexbor call) and most of
`Window`'s surface (`navigator` doesn't exist at all; `window` has
only the two event-listener stubs, no `matchMedia`/
`requestAnimationFrame`/etc.). No event handling ever fires (no event
loop, by design), and no `fetch`/XHR from JS. What's genuinely
unresolved is not a missing binding but a bug: **running multiple
external scripts against one real page can still intermittently crash
the VM** (see "Stack-depth crash investigation" above) -- narrowed
significantly across four investigation rounds now (a real stack-depth
bug found and fixed; the remaining crash traced to specific QuickJS-ng
internal functions; a `list_empty(&rt->gc_obj_list)` assertion failure
pointing at a genuine reference leak; and, most recently, the exact
leaked objects identified by name -- `js-yaml`'s module-level schema
singletons, bundled inside Swagger UI's real script -- via quickjs-ng's
own already-compiled-in leak-dump facility, turned on for the first
time with one line). Still not root-caused to a specific quickjs-ng
line or fixed -- see "The leak, identified for real" above for the
concrete next step (a standalone `js-yaml`-only repro) whoever resumes
this should try first. Every other binding gap here is a distinct,
addable follow-up, not a structural blocker; this crash is the one
open item that's a real bug rather than a scope cut.
