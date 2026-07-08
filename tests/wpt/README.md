# PageCore WPT Corpus

This directory contains PageCore's default offline WPT-style smoke corpus. It is
small by design so `ctest` stays fast and does not need a WPT checkout or a local
HTTP server.

For broader compatibility checks, configure with a local upstream WPT checkout:

```sh
cmake -S . -B build \
  -DPAGECORE_ENABLE_WPT_EXTENDED=ON \
  -DPAGECORE_WPT_ROOT=/path/to/wpt
```

The extended manifest is intentionally empty by default and has no default
`root`; pass `--root` explicitly when invoking `tools/run_wpt_subset.js`
against it manually (CTest does this via `PAGECORE_WPT_ROOT`). Add
manifest-listed tests with exact expected statuses before enabling it in CI.
For exploratory upstream subsets, generate a local manifest first:

```sh
node tools/generate_wpt_manifest.js \
  --root /path/to/wpt \
  --prefix url/ \
  --prefix dom/nodes/ \
  --prefix css/cssom-view/ \
  --output /tmp/pagecore-wpt-generated.json
```

The generator also skips HTML tests whose local helper scripts or stylesheet
resources are absent from the selected checkout.

All tests are served from the single synthetic origin
`https://web-platform.test` — cross-origin/multi-origin tests (references to
`www1.web-platform.test` and similar) are not supported. See
[docs/testing.md](../../docs/testing.md) for the full list of v1 limitations.
