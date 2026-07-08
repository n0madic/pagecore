# Testing

PageCore has three layers of compatibility tests in addition to the C++ and
DOM-shim unit tests.

## WPT Smoke Subset

`pagecore_wpt_smoke` runs a small offline WPT-style corpus from `tests/wpt`.
It covers the surfaces PageCore cares about first: DOM, CSSOM View, fetch, and
cookies. The corpus is intentionally small and self-contained so normal `ctest`
runs do not need a WPT checkout or a local HTTP server.

Run it through CTest:

```sh
ctest --test-dir build -R pagecore_wpt_smoke --output-on-failure
```

Or run the harness directly:

```sh
node tools/run_wpt_subset.js \
  --case-runner build/pagecore_wpt_case \
  --manifest tests/wpt/pagecore-wpt-smoke.json \
  --root tests/wpt
```

The manifest is authoritative. Each test lists expected harness and subtest
statuses; known failures should be represented there, so both unexpected
failures and unexpected passes are visible.

## Extended WPT

The extended WPT path is opt-in and reads tests from a local upstream
`web-platform-tests` checkout:

```sh
cmake -S . -B build \
  -DPAGECORE_ENABLE_WPT_EXTENDED=ON \
  -DPAGECORE_WPT_ROOT=/path/to/wpt
cmake --build build
ctest --test-dir build -R pagecore_wpt_extended --output-on-failure
```

`tests/wpt/pagecore-wpt-extended.json` is empty by default and has no default
`root` — pass `--root` explicitly (the `pagecore_wpt_extended` CTest target
does this via `PAGECORE_WPT_ROOT`) when invoking the runner manually against
it. Add explicit manifest entries before enabling the target in CI. The v1
runner supports manifest-listed `.html`, `.window.js`, and window-variant
`.any.js` tests. It does not implement WPT's full manifest discovery, workers,
service workers, Python handlers, websockets, HTTPS certificates, the full
wptserve feature set, or cross-origin/multi-origin requests — every test is
served from the single synthetic origin `https://web-platform.test`, so tests
that reference alternate hosts (`www1.web-platform.test` and similar) will
fail to resolve those sub-resources. This keeps the runner deterministic and
avoids weakening PageCore's default resource security model for local
loopback servers.

## Display-List Reftests

`pagecore_display_list_reftests` compares `pagecore_cli --dump-display-list`
output against JSON references. The comparison parses JSON, normalizes repo-local
`file://` URLs to `file://<repo>/...`, and compares numeric fields with a small
tolerance. This avoids machine-specific path churn while still locking command
order and structure.

Run it through CTest:

```sh
ctest --test-dir build -R pagecore_display_list_reftests --output-on-failure
```

Update references only when the display-list output intentionally changes:

```sh
node tools/run_display_list_reftests.js \
  --pagecore-cli build/pagecore_cli \
  --manifest tests/display-list/pagecore-display-list-reftests.json \
  --root . \
  --update
```
