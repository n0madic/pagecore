# stb

Vendored copy of `stb_image.h` from the `nothings/stb` repository.

- Upstream: https://github.com/nothings/stb
- File: `stb_image.h`
- Commit: `31c1ad37456438565541f4919958214b6e762fb4`
- License: public domain or MIT; see `LICENSE` and the license block at the end of `stb_image.h`.

PageCore uses this only when configured with `-DPAGECORE_IMAGE_DECODER=stb`.
That backend decodes PNG, JPEG, and GIF through stb_image while keeping WebP on
libwebp and SVG on PageCore's Cairo-based SVG subset.
