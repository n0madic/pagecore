# PageCore

PageCore is a small, embeddable web engine for headless automation, offscreen
rendering, and building browser- or GUI-like frontends. It loads HTML, runs the
page's JavaScript in a browser-like environment, and renders the result to an
image or PDF — all in-process, with no browser to drive.

It is a C++20 library with a command-line tool. Use it to take screenshots,
export PDFs, scrape rendered pages, run page scripts, or read computed styles and
layout geometry, without shipping a full browser.

## Features

- **Real HTML + DOM.** Full HTML parsing and a live DOM (backed by Lexbor) with
  querying, mutation, and serialization.
- **JavaScript execution.** Page scripts run on QuickJS-NG in a browser-like
  environment: `window`, `document`, `Node`/`Element`/`HTMLElement`, events,
  timers, forms and constraint validation, `fetch`/XHR, `URL`/`URLSearchParams`,
  CSSOM, streams, and `crypto` backed by the OS CSPRNG. Classic and ES-module
  scripts are supported.
- **Rendering to PNG and PDF.** CSS layout via litehtml is rasterized through
  Cairo/PangoCairo with real font selection, shaping, and anti-aliased text.
  Supports background images (size/position/repeat), linear gradients,
  `border-radius`, and `@font-face` web fonts (WOFF/WOFF2).
- **Images.** PNG, JPEG, WebP, GIF, and SVG (via lunasvg, covering nearly all of
  SVG 1.1/1.2 Tiny — gradients, patterns, clipping, masking, `<use>`/`<defs>`)
  decode into the render; decoders are pluggable and individually optional.
- **Style and layout read-back.** `getComputedStyle()`,
  `getBoundingClientRect()`, `offsetWidth`/`offsetHeight`, and related geometry
  are backed by the same engine that produces the render, so scripts see
  consistent values.
- **Cookies.** A page-scoped cookie jar with `Domain`/`Path`/`Expires`/`Max-Age`/
  `Secure`/`SameSite` handling, `HttpOnly` and `__Host-`/`__Secure-` support,
  shared across document loads, scripts, and `fetch`/XHR.
- **Secure by default.** Built-in SSRF protection blocks requests to
  loopback/private/link-local/cloud-metadata addresses (including DNS-rebinding
  and redirect-to-internal), a `file://` sandbox, referrer sanitization, and
  outgoing-header injection defenses.
- **Controllable resource loading.** A pluggable `ResourceLoader` with caching,
  concurrent fetching, per-policy access control, and byte/count/time budgets for
  script-initiated loads.
- **Page-readiness control.** Wait for `load`, network idle, a stable DOM, or a
  full `ready` state before rendering or reading results.
- **Embeddable.** A clean C++ API and a backend-neutral `DisplayList` so the
  layout engine and raster backend can be replaced.

Some browser APIs are intentionally absent (e.g. `Worker`,
`IntersectionObserver`, `ResizeObserver`) so feature detection falls back
cleanly instead of hitting broken stubs. See
[docs/browser-api-support.md](docs/browser-api-support.md) for per-API status,
and [docs/architecture.md](docs/architecture.md) for how the engine works
internally.

## Quick start

Build the engine and the CLI, then render a page:

```sh
cmake -S . -B build
cmake --build build

# Run a script and print a DOM value
build/pagecore_cli --html '<html><body><div id="x"></div><script>document.getElementById("x").textContent="ok"</script></body></html>' \
  --eval 'document.getElementById("x").textContent'

# Take a screenshot
build/pagecore_cli --url https://example.com \
  --format png --output /tmp/example.png --viewport 1280x720
```

### Dependencies

Core:

- libcurl 7.64.0 or newer
- zlib

Rendering (enabled by default):

