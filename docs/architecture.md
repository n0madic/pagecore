# PageCore Architecture

This document describes how PageCore is built internally. It is aimed at
contributors and at embedders who need to reason about behavior beyond the public
API. For usage, see the [README](../README.md); for per-API browser-compatibility
status, see [browser-api-support.md](browser-api-support.md).

## Overview

PageCore is a C++20/CMake core that composes three engines:

- **Lexbor** is the single source of truth for HTML parsing, DOM storage,
  selectors, mutations, and serialization.
- **QuickJS-NG** runs page JavaScript.
- **litehtml** (optional, `PAGECORE_ENABLE_RENDERING`) performs CSS layout, and a
  **Cairo/PangoCairo** backend rasterizes the result.

`pagecore::Page` ties them together; rendering is decoupled through a
backend-neutral `DisplayList` so layout engines and raster backends can be
swapped without touching the public API.

## HTML and the DOM

Lexbor owns the DOM tree. The C++ `DomDocument` wraps it and hands out numeric
`NodeId`s. A `dom_bridge` exposes primitive DOM operations to JavaScript, keeping
Lexbor as the only DOM store.

DOM wrappers are hardened with a Lexbor-backed `mutationVersion`/`hasNode`
contract: after a destructive mutation a stale wrapper fails cleanly instead of
dereferencing a removed node. Every node freed on the C++ side is un-registered
so its `NodeId` can never resolve to freed memory.

## JavaScript runtime

QuickJS-NG runs both classic and module scripts. The browser-like environment
lives in `src/dom_shim/`: standalone `install(ctx, api)` JavaScript modules for
`window`, `document`, `Node`, `Element`, `HTMLElement`, events, DOM traversal,
constraint validation, forms, timers, CSSOM, streams, and `fetch`/XHR. Each
module declares explicit dependencies and exports; CMake concatenates them into
`generated/dom_shim.js` at build time and embeds that artifact into the binary.

PageCore only installs page-visible globals for APIs with real support or an
intentionally useful partial implementation. Feature-detection surfaces without a
real implementation — `Worker`, `SharedWorker`, `navigator.serviceWorker`,
`IntersectionObserver`, `ResizeObserver`, `PerformanceObserver` — are left absent
rather than exposed as no-op or always-failing stubs, so scripts fall back to
their non-supported code paths. Detailed `supported`/`partial`/`absent` status is
in [browser-api-support.md](browser-api-support.md).

After install, the shim deletes the raw native `__dom`/`__host` bridges from the
page-visible global, so page script must go through the wrapper layer instead of
the C++ API directly. The bridge removal runs unconditionally, even if a module
fails during install, so the sandbox boundary is never left half-open.
`crypto.getRandomValues`/`randomUUID` are backed by the host OS CSPRNG, not
`Math.random`.

Top-level script exceptions are logged as console errors and do not abort later
scripts or lifecycle dispatch. Unhandled promise rejections — including a
top-level `throw` in an ES module, whose evaluation returns a rejected promise —
are surfaced the same way through a host promise-rejection tracker. Direct
`Page::eval()` remains fail-fast.

Adversarial script is bounded on several independent axes: `LoadOptions::js_timeout`
caps each individual script's execution, while `LoadOptions::max_load_time` caps
the aggregate wall-clock of the whole `<script>` sequence (so K scripts cannot
consume K × `js_timeout`); once the aggregate deadline passes the remaining scripts
are skipped. `LoadOptions::js_memory_limit_bytes` bounds the QuickJS heap, and
`LoadOptions::max_dom_nodes` separately bounds the cumulative number of DOM nodes
scripts may create (`createElement`, `cloneNode`, `innerHTML`, …) — native Lexbor
node memory the JS heap limit does not cover. Setting a limit to 0 / `std::nullopt`
disables that axis.

## Resource pipeline

Resources flow through `ResourceRequest`/`ResourceResponse` with request kinds
(`Document`, `Script`, `Stylesheet`, `Image`, `Font`, `Other`), referrer, HTTP
method/body/headers for XHR/fetch, response headers/status text/final-URL
metadata, and typed `ResourceError`s. `Page`, JS script loading, litehtml
stylesheet imports, and image placeholders all share this one pipeline.

