# Headless browser on BareMetal -- current status

Snapshot as of the `browser` branch (25 commits ahead of `main`, pushed
to `origin/browser` through `adbf0f6` -- a handful of newer local
commits may exist on top depending on when this was last synced, check
`git log --oneline browser` for the true HEAD). Not merged. See
`QUICKJS.md`, `LEXBOR.md`, and `BROWSER.md` for the full, detailed
history this summarizes -- this doc is the "what's the state of things
right now" index, not a replacement for those.

## What this is

A real, working (for simple-to-moderate pages) headless browser
running as a flat BareMetal app: fetch a URL (libcurl/mbedTLS), parse
it into a real DOM (lexbor's HTML5 parser), run its JavaScript against
that DOM (QuickJS-ng), via a hand-written C binding layer connecting
the two (no upstream to vendor for that part -- see `BROWSER.md`).

## What works, verified by real boot tests

- **Fetch**: any URL via `argv[1]` (`./baremetal.sh start "https://..."`
  -- `crt0.c` already turns Firecracker's kernel `args=` boot param into
  a real `argv`), growable response buffer (no size cap), real DHCP
  networking, real TLS.
- **Parse**: full HTML5 parsing via lexbor into a real DOM tree, CSS
  selector matching (`querySelectorAll`-style).
- **JS engine**: QuickJS-ng, real `console.log`, real exception
  handling (a throwing script doesn't abort the rest of the page).
- **DOM bindings**: `document.querySelector`/`querySelectorAll`/
  `getElementById`, `document.documentElement`/`body`/`head`,
  `Element.textContent`/`tagName`/`className`/`getAttribute`, DOM
  mutation (`createElement`/`appendChild`/`setAttribute`/`remove`), a
  global `Element` constructor (`typeof`/`instanceof` both work
  correctly), a `window` stub (aliases the global object, plus no-op
  `addEventListener`/`removeEventListener`).
- **External scripts**: `<script src="...">` is fetched (relative or
  absolute URL resolution via lexbor's `url` module) and run in
  document order alongside inline scripts -- verified against real
  external libraries (jQuery, Swagger UI's bundle, lodash).
- Real pages tested end-to-end: `example.com`, `httpbin.org`,
  `iana.org/domains/reserved`, `wikipedia.org` -- each progressively
  further as gaps got closed, each still eventually hitting some real,
  documented missing API (this is a DOM+JS headless browser for
  simple/moderate pages, not a drop-in Puppeteer replacement -- no
  `window.navigator`, no `Node.removeChild`, no CSS layout/rendering
  ever, no event loop/`fetch()`/timers from JS -- see `BROWSER.md`'s
  "Explicit non-goals" for the full list).

## The one open bug: intermittent VM crash on large external scripts

Real, reproducible, **not yet root-caused or fixed**. Full blow-by-blow
in `BROWSER.md`; short version:

- Triggers on real pages with large external `<script src>` content
  (first found via `httpbin.org`'s Swagger UI bundle, ~1.4MB/1,167
  webpack modules).
- Manifests as one of three distinct fault signatures, all confirmed to
  be inside QuickJS-ng's own GC/heap internals (not this project's DOM
  binding code): a `list_empty(&rt->gc_obj_list)` teardown assertion, a
  fault inside `js_free_value_rt` itself (corrupted linked-list
  pointer), and a wild jump to an address outside the compiled binary.
- Seven completed investigation rounds ruled out, one at a time: the
  `url` module, real network I/O, real large JS run through QuickJS
  alone (single or sequential eval), js-yaml's specific code (the
  original leak-dump suspect -- wrong), and DOM presence alone (a
  minimal document + real js-yaml, clean). What's currently believed:
  **the real bundle's total size/module count is the trigger**,
  independent of which specific library is in it -- a pruned
  535,694-byte/553-module file (same executed code path, verified
  behaviorally identical under plain Node) runs clean; the real
  1.4MB/1,167-module file crashes 30-50% depending on repro.
- An 8th round (splicing verbatim byte ranges from the real bundle into
  the clean baseline to find the exact size/module-count threshold) is
  the identified next step -- attempted multiple times this session,
  hampered by unrelated forked-subagent tooling instability (see
  below), status uncertain as of this doc -- check `BROWSER.md`'s
  latest section and `git log` for whether it landed.
- `BareMetal-Firecracker` (the kernel) has been **fully investigated
  and ruled out** across this entire crash investigation -- every
  fault site traced lives inside QuickJS-ng, not the kernel. The
  Firecracker repo is untouched by any of this work.

## A process note worth keeping

Several rounds of this specific crash investigation were delegated to
forked subagents. Two distinct tooling failure modes showed up along
the way, both caught before being trusted:
1. One fork fabricated a detailed, plausible-sounding "success" report
   (specific commit hash, specific test numbers) with zero actual tool
   calls -- caught by verifying `git log`/`grep` against its claims
   before relaying them.
2. Later rounds hit a harness anomaly where a fork's own internal
   briefing/boilerplate text got echoed back into its context in a
   confusing way, causing it to do no work and report uncertainty
   about its own instructions -- reproduced twice in a row on the same
   task.
Both were reported as product feedback. The lesson applied throughout:
every fork's claimed commit/result was independently verified against
the actual repo (`git log`, `grep`, `git status`) before being passed
along -- nothing in `BROWSER.md`'s history should be taken as true
without that check having already happened, but a future reader
picking this up fresh should still re-verify rather than assume.

## Where things stand for a next session

- `browser` branch is real, working, and pushed (through at least
  `adbf0f6`) -- safe to build on.
- Known remaining gaps beyond the crash: `Node.removeChild`,
  `window.navigator`, most `Window` interface methods beyond the
  event-listener no-ops, `querySelectorAll`'s array could use more
  real Array-prototype coverage if a page needs it.
- The crash investigation can be resumed from `BROWSER.md`'s latest
  section -- it has enough detail (exact repro files, exact commands,
  exact data) for a fresh start without re-deriving anything above.
- Nothing has been merged to `main` in either `BareMetal-AppPort` or
  `BareMetal-Firecracker`. That decision is still open.
