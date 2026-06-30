# pagecore visual fixture

This directory contains a single-page visual fixture for the current offscreen rendering pipeline used by the CLI and headless automation workflows.

Render it with a rendering-enabled build:

```sh
build/pagecore_cli \
  --file examples/visual-regression/index.html \
  --format png \
  --output examples/visual-regression/pagecore-visual.png \
  --dump-display-list examples/visual-regression/display-list.json \
  --viewport 1280x1000
```

Covered surfaces:

- external HTML file loading through `file://`
- external CSS plus `@import`
- external JavaScript mutating the Lexbor DOM before layout
- text, inline elements, list markers and a table
- solid fills, borders, rounded clipping and overflow clipping
- `<img>` decoding for PNG, JPEG and WebP
- CSS background images using PNG, JPEG and WebP with sizing, repeat and positioning
- CSS linear gradients through the backend-neutral display list

Image sources:

- `sample-photo.jpg`: Wikimedia Commons `Fronalpstock_big.jpg`, downloaded via `Special:FilePath` and resized by Wikimedia. Source page: <https://commons.wikimedia.org/wiki/File:Fronalpstock_big.jpg>
- `sample-transparent.png`: Wikimedia Commons `PNG_transparency_demonstration_1.png`, downloaded via `Special:FilePath` and resized by Wikimedia. Source page: <https://commons.wikimedia.org/wiki/File:PNG_transparency_demonstration_1.png>
- `sample-photo.webp`: local WebP derivative of `sample-photo.jpg`, generated with `cwebp -q 82`.