- **`CurlResourceLoader`** performs real network/file transfers. It applies the
  configured user agent, sends a sanitized `Referer` (HTTP(S) only, no
  userinfo/fragment, downgraded to origin-only cross-origin —
  strict-origin-when-cross-origin), and drops outgoing headers containing control
  characters (CR/LF/NUL) to prevent header injection. It reuses a libcurl share
  handle (`CURLSH`) so connections, DNS, and TLS sessions stay alive between
  sub-resource loads.
- **`CachingResourceLoader`** is a bounded, thread-safe, LRU-capped decorator;
  error responses are not cached.
- **`ResourceLoader::load_all`** loads a batch in request order. In the default
  `FailFast` mode it throws the first request-order error like `load`; in
  `Lenient` mode it never throws and returns a status-0 placeholder for each
  failed request (for non-critical sub-resources). `CurlResourceLoader` overrides
  it to fetch concurrently with a single-threaded `curl_multi` driver, bounded to
  a browser-like 6 connections per host; `CachingResourceLoader` serves hits then
  batches only the misses.

`LoadOptions::js_resource_load_policy` and the JS resource budgets restrict only
JS-initiated `fetch`/XHR/dynamic-script loads. Parser-discovered `<script src>`
fan-out is bounded separately by `LoadOptions::max_document_script_loads` (excess
`<script src>` are neither fetched nor executed). Every transfer is size-bounded:
`ResourcePolicy::max_response_bytes` caps the body, and response headers are capped
cumulatively across the whole transfer (including `Set-Cookie` accumulated over
redirect hops, which libcurl resets per hop) so a server cannot move an unbounded
payload into headers.

### Script loading

`Page` scans the whole document for external `<script src>` and prefetches them
through `load_all`, while still executing parser-discovered classic scripts in
document order. Static `async`/`defer` attributes are recognized. Static
inline/external `type=module` scripts support relative imports and
`import.meta.url`. Static scripts execute only for standard JavaScript MIME types;
non-standard types stay inert until page code creates or mutates a classic
`<script>` through the DOM.

Dynamically inserted classic scripts load through the same `ResourceLoader`, run
with `document.currentScript`, dispatch `load`/`error`, and are marked as started
so moving them later does not re-run them. DOM-created classic scripts default to
async-like execution; explicit `script.async = false` uses an ordered FIFO queue.

### Render prefetch

Before each render, `Page` scans the document for the sub-resources litehtml will
fetch and warms a `CachingResourceLoader` with concurrent `Lenient` `load_all`
calls, so the layout's otherwise-synchronous, one-at-a-time requests hit a warm
cache instead of blocking on a network round-trip each. This turns the resource
phase from O(N·latency) into O(latency) (bounded by the per-host connection cap);
a failed prefetch never aborts the render — litehtml falls back to its
placeholder.

The cache persists across styled-document rebuilds for the lifetime of the
current document (reset when the document or loader identity changes). A
script-heavy page invalidates its styled document on every DOM mutation and
re-lays-out many times per load; the persistent cache fetches each resource once
instead of re-downloading on every rebuild. Two waves are issued: wave one covers
`<img src>`, `<link rel=stylesheet href>`, and `url(...)` background images in
inline `style=""` and `<style>` blocks (resolved against the document's effective
`<base href>`); wave two scans the just-fetched external stylesheets and
prefetches their `url(...)` background images (resolved against each stylesheet's
own URL). `@font-face src:` URLs are downloaded, decoded for WOFF/WOFF2 where
needed, and registered with Fontconfig/Pango for the render.

## Security model

`ResourcePolicy` is secure by default:

- **`block_private_hosts`** rejects loopback/private/link-local/cloud-metadata
  targets. It is enforced both on literal-IP URLs — including legacy
  decimal/octal/hex/short numeric IPv4 forms such as `http://2130706433/`,
  IPv4-mapped/-compatible, 6to4, and NAT64-embedded IPv6 forms, and trailing-dot
  FQDN forms of `localhost`/literal IPs — and, via a connect-time socket callback
  that runs on every platform, on the post-DNS address of each connection. This
  closes DNS-rebinding and redirect-to-internal SSRF. Connection reuse does not
  weaken it: the socket callback fires only when a new socket is opened, and
  reused connections were already validated.
- **Proxying is off by default** and the ambient
  `http_proxy`/`HTTPS_PROXY`/`ALL_PROXY` environment variables are ignored — a
  proxy would resolve and connect to the real target itself, hiding it from the
  socket guard. Set `ResourcePolicy::proxy` to opt into a specific proxy.
