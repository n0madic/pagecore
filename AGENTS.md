# Repository Guidelines

## Project Structure & Module Organization

This is a C++20/CMake modular web engine for headless automation, embedding, offscreen rendering, and browser- or GUI-like frontends. Public APIs live in `include/pagecore/`. Core implementation lives in `src/`, including DOM/JS bridge code, resource loading, image decoding, display-list dumping, perf tracing, and Cairo rasterization. `src/dom_shim/` contains standalone DOM shim modules; CMake concatenates them into `generated/dom_shim.js` and embeds that generated artifact into the binary. CLI entry points are in `tools/`. C++ tests are centralized in `tests/pagecore_tests.cpp`; JavaScript DOM-shim unit tests live in `tests/dom_shim/dom_shim_unit_tests.js`, and `tools/check_dom_shim_modules.js` validates each shim module's dependency contract and detects dependency cycles. Contributor documentation is in `docs/`: `architecture.md` covers engine internals (DOM, JS runtime, the libuv event loop, resource pipeline, security, rendering) and `browser-api-support.md` tracks per-API status. Visual regression fixtures and generated reference artifacts are in `examples/visual-regression/`.

## Build, Test, and Development Commands

Use an out-of-tree CMake build:

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

For a smaller DOM/JS-only test build, configure with `-DPAGECORE_BUILD_TOOLS=OFF -DPAGECORE_ENABLE_RENDERING=OFF`. The DOM-shim JavaScript checks (`pagecore_dom_shim_modules` and `pagecore_dom_shim_unit_tests` CTests) require `node` on `PATH`; without it the shim is still embedded but those checks are skipped.

Useful CLI examples:

```sh
build/pagecore_cli --file examples/visual-regression/index.html --screenshot /tmp/shot.png --viewport 1280x1000
build/pagecore_cli --file page.html --dump-display-list /tmp/display-list.json
build/pagecore_cli --file page.html --format png --output /tmp/shot.png --perf-trace /tmp/pagecore-trace.jsonl
```

## Coding Style & Naming Conventions

Follow the existing C++ style: 4-space indentation, braces on their own lines for functions/classes, `snake_case` for functions and local variables, and `PascalCase` for public structs/classes such as `DisplayList` or `ResourceLoader`. Prefer small, focused helpers in anonymous namespaces. Keep public types backend-neutral; layout adapters and raster backends should communicate through `include/pagecore/render.hpp`.

## Testing Guidelines

Use the existing CTest target and add focused C++ tests to `tests/pagecore_tests.cpp`, named with the `test_<area>_<behavior>` pattern and registered in the test runner. Every bug fix should include a regression test that fails without the fix. Changes to the DOM shim's page-visible behavior should add Node tests to `tests/dom_shim/dom_shim_unit_tests.js` when they can be exercised against the mock bridge, or a C++ `Page`-driven test when they need the real DOM/render path. Rendering-sensitive changes should include pixel-invariant checks rather than brittle full-image comparisons. Timing-coupled tests must be written as must-exceed lower bounds (a sleep only ever raises elapsed time) so a contended CI scheduler cannot flake them. Performance-sensitive render/layout/geometry changes should include before/after measurements, preferably with `pagecore_cli --perf-trace`, and should document whether the win appears in the full pipeline rather than only in an isolated micro-step. Regenerate `examples/visual-regression/pagecore-visual.png` only when the intentional visual output changes.

## Commit & Pull Request Guidelines

Local Git history was unavailable in this checkout, so no repository-specific commit convention could be inferred. Use concise imperative commit messages, for example `Add WebP decoder tests` or `Fix rounded border rasterization`. Pull requests should describe the affected subsystem, list test commands run, and include screenshots when visual output changes.

## Agent-Specific Instructions

If the same build, test, or runtime error appears twice, stop guessing. Research 3-5 plausible fixes, choose the strongest one, and document the reasoning in the handoff or final summary.

For JS-heavy DOM/geometry performance work, measure the current behavior before changing code and re-run the same workload after the change. Use `--perf-trace PATH|-` to capture JSONL phase timings for `serialize_html`, `subresource_scan`, `litehtml_load_html`, `litehtml_layout`, `computed_style`, `geometry`, `raster`, and `png_encode`; `-` writes to stderr. Treat perf events as nested, not exclusive: a geometry event can include a forced litehtml load/layout and those phases may also be emitted separately.

Keep documentation aligned with the code in the same change. `README.md` is the user-facing document (features, CLI, embedding); when changing public embedding APIs, keep it and the public headers in sync. `docs/architecture.md` describes engine internals; update it when changing the DOM/JS bridge, resource pipeline, security model, or rendering behavior it documents. Perf tracing lives in `include/pagecore/perf.hpp`; `LoadOptions::perf_trace` is the load-time/fallback sink, `RenderOptions::perf_trace` can override it for a specific render/display-list/layout-related read, and PNG encoding has explicit traced overloads.

When changing the page-visible browser API surface in `src/dom_shim/` (adding, removing, or materially changing globals, constructors, or feature-detection behavior), update `docs/browser-api-support.md` in the same change. Keep each affected API classified as `supported`, `partial`, or `absent`, and do not expose no-op/always-failing stubs when an absent API would let scraping targets take a safer fallback path.

Keep measured negative perf experiments documented instead of reintroducing them. In particular, the one-pass DOM traversal replacement for render sub-resource discovery was measured and reverted after a small end-to-end regression; keep the selector-based discovery unless a new patch shows both an isolated discovery-stage win and a full render-pipeline win.
