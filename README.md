# PageCore

Small modular web engine for headless automation, embedding, offscreen rendering, and future browser or GUI frontends.

## Architecture

- C++20/CMake core.
- Lexbor is the source of truth for HTML parsing, DOM storage, selectors, mutations, and serialization.
- QuickJS-NG runs page JavaScript.
- `dom_bridge` exposes primitive DOM operations to JS and keeps Lexbor as the only DOM source of truth.
- `src/dom_shim/` contains standalone `install(ctx, api)` JavaScript modules for the browser-like `window`, `document`, `Node`, `Element`, `HTMLElement`, events, DOM traversal, constraint validation, forms, timers, CSSOM, streams, fetch/XHR, and related shims. Each module declares explicit dependencies and exports; CMake concatenates them into `generated/dom_shim.js` at build time and embeds that generated artifact into the binary.
- DOM wrappers are hardened with a Lexbor-backed `mutationVersion`/`hasNode` contract so stale wrappers after destructive mutations fail cleanly instead of dereferencing removed nodes.
- Resources flow through `ResourceRequest`/`ResourceResponse` with request kinds (`Document`, `Script`, `Stylesheet`, `Image`, `Font`, `Other`), typed `ResourceError`s, `ResourcePolicy`, and an optional `CachingResourceLoader` decorator.
- `Page`, JS script loading, litehtml stylesheet imports, and image placeholders use the same resource pipeline.
- Rendering is adapter-shaped. `include/pagecore/render.hpp` defines a backend-neutral `DisplayList` boundary between layout engines and raster backends.
- `Page::display_list(RenderOptions)` serializes the JS-mutated Lexbor DOM, runs the configured layout engine, and returns backend-neutral commands.
- `Page::render(RenderOptions)` rasterizes that display list through the Cairo/PangoCairo backend and returns RGBA pixels. Cairo owns 2D rasterization; PangoCairo owns real font selection, shaping, and anti-aliased text drawing. Future Skia backends should implement `RasterBackend` over the same `DisplayList` contract.
- PNG, JPEG, WebP, GIF, and SVG images are loaded through the shared `ResourceLoader`, decoded into backend-neutral RGBA, carried on `ImageCommand`, and drawn by the Cairo raster backend. PNG uses Cairo's stream decoder, JPEG uses TurboJPEG, WebP uses libwebp, GIF uses giflib first-frame decoding, and SVG uses the built-in Cairo-based raster subset for common primitives and paths. Decode failures remain non-fatal placeholders.
- CSS `background-size`, `background-position`, and `background-repeat` are resolved by the litehtml adapter into backend-neutral image tile metadata, then rasterized by Cairo with clipping and repeat-x/repeat-y/repeat/no-repeat behavior.
- CSS linear gradients are carried as backend-neutral `LinearGradientCommand`s and rasterized by Cairo.
- CSS `border-radius` is carried as backend-neutral corner radii on background fill/image/border/clip commands. Cairo uses it for rounded clipping of solid backgrounds, background images, and pushed clip regions.
- `display_list_to_json` and `pagecore_cli --dump-display-list` expose the backend-neutral render boundary for debugging layout/raster issues.
- `include/pagecore/image_io.hpp` provides dependency-free PNG encoding for `RenderedImage`, so screenshots can be written without coupling image output to a specific layout or raster backend.
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

If `node` is available, `pagecore_dom_shim_check` and the `pagecore_dom_shim_modules` CTest verify that each DOM shim module can be syntax-checked, executed standalone, and loaded as a module definition with the expected dependency contract. The `pagecore_dom_shim_unit_tests` target and CTest run focused Node tests from `tests/dom_shim/` for core URL/resource helpers, event dispatch/abort behavior, DOMException constants, DOM traversal, form validation, FormData, web utility shims, timers, streams, compat helpers, and runtime dependency errors. Worker and ServiceWorker surfaces are intentionally explicit `NotSupportedError` paths until PageCore has a real worker runtime.

## CLI

```sh
build/pagecore_cli --html '<html><body><div id="x"></div><script>document.getElementById("x").textContent="ok"</script></body></html>'
build/pagecore_cli --url https://example.com
build/pagecore_cli --file ./page.html --eval 'document.body.textContent'
```

Screenshot output is available in the default top-level build:

```sh
build/pagecore_cli \
  --html '<html><body><div style="width:80px;height:40px;background:#ff0000"></div></body></html>' \
  --screenshot /tmp/pagecore-shot.png \
  --viewport 320x240 \
  --scale 1
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