- **`allow_file_from_network`** is off, so a network-origin document cannot read
  local files. `file_root` optionally sandboxes `file://` reads
  (symlink/`..` escapes are rejected; only regular files within the size limit
  are served).
- The initial transfer and redirects are pinned to the allowed scheme set.

When `js_resource_load_policy` is `SameOriginOnly`, an http(s) target from a
document with no comparable network origin (a `data:`/`file:` or empty base)
fails closed and is blocked rather than silently allowed.

## Cookies

Each `Page` owns a page-scoped `CookieJar` shared by top-level document loads,
static and dynamic scripts, fetch/XHR, stylesheet/image/font loads, and render
prefetching. `document.cookie` reads and writes the same jar; `Set-Cookie`
updates it from real responses; `HttpOnly` cookies are sent on matching requests
but hidden from `document.cookie` and from the `fetch`/XHR response headers
exposed to script; fetch/XHR credentials control whether cookies are sent or
accepted.

Cookie matching implements `Domain`, `Path`, `Expires`, `Max-Age` (which takes
precedence over `Expires`, clamped to a 400-day maximum), `Secure`, and
`SameSite` behavior, with `SameSite=None` requiring `Secure`. A `Domain` that is
a public suffix is rejected, and a `Domain` on an IP-literal host must equal the
host; the same registrable-domain rule feeds the schemeful same-site check (the
full Public Suffix List via libpsl's built-in data, falling back to a bare
single-label rule when no PSL data is available). `__Host-`/`__Secure-` prefixes
are enforced case-insensitively, a non-secure origin cannot overwrite a `Secure`
cookie, names/values with control characters are rejected, and the jar is bounded
per-domain/globally with oldest-first eviction. Cookies are handed to libcurl's
host-scoped cookie engine rather than a raw header, so they are not replayed to a
different origin across a redirect.

## Rendering pipeline

Rendering is adapter-shaped. `include/pagecore/render.hpp` defines a
backend-neutral `DisplayList` boundary between layout engines and raster
backends.

`Page::display_list(RenderOptions)` feeds the JS-mutated Lexbor DOM to the
configured layout engine and returns backend-neutral commands. The result is
memoized: repeated `display_list`/`render` calls reuse the cached list while the
layout mutation version, viewport, scale, base URL, external-resource flag,
render resource mode, and absolute-%-corrected serialization mode are unchanged.
The cache is invalidated by layout-affecting DOM mutations, `load_html`, and
`set_resource_loader`/`set_layout_engine_factory`, but not by service-only DOM
mutations such as `data-*`/`aria-*` attributes unless current CSS references them
(via an attribute selector or `content: attr(...)`).

**The final render is always exact and never fed by any read-time cache.**
`display_list()` performs an exact `Full` layout at the requested
viewport/version.

litehtml sizes a `position:absolute; width:%` element's own box but does not make
it a definite-width containing block, so its percentage-width children collapse.
PageCore repairs this within a single render by reading each such element's
own litehtml-computed width from that same pass and pinning it as a transient
explicit `width:<N>px` on a second same-viewport/same-version pass (never mutating
`serialize_html()` output). It pins litehtml's own value — never a re-derived or
guessed width — so a correctly-sized element is untouched and only its collapsing
children are fixed. The correction is derived entirely within one render, so a
width measured during a synchronous JS read at one viewport can never leak into a
paint at another.

`Page::layout(RenderOptions)` returns a freshly built engine for callers that
need the engine itself.

### Direct-DOM layout tree

The litehtml tree is built directly from the Lexbor DOM by default
(`LoadOptions::layout_tree_input = LayoutTreeInput::DirectDom`).
`DomDocument::visit_layout_tree` walks the layout view of the DOM (the same
`<noscript>`/head-text rules as the serialized path, style overrides merged into
the reported `style` attribute, adjacent text nodes coalesced into single runs,
inert `<template>` subtrees skipped) and `LiteHtmlLayoutEngine::load_dom` builds
litehtml elements from it, running the exact finalization sequence the
HTML-parser path runs. This removes the serialize→re-parse round trip from every
styled-document rebuild and gives a direct `NodeId`→element map instead of
string-based identity. `LayoutTreeInput::SerializedHtml` keeps the round trip
available; engines without `load_dom` support fall back to it automatically.

