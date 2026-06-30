# Repository Guidelines

## Project Structure & Module Organization

This is a C++20/CMake modular web engine for headless automation, embedding, offscreen rendering, and future browser or GUI frontends. Public APIs live in `include/pagecore/`. Core implementation lives in `src/`, including DOM/JS bridge code, resource loading, image decoding, display-list dumping, and Cairo rasterization. `src/dom_shim.js` is embedded into the binary at build time. CLI entry points are in `tools/`. Tests are centralized in `tests/pagecore_tests.cpp`. Visual regression fixtures and generated reference artifacts are in `examples/visual-regression/`.

## Build, Test, and Development Commands

Use an out-of-tree CMake build:

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

For a smaller DOM/JS-only test build, configure with `-DPAGECORE_BUILD_TOOLS=OFF -DPAGECORE_ENABLE_RENDERING=OFF`.

Useful CLI examples:

```sh
build/pagecore_cli --file examples/visual-regression/index.html --screenshot /tmp/shot.png --viewport 1280x1000
build/pagecore_cli --file page.html --dump-display-list /tmp/display-list.json
```

## Coding Style & Naming Conventions

Follow the existing C++ style: 4-space indentation, braces on their own lines for functions/classes, `snake_case` for functions and local variables, and `PascalCase` for public structs/classes such as `DisplayList` or `ResourceLoader`. Prefer small, focused helpers in anonymous namespaces. Keep public types backend-neutral; layout adapters and raster backends should communicate through `include/pagecore/render.hpp`.

## Testing Guidelines

Use the existing CTest target and add focused tests to `tests/pagecore_tests.cpp`. Name tests with the `test_<area>_<behavior>` pattern. Rendering-sensitive changes should include pixel-invariant checks rather than brittle full-image comparisons. Regenerate `examples/visual-regression/pagecore-visual.png` only when the intentional visual output changes.

## Commit & Pull Request Guidelines

Local Git history was unavailable in this checkout, so no repository-specific commit convention could be inferred. Use concise imperative commit messages, for example `Add WebP decoder tests` or `Fix rounded border rasterization`. Pull requests should describe the affected subsystem, list test commands run, and include screenshots when visual output changes.

## Agent-Specific Instructions

If the same build, test, or runtime error appears twice, stop guessing. Research 3-5 plausible fixes, choose the strongest one, and document the reasoning in the handoff or final summary.
