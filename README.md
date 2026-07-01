# PageCore

Small modular web engine for headless automation, embedding, offscreen rendering, and future browser or GUI frontends.

## Architecture

- C++20/CMake core.
- Lexbor is the source of truth for HTML parsing, DOM storage, selectors, mutations, and serialization.
- QuickJS-NG runs page JavaScript.
- `dom_bridge` exposes primitive DOM operations to JS and keeps Lexbor as the only DOM source of truth.
- `src/dom_shim/` contains standalone `install(ctx, api)` JavaScript modules for the browser-like `window`, `document`, `Node`, `Element`, `HTMLElement`, events, DOM traversal, constraint validation, forms, timers, CSSOM, streams, fetch/XHR, and related shims. Each module declares explicit dependencies and exports; CMake concatenates them into `generated/dom_shim.js` at build time and embeds that generated artifact into the binary.
- DOM wrappers are hardened with a Lexbor-backed `mutationVersion`/`hasNode` contract so stale wrappers after destructive mutations fail cleanly instead of dereferencing removed nodes.
- Resources flow through `ResourceRequest`/`ResourceResponse` with request kinds (`Document`, `Script`, `Stylesheet`, `Image`, `Font`, `Other`), typed `ResourceError`s, `ResourcePolicy`, and an optional bounded, thread-safe `CachingResourceLoader` decorator (LRU-capped; error responses are not cached).
- `CurlResourceLoader` reuses a libcurl share handle (`CURLSH`) across requests, so connections, DNS, and TLS sessions are kept alive between sub-resource loads instead of re-handshaking per request. Connection reuse does not weaken the SSRF guard: the post-DNS socket callback fires only when a new socket is opened, and reused connections were already validated.
- `ResourceLoader::load_all` loads a batch of resources in request order. In the default `FailFast` mode it throws the first request-order error like `load`; in `Lenient` mode it never throws and returns a status-0 placeholder for each failed request (for non-critical sub-resources). The default implementation is serial; `CurlResourceLoader` overrides it to fetch concurrently with a single-threaded `curl_multi` driver (bounded to browser-like 6 connections per host), and `CachingResourceLoader` serves hits then batches only the misses. `Page` scans the whole document for external `<script src>` and prefetches them through `load_all`, while still executing them in document order.
- Before each render, `Page` scans the document for the sub-resources litehtml will fetch and warms a `CachingResourceLoader` with concurrent `Lenient` `load_all` calls so the layout's otherwise-synchronous, one-at-a-time requests hit the warm cache instead of blocking on a network round-trip each. This turns the resource phase from O(N·latency) into O(latency) (bounded by the per-host connection cap); a failed prefetch never aborts the render — litehtml falls back to its placeholder. The cache persists across styled-document rebuilds for the lifetime of the current document (reset when the document or loader identity changes): a script-heavy page invalidates its styled document on every DOM mutation, so it rebuilds and re-lays-out the litehtml document many times during a single load, and each layout re-requests the page's images and stylesheets — a per-rebuild cache would re-download all of them every time, while the persistent one fetches each resource once. Two waves are issued: wave one covers `<img src>`, `<link rel=stylesheet href>`, and `url(...)` background images in inline `style=""` attributes and `<style>` blocks (resolved against the document's effective `<base href>`); wave two scans the just-fetched external stylesheets and prefetches their `url(...)` background images (resolved against each stylesheet's own URL, exactly as the container does). `@font-face src:` URLs are intentionally skipped — text is rendered with system fonts through PangoCairo, so the engine never downloads web fonts.
- `ResourcePolicy` is secure by default: `block_private_hosts` rejects loopback/private/link-local/cloud-metadata targets (enforced on literal-IP URLs and, via a connect-time socket callback, on the post-DNS address, closing DNS-rebinding and redirect-to-internal SSRF), `allow_file_from_network` is off so a network-origin document cannot read local files, and `file_root` optionally sandboxes `file://` reads (symlink/`..` escapes are rejected; only regular files within the size limit are served). The initial transfer and redirects are pinned to the allowed scheme set.
- The DOM shim removes the raw native `__dom`/`__host` bridges from the page-visible global after install, so page script must go through the wrapper layer instead of the C++ API directly. `crypto.getRandomValues`/`randomUUID` are backed by the host OS CSPRNG, not `Math.random`.
- `Page`, JS script loading, litehtml stylesheet imports, and image placeholders use the same resource pipeline.
- Rendering is adapter-shaped. `include/pagecore/render.hpp` defines a backend-neutral `DisplayList` boundary between layout engines and raster backends.
- `Page::display_list(RenderOptions)` serializes the JS-mutated Lexbor DOM, runs the configured layout engine, and returns backend-neutral commands. The result is memoized: repeated `display_list`/`render` calls reuse the cached list while the document mutation version, viewport, scale, base URL, and external-resource flag are unchanged (the cache is invalidated by DOM mutations, `load_html`, and `set_resource_loader`/`set_layout_engine_factory`, but not by external resource bytes changing under a fixed DOM). `Page::layout(RenderOptions)` still returns a freshly built engine for callers that need the engine itself.
- `getComputedStyle()` is backed by the same litehtml engine that produces the rendered `DisplayList`, not a separate cascade: `Page::computed_style(NodeId)` lazily builds (or reuses) a litehtml document keyed on the same `(mutation_version, viewport, base_url)` as the render cache, runs litehtml's cascade without a full layout pass (`compute_styles_only()`), and reads back computed values through typed `litehtml::css_properties` getters mapped to CSSOM strings (`LayoutEngine::computed_style`, `src/litehtml_layout.cpp`). Element identity across the JS/C++ boundary uses a transient `data-pc-sid="<NodeId>"` attribute injected only while building this document (`DomDocument::serialize_html_for_layout`); it never bumps `mutation_version` and never appears in the rendered output. The JS side (`computedStyleFor` in `src/dom_shim/30_dom.js`) calls `__dom.computedStyle(element.__id)` and wraps the result in the existing read-only `CSSStyleDeclaration` proxy — the CSSOM model itself (`CSSStyleDeclaration`, `CSSStyleSheet`, `CSSStyleRule`, `el.style`, `insertRule`/`deleteRule`, `document.styleSheets`) is independent of this and unaffected.
- Values returned by `getComputedStyle()` are litehtml's computed values, not used/layout values: percentages and `auto` are returned as authored (e.g. `width:50%` stays `"50%"`), not resolved to pixels. Known gaps: `opacity` always reads `"1"` (litehtml applies it at draw time, not as a stored property) and CSS custom properties (`--*`/`var()`) are not resolved. Because litehtml's cascade only ever sees the real DOM, three CSSOM-only states have no effect on `getComputedStyle()` (the same as they have no effect on the actual rendered output): a stylesheet's `disabled` flag, `document.adoptedStyleSheets`, and any styling inside a shadow tree (Shadow DOM here is a JS-only simulation — its nodes are never attached to the Lexbor tree litehtml renders from).
- Layout geometry flows back to JS through the same litehtml engine and `data-pc-sid` identity used by `getComputedStyle()`, but reads from a real `layout()` pass (`document_->render()`, populating litehtml's `render_item` tree) instead of just the cascade: `Page::element_geometry(NodeId)` forces that layout (shared `Page::Impl::ensure_layout`, also used by `display_list()`, so a render afterwards reuses the same pass) and `LayoutEngine::element_geometry` reads `render_item::get_placement()` (content-box) expanded by `get_paddings()`/`get_borders()` into padding-box and border-box (`litehtml::position::operator+=`, the same expansion `html_tag.cpp` uses for paint). `getBoundingClientRect()`, `offsetWidth/offsetHeight`, `offsetTop/offsetLeft`, `clientWidth/clientHeight`, and `clientTop/clientLeft` (`src/dom_shim/30_dom.js`) are derived from this; a `null` result (element is `display:none`, which never gets a `render_item`, or `layout()` hasn't run) maps to all-zero geometry, same as before this channel existed. Because litehtml has no incremental layout against the Lexbor tree, each geometry read that follows a DOM mutation rebuilds the whole styled document and re-runs layout, so a script that reads geometry in a tight read-modify-write loop (e.g. jQuery init) on a large DOM would otherwise spend seconds per read. To bound this, once `Page::Impl` observes that building/laying-out the page's styled document is expensive (a single rebuild or layout exceeds a small threshold), it stops forcing synchronous layout for geometry reads and returns `nullopt` (geometry → 0, the pre-geometry-channel value) for the rest of the load; detection piggybacks on work `getComputedStyle()`/the render already do, so heavy pages pay nothing extra for geometry while normal pages keep full geometry. `window.innerWidth/innerHeight/outerWidth/outerHeight/devicePixelRatio` and `screen.*` are live getters reading `Page::Impl::last_render_options.viewport` through a separate, layout-free bridge call, so they reflect whatever viewport the page was most recently rendered with instead of a value fixed at JS install time.
- Known geometry-channel limitations: `scrollWidth`/`scrollHeight` are approximated as `clientWidth`/`clientHeight` (litehtml's real scrolled-content size is a protected field with no public getter), so they're wrong for `overflow:scroll|auto` content with actual overflow; `scrollTop`/`scrollLeft` are no-op stubs (read 0, write does nothing) — there is no write channel from JS scroll state back into litehtml layout; `getClientRects()` returns a single bounding rect rather than one rect per line-box fragment for wrapped inline elements; `Range.getBoundingClientRect`/`getClientRects` and `Document.elementFromPoint` are not implemented; and `IntersectionObserver`/`ResizeObserver` don't actually fire (no observation loop), unlike the one-shot geometry reads above.
- `Page::render(RenderOptions)` rasterizes that display list through the Cairo/PangoCairo backend and returns RGBA pixels. Cairo owns 2D rasterization; PangoCairo owns real font selection, shaping, and anti-aliased text drawing. Future Skia backends should implement `RasterBackend` over the same `DisplayList` contract.
- PNG, JPEG, WebP, GIF, and SVG images are loaded through the shared `ResourceLoader`, decoded into backend-neutral RGBA, carried on `ImageCommand`, and drawn by the Cairo raster backend. PNG uses Cairo's stream decoder, JPEG uses TurboJPEG, WebP uses libwebp, GIF uses giflib first-frame decoding, and SVG uses the built-in Cairo-based raster subset for common primitives and paths. Decode failures remain non-fatal placeholders.
- CSS `background-size`, `background-position`, and `background-repeat` are resolved by the litehtml adapter into backend-neutral image tile metadata, then rasterized by Cairo with clipping and repeat-x/repeat-y/repeat/no-repeat behavior.
- CSS linear gradients are carried as backend-neutral `LinearGradientCommand`s and rasterized by Cairo.
- CSS `border-radius` is carried as backend-neutral corner radii on background fill/image/border/clip commands. Cairo uses it for rounded clipping of solid backgrounds, background images, and pushed clip regions.
- `display_list_to_json` and `pagecore_cli --dump-display-list` expose the backend-neutral render boundary for debugging layout/raster issues.
- `include/pagecore/image_io.hpp` provides dependency-free PNG encoding for `RenderedImage`, and `write_pdf` exports the same display-list commands to a Cairo PDF surface for vector output.
- Top-level CLI builds enable rendering by default through the current litehtml-backed `LayoutEngineFactory`; future Blitz/Servo-like adapters should provide another factory that produces the same `DisplayList` instead of leaking their own types into the public API. Use `-DPAGECORE_ENABLE_RENDERING=OFF` for a DOM/JS-only or test-only build.

## Build

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

If the build host cannot resolve GitHub during `FetchContent`, pre-clone the dependencies and point CMake at them:

```sh
cmake -S . -B build \
  -DFETCHCONTENT_SOURCE_DIR_LEXBOR=/path/to/lexbor \
  -DFETCHCONTENT_SOURCE_DIR_QUICKJS_NG=/path/to/quickjs \
  -DFETCHCONTENT_SOURCE_DIR_LITEHTML=/path/to/litehtml
```

FetchContent pins:

- Lexbor `f1ba992ba33e7cf7d39936a9fd43444d7b31b600`
- QuickJS-NG `377a25e0e646356670eef3d3f03d9c4839b23d6d`
- litehtml `932439c91afb04dbce30903673292e3bf2da01dc` when `PAGECORE_ENABLE_RENDERING=ON`

System dependencies:

- `cairo`
- `pangocairo`
- `libturbojpeg`
- `libwebp`
- `libwebpdecoder`
- `giflib`

CMake uses pkg-config for these system libraries and links static archives by default when an advertised `-lfoo` can be resolved to `libfoo.a` in the pkg-config library search paths. Libraries without an available `.a` fall back to the normal linker token, so the build remains usable on hosts where a dependency is only available as a shared/system library.

Top-level builds enable rendering by default because `PAGECORE_BUILD_TOOLS=ON`
creates a full `pagecore_cli`. For a smaller DOM/JS-only or test-only build,
disable tools or rendering explicitly:

```sh
cmake -S . -B build-dom-only \
  -DPAGECORE_BUILD_TOOLS=OFF \
  -DPAGECORE_ENABLE_RENDERING=OFF
cmake --build build-dom-only
ctest --test-dir build-dom-only --output-on-failure
```

Size-optimized release build:

```sh
cmake -S . -B build-size \
  -DCMAKE_BUILD_TYPE=MinSizeRel \
  -DPAGECORE_OPTIMIZE_SIZE=ON
cmake --build build-size --target pagecore_cli
```

`PAGECORE_OPTIMIZE_SIZE=ON` enables function/data sections, linker dead-code elimination (`-dead_strip` on Darwin or `--gc-sections` on ELF linkers), IPO/LTO when CMake reports compiler support, and post-build stripping for `Release`/`MinSizeRel` executables. If symbols are needed for diagnostics, add `-DPAGECORE_STRIP_RELEASE_BINARIES=OFF`.

To embed a minified DOM shim, install either `terser` or `esbuild` in `PATH` and configure with:

```sh
cmake -S . -B build-size \
  -DPAGECORE_MINIFY_DOM_SHIM=ON
```

When enabled, CMake still writes the readable concatenated shim to `generated/dom_shim.js`, then embeds `generated/dom_shim.min.js`.

By default the concatenated shim has its comments stripped before embedding (via a tokenizer-aware `cmake/strip_js_comments.js` run with `node`, which preserves strings and regex literals). This requires `node`; when it is unavailable the shim is embedded with comments intact. Disable with `-DPAGECORE_STRIP_DOM_SHIM_COMMENTS=OFF`.

If `node` is available, `pagecore_dom_shim_check` and the `pagecore_dom_shim_modules` CTest verify that each DOM shim module can be syntax-checked, executed standalone, and loaded as a module definition with the expected dependency contract. The `pagecore_dom_shim_unit_tests` target and CTest run focused Node tests from `tests/dom_shim/` for core URL/resource helpers, event dispatch/abort behavior, DOMException constants, DOM traversal, form validation, FormData, web utility shims, timers, streams, compat helpers, and runtime dependency errors. Worker and ServiceWorker surfaces are intentionally explicit `NotSupportedError` paths until PageCore has a real worker runtime.

## CLI

```sh
build/pagecore_cli --html '<html><body><div id="x"></div><script>document.getElementById("x").textContent="ok"</script></body></html>'
build/pagecore_cli --url https://example.com
build/pagecore_cli --file ./page.html --eval 'document.body.textContent'
```

PNG and vector PDF output are available in the default top-level build:

```sh
build/pagecore_cli \
  --html '<html><body><div style="width:80px;height:40px;background:#ff0000"></div></body></html>' \
  --format png \
  --output /tmp/pagecore-shot.png \
  --viewport 320x240 \
  --scale 1

build/pagecore_cli \
  --file ./page.html \
  --format pdf \
  --output /tmp/pagecore-page.pdf \
  --viewport 320x240 \
  --scale 1
```

`--full-page` expands the viewport height to the page's actual content height before rendering, so the PNG/PDF/display-list output covers the whole page instead of being cropped to `--viewport`'s height (the requested width is kept as-is, since layout reflows on width, not height):

```sh
build/pagecore_cli \
  --file ./page.html \
  --format png \
  --output /tmp/pagecore-fullpage.png \
  --viewport 1280x720 \
  --full-page
```

Display-list debugging is available in the default top-level build:

```sh
build/pagecore_cli \
  --file ./page.html \
  --dump-display-list /tmp/pagecore-display-list.json \
  --viewport 1280x720
```

## Embedding

The project also builds a C++ library target. When used as a CMake
subdirectory, CLI tools, tests, and examples are disabled by default for the
consumer project:

```cmake
add_subdirectory(path/to/pagecore)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE PageCore::pagecore)
```

Build the standalone embedding example with:

```sh
cmake -S . -B build-embed -DPAGECORE_BUILD_EXAMPLES=ON
cmake --build build-embed --target pagecore_embed_minimal
build-embed/pagecore_embed_minimal
```

To install and consume the package with `find_package`:

```sh
cmake -S . -B build-install -DCMAKE_INSTALL_PREFIX=/tmp/pagecore-install
cmake --build build-install --target install
```

```cmake
find_package(PageCore CONFIG REQUIRED)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE PageCore::pagecore)
```

The `examples/` directory also contains a standalone CMake project that uses
`find_package(PageCore CONFIG REQUIRED)` and links the installed package.

The public C++ API is in `include/pagecore/`. `pagecore::Page` is the main
entry point, `ResourceLoader` controls network/file access, and
`render.hpp` exposes the backend-neutral `DisplayList`, `LayoutEngine`, and
`RasterBackend` boundaries. See `examples/embed_minimal.cpp` for a minimal
in-memory page load with JavaScript execution.