A litehtml FetchContent patch
(`cmake/patches/0001-litehtml-external-tree-build.patch`, applied idempotently by
`cmake/apply_patch.cmake`) refactors litehtml's `createFromString` finalization
into a public `finalize_from_external_root()` shared by both paths, plus a
`set_document_mode()` setter so quirks mode is seeded before elements exist.

On top of the direct build, a styled-document cache miss caused only by
inline-style edits is satisfied in place instead of rebuilt: `DomDocument` keeps a
bounded journal (64 records) of layout-affecting mutations, and when every change
since the cached version is an inline `style` write that touches no
structure-affecting property (`display`/`position`/`float`/`content`/`direction`/
`list-style*`), no `[style...]` attribute selector is live, and every touched
element is still connected with a live render item,
`LayoutEngine::apply_inline_style_patches` restyles just those elements and the
cache key's version is promoted so the next `layout()` reuses the patched
document. Any other mutation, a journal overflow, the abs-% second pass, or the
serialized input falls back to a full rebuild. Parity tests byte-compare a
patched page against a freshly built page on the same DOM.

Because the direct path is the real DOM rather than a re-parse, a few documented
behaviors differ from the serialized round trip — matching real browsers:
(1) DOM shapes JavaScript can create but an HTML parser would normalize away (a
`<p>` inside a `<p>`, a `<div>` inside a `<table>`) reach layout as the real tree;
(2) a leading newline assigned into a `<pre>` via `textContent` is preserved.
Entities, quirks-mode class/id matching, `<template>` inertness, and SVG islands
are byte-identical across both inputs (locked by parity tests).

### Computed style

`getComputedStyle()` is backed by the same litehtml engine that produces the
rendered `DisplayList`, not a separate cascade. `Page::computed_style(NodeId)`
lazily builds (or reuses) a litehtml document keyed on the same
`(layout_mutation_version, viewport, base_url)` as the render cache, runs
litehtml's cascade without a full layout pass (`compute_styles_only()`), and reads
computed values through typed `litehtml::css_properties` getters mapped to CSSOM
strings. Single-property reads use `Page::computed_style_property(NodeId,
property)`.

Reuse is sound by construction: a cached value is reused for a node only when its
*layout-input digest* is unchanged — an allocation-free FNV-1a hash folding a
conservative superset of every DOM-derived cascade input (the node and each
ancestor's tag/id/class/inline-style/selector attributes, the ordered sibling
context and child-presence at every level, a global stylesheet generation, the
base URL, and the viewport). A digest miss only costs one extra exact rebuild; a
`class`/stylesheet-driven change is therefore always observed, not just
inline-style edits.

If repeated post-mutation reads prove that styled-document rebuilds are expensive,
the single-property path enters an explicitly approximate bounded mode: it stops
forcing rebuilds and returns an inline `style=""` value when present, then the
last known value, then conservative CSS defaults. Full `Page::computed_style()`
remains exact.

Element identity across the JS/C++ boundary uses a `data-pc-sid="<NodeId>"` marker
that exists only on the litehtml side: the direct-DOM builder sets it on the
litehtml elements it creates (the Lexbor DOM is never touched), and the serialized
fallback injects it transiently; it never bumps `mutation_version` and never
appears in rendered output. The CSSOM model (`CSSStyleDeclaration`,
`CSSStyleSheet`, `el.style`, `insertRule`/`deleteRule`, `document.styleSheets`) is
independent of this.

Returned values are litehtml's computed values, not used/layout values:
percentages and `auto` are returned as authored (`width:50%` stays `"50%"`).
Known gaps: `opacity` always reads `"1"` (litehtml applies it at draw time), and
CSS custom properties (`--*`/`var()`) are not resolved. Because litehtml's cascade
only sees the real DOM, three CSSOM-only states have no effect on
`getComputedStyle()` (as they have none on the rendered output): a stylesheet's
`disabled` flag, `document.adoptedStyleSheets`, and styling inside a shadow tree
(Shadow DOM here is a JS-only simulation whose nodes are never attached to the
Lexbor tree litehtml renders).

### Layout geometry