- `cairo`, `pangocairo`, `pangoft2`, `fontconfig`, `libbrotlidec`
- `libturbojpeg` and `giflib` for the default image decoder
- `libwebp` and `libwebpdecoder` for WebP (default on)
- [lunasvg](https://github.com/sammycage/lunasvg) (with its vendored
  [plutovg](https://github.com/sammycage/plutovg) backend) for SVG rasterization
  (default on) — fetched and built via CMake, no extra host toolchain required

CMake resolves these through pkg-config and links static archives when a `.a` is
available, falling back to shared libraries otherwise. Dependencies fetched from
GitHub (Lexbor, QuickJS-NG, litehtml, woff2, lunasvg) are pinned to specific
commits; if the build host cannot reach GitHub, pre-clone them and point CMake at
the copies:

```sh
cmake -S . -B build \
  -DFETCHCONTENT_SOURCE_DIR_LEXBOR=/path/to/lexbor \
  -DFETCHCONTENT_SOURCE_DIR_QUICKJS_NG=/path/to/quickjs \
  -DFETCHCONTENT_SOURCE_DIR_LITEHTML=/path/to/litehtml \
  -DFETCHCONTENT_SOURCE_DIR_WOFF2=/path/to/woff2 \
  -DFETCHCONTENT_SOURCE_DIR_LUNASVG=/path/to/lunasvg
```

### Build options

| Option | Default | Effect |
| --- | --- | --- |
| `PAGECORE_ENABLE_RENDERING` | `ON` | Layout + raster backend. Turn off for a DOM/JS-only core. |
| `PAGECORE_BUILD_TOOLS` | `ON` | Build the `pagecore_cli` tool. |
| `PAGECORE_IMAGE_DECODER` | `system` | `system` (Cairo/TurboJPEG/giflib) or `stb` (vendored, no TurboJPEG/giflib dependency). |
| `PAGECORE_ENABLE_WEBP` | `ON` | WebP decoding (requires libwebp). |
| `PAGECORE_ENABLE_SVG` | `ON` | SVG rasterization via lunasvg. |
| `PAGECORE_OPTIMIZE_SIZE` | `OFF` | Dead-code elimination, LTO, and binary stripping for release builds. |
| `PAGECORE_MINIFY_DOM_SHIM` | `OFF` | Embed a minified DOM shim (needs `terser` or `esbuild`). |
| `PAGECORE_INSTALL` | `ON` | Generate install/export rules (see note below). |

A DOM/JS-only build (smallest, no rendering dependencies):

```sh
cmake -S . -B build-dom-only -DPAGECORE_BUILD_TOOLS=OFF -DPAGECORE_ENABLE_RENDERING=OFF
cmake --build build-dom-only
```

A minimal image-decoder rendering build:

```sh
cmake -S . -B build-minimal \
  -DPAGECORE_IMAGE_DECODER=stb \
  -DPAGECORE_ENABLE_WEBP=OFF \
  -DPAGECORE_ENABLE_SVG=OFF
```

A size-optimized release build (about 5 MB stripped `pagecore_cli`):

```sh
cmake -S . -B build-size -DCMAKE_BUILD_TYPE=MinSizeRel -DPAGECORE_OPTIMIZE_SIZE=ON
cmake --build build-size --target pagecore_cli
```

## Command-line usage

The input can be inline HTML, a URL, a local file, or stdin. Output defaults to
serialized HTML; use `--format png|pdf` (or `--screenshot PATH`) to render.

```sh
build/pagecore_cli --html '<h1>Hi</h1>'
build/pagecore_cli --url https://example.com
build/pagecore_cli --file ./page.html --eval 'document.body.textContent'
echo '<h1>Hi</h1>' | build/pagecore_cli --stdin
```

### Rendering

```sh
build/pagecore_cli --file ./page.html \
  --format png --output /tmp/shot.png \
  --viewport 320x240 --scale 1

build/pagecore_cli --file ./page.html \
  --format pdf --output /tmp/page.pdf \
  --viewport 320x240
```

`--full-page` keeps the layout viewport from `--viewport` and expands only the
output canvas to the full content height, matching browser full-page screenshot
semantics (viewport units, fixed-position elements, and JS geometry still see the
requested viewport):

```sh
build/pagecore_cli --file ./page.html \
  --format png --output /tmp/fullpage.png \
  --viewport 1280x720 --full-page
```

### Waiting for the page

Page readiness is controlled by `--wait-until load|network-idle|dom-stable|ready`
(default `ready`), `--wait-ms` (overall hard budget), and `--stable-window-ms`.
The `ready` default waits for scripts and lifecycle events, JS-initiated
`fetch`/XHR, dynamic scripts, image/stylesheet load events, drained
microtasks/MutationObserver delivery, and a quiet DOM-mutation window.

### Restricting script-initiated loads

`--js-resource-policy allow|same-origin|none` controls only resource loads
started by page JavaScript (`allow` is the default; `same-origin` blocks
cross-origin `fetch`/XHR/dynamic scripts; `none` blocks all such loads).
Top-level document loading and parser-discovered scripts are unaffected. These
loads can also be capped with `--max-js-resource-loads`,
`--max-js-resource-bytes`, and `--max-js-resource-time-ms`; render-time
sub-resources have their own `--max-render-resource-*` budgets.

### Debugging and profiling

Dump the backend-neutral display list to inspect layout/raster output:

```sh
build/pagecore_cli --file ./page.html \
  --dump-display-list /tmp/display-list.json --viewport 1280x720
```

Enable performance tracing with `--perf-trace PATH` (newline-delimited JSON; use
`-` for stderr). Each event looks like:

```json
{"phase":"litehtml_layout","name":"layout","elapsed_us":21438,"count":3211}
```

Phases cover parsing, subresource scanning, scripts, DOM bridge calls, resource
loads, litehtml load/layout, computed style, geometry, raster, and PNG encode.
Use `tools/perf_trace_summary.js /tmp/trace.jsonl` to print phase totals and the
slowest resource events.

## Embedding

PageCore builds a C++ library target. As a CMake subdirectory, CLI tools, tests,
and examples are disabled by default for the consumer:

```cmake
add_subdirectory(path/to/pagecore)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE PageCore::pagecore)
```

The public API is in `include/pagecore/`. `pagecore::Page` is the main entry
point, `ResourceLoader` controls network/file access, and `render.hpp` exposes
the backend-neutral `DisplayList`, `LayoutEngine`, and `RasterBackend`
boundaries. See `examples/embed_minimal.cpp` for a minimal in-memory page load
with JavaScript execution.

Build the standalone example:

```sh
cmake -S . -B build-embed -DPAGECORE_BUILD_EXAMPLES=ON
cmake --build build-embed --target pagecore_embed_minimal
build-embed/pagecore_embed_minimal
```

### Performance callback

Embedders receive the same performance events through
`pagecore::PerfTraceCallback` (`include/pagecore/perf.hpp`):

```cpp
pagecore::PerfTraceCallback perf_trace = [](const pagecore::PerfEvent& event) {
    std::cerr << pagecore::perf_phase_name(event.phase)
              << " " << event.elapsed_us << "us count=" << event.count << "\n";
};

pagecore::LoadOptions load_options;
load_options.perf_trace = perf_trace;

pagecore::RenderOptions render_options;
render_options.perf_trace = perf_trace;
auto image = page.render(render_options);
pagecore::write_png_rgba(image, "/tmp/shot.png", perf_trace);
```

`LoadOptions::perf_trace` observes load-time work and is the fallback sink for
later render/style/geometry work; set `RenderOptions::perf_trace` to route a
specific `render()`/`display_list()` to a different sink.

### Installing as a package

```sh
cmake -S . -B build-install -DCMAKE_INSTALL_PREFIX=/tmp/pagecore-install
cmake --build build-install --target install
```

```cmake
find_package(PageCore CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE PageCore::pagecore)
```

> **Note:** the installed/exported package is non-relocatable. Its static-first
> dependency targets bake this build host's absolute library paths into the
> exported CMake config, so a `find_package(PageCore)` consumer must run on the
> same host and layout that built it. Set `-DPAGECORE_INSTALL=OFF` to skip
> generating install/export rules. The `examples/` directory contains a
> standalone project that consumes the installed package.

## Documentation

- [docs/architecture.md](docs/architecture.md) — how the engine works
  internally (DOM, JS runtime, resource pipeline, security, rendering).
- [docs/browser-api-support.md](docs/browser-api-support.md) — per-API
  `supported`/`partial`/`absent` status.
- [docs/testing.md](docs/testing.md) — CTest, WPT subset, and display-list
  regression infrastructure.