Layout geometry flows back to JS through the same litehtml engine and
`data-pc-sid` identity, but reads from a real `layout()` pass populating litehtml's
`render_item` tree. `Page::element_geometry(NodeId)` forces that layout (shared
with `display_list()`) and reads `render_item::get_placement()` (content-box)
expanded into padding-box and border-box. Forced JS geometry/computed-style
rebuilds use a stylesheet-only render resource mode: external stylesheets still
load so metrics see CSS, but images/fonts are skipped until the final render.

`getBoundingClientRect()`, `offsetWidth/offsetHeight`, `offsetTop/offsetLeft`,
`clientWidth/clientHeight`, and `clientTop/clientLeft` derive from this. A `null`
result (element is `display:none`, or `layout()` hasn't run) maps to all-zero
geometry.

Because litehtml has no incremental layout, each geometry read after a DOM
mutation rebuilds the whole styled document. To bound a tight read-modify-write
loop (e.g. jQuery init) on a large DOM, once building/laying-out the styled
document is observed to be expensive, later post-mutation geometry reads can reuse
the last exact value for a node instead of forcing more synchronous layouts. That
cache is versioned by layout mutation, is not reused after ancestor layout
mutations, can synthesize simple own inline `width`/`height` changes, and can
fast-adjust own inline `left`/`top` for `position:absolute|fixed`. Exact reads
before the threshold keep common JS layout libraries from treating initial probes
as zero-size. PageCore also resolves `position:absolute; width:<percent>;
box-sizing:border-box` against the positioned containing block before exposing
geometry to JS. Normal pages keep full geometry.

`window.innerWidth/innerHeight/outerWidth/outerHeight/devicePixelRatio` and
`screen.*` are live getters reading the most recent render viewport through a
layout-free bridge call.

Known geometry-channel limitations: `scrollWidth`/`scrollHeight` are approximated
as `clientWidth`/`clientHeight`; `scrollTop`/`scrollLeft` are no-op stubs;
`getClientRects()` returns a single bounding rect rather than one per line-box
fragment; `Range.getBoundingClientRect`/`getClientRects` return empty; and
`document.elementFromPoint`/`elementsFromPoint` do no coordinate-based
hit-testing (they return `null`/`[]` — present because libraries call them
without feature-detecting, but never a real point-to-element lookup).

## Raster backend

`Page::render(RenderOptions)` rasterizes the display list through the
Cairo/PangoCairo backend and returns RGBA pixels. Cairo owns 2D rasterization;
PangoCairo owns real font selection, shaping, and anti-aliased text drawing. A new
backend implements `RasterBackend` over the same `DisplayList` contract.

- **Images.** PNG, JPEG, WebP, GIF, and SVG images load through the shared
  `ResourceLoader`, decode into backend-neutral RGBA on `ImageCommand`, and are
  drawn by Cairo. The default `PAGECORE_IMAGE_DECODER=system` backend uses Cairo's
  stream decoder for PNG, TurboJPEG for JPEG, libwebp for WebP, giflib first-frame
  decoding for GIF, and [lunasvg](https://github.com/sammycage/lunasvg) for SVG.
  `PAGECORE_IMAGE_DECODER=stb` switches PNG/JPEG/GIF to the vendored
  `third_party/stb/stb_image.h`. WebP and SVG are independently optional
  (`PAGECORE_ENABLE_WEBP`, `PAGECORE_ENABLE_SVG`); decode failures or disabled
  formats remain non-fatal placeholders.
- **Backgrounds.** `background-size`, `background-position`, and
  `background-repeat` are resolved into backend-neutral image tile metadata, then
  rasterized by Cairo with clipping and repeat behavior.
- **Gradients.** CSS linear gradients are carried as `LinearGradientCommand`s.
- **Rounded corners.** `border-radius` is carried as corner radii on
  background/image/border/clip commands and used for rounded clipping.

`include/pagecore/image_io.hpp` provides dependency-free PNG encoding for
`RenderedImage`, and `write_pdf` exports the same display-list commands to a Cairo
PDF surface for vector output. `display_list_to_json` and
`pagecore_cli --dump-display-list` expose the render boundary for debugging.

## Extension points

The rendering boundary is designed for substitution. A future layout engine
provides a `LayoutEngineFactory` that produces the same `DisplayList` instead of
leaking its own types into the public API; a future raster backend (e.g. Skia)
implements `RasterBackend` over the same contract. Build with
`-DPAGECORE_ENABLE_RENDERING=OFF` for a DOM/JS-only or test-only core.
