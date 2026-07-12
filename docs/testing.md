# Testing

PageCore has three layers of compatibility tests in addition to the C++ and
DOM-shim unit tests.

## Real-time event loop and tests

The event loop runs on real wall-clock time (libuv timers), so time-dependent
tests must budget real waits:

- Use short timer delays (tens of milliseconds) with generous `wait_time`
  budgets; never assert exact wall-clock ordering between a network response
  and a timer of similar magnitude (that is a race by construction).
- Pages that quiesce completely (no pending timers, tasks, transfers, or
  animation frames) become ready immediately regardless of `stable_window`, so
  most tests need no timing tweaks at all. Tests that keep work pending (an
  interval, an rAF loop, a long timer) should set a small `stable_window` to
  avoid waiting out the default 350ms per load.
- `wait_time = 0` executes already-queued zero-delay tasks without blocking,
  but never waits for delayed timers or in-flight transfers.
- Loopback-server tests must make server threads exit-able (close the listening
  socket from the test body) so a missing connection fails the assertion
  instead of hanging `join()`.
- Timing-sensitive additions should survive `./build/pagecore_tests` repeated
  10-20 times and a `-DPAGECORE_SANITIZE=address,undefined` build, which runs
  everything slower.

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

## Running a subset in parallel (`--jobs`)

`run_wpt_subset.js` runs up to `--jobs N` case runners concurrently, defaulting
to the machine's available parallelism. Each WPT test is already an isolated
process against an offline corpus, so the workload parallelizes well: a 127-test
sample takes 127s with `--jobs 1` and 37s with `--jobs 8`; a 1268-test corpus
drops from roughly 21 minutes to about 3 minutes.

Results are still emitted in manifest order — a finished test is only printed
once every earlier test has printed — so raising `--jobs` never reorders output
or makes a run less reproducible. Pass `--jobs 1` when you want strictly
sequential execution.

`--wait-ms` also raises the per-script and aggregate JavaScript deadlines when it
exceeds their defaults (30s and 60s). Without that, a large generated test — the
`html/dom/reflection-*` files drive thousands of subtests from a single inline
script and take tens of seconds — is interrupted mid-script no matter how long
you asked to wait, and reports as "WPT did not complete" instead of as a result.
A shorter `--wait-ms` never lowers those deadlines.

Runtime is dominated by a small tail of outliers (a few very large reflection
tests, and tests where a testharness subtest hits its own internal timeout, at
~30s each) rather than by the typical test (~0.5s). Tests that never call
`done()` are *not* slow: they exit as soon as the page quiesces, without
consuming the `--wait-ms` budget.

## Failure reports (`--report`)

`--report PATH` writes a structured JSON report: per-test harness status,
subtest statuses and messages, per-test durations, the slowest tests, and
failures grouped into clusters by cause.

```sh
node tools/run_wpt_subset.js \
  --case-runner build/pagecore_wpt_case \
  --manifest /tmp/pagecore-wpt-generated.json \
  --root /path/to/wpt \
  --jobs 8 --report /tmp/pagecore-wpt-report.json
```

Clustering normalizes each failure line (quoted strings and numbers are folded
away) so that one underlying defect reported across hundreds of tests with
different values collapses into a single cause. It is deliberately value-based
rather than a hardcoded taxonomy of subsystems, which would rot as the engine
changes. The top clusters are also printed at the end of a failing run, so
"what is failing and why" is answered by the run itself instead of by ad-hoc
post-processing.

Note that the summary line counts *test files* for `pass`/`fail`/`skip`, and
reports `failureLines` separately, because one failing file can contribute many
failing subtests.

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
it.

`--root` is repeatable; the first root listed wins on a resource-path
collision (matches `pagecore_wpt_case`'s own multi-root resolution). The
`pagecore_wpt_extended` CTest target always lists
`tests/wpt/vendor-overlay` before `PAGECORE_WPT_ROOT`, so every extended run
automatically gets PageCore's real `test_driver_internal` implementation at
`/resources/testdriver-vendor.js` (upstream's own copy at that path is an
empty vendor extension point, so `test_driver.click()`/`send_keys()`/
`Actions()` would otherwise fail with "Cannot read property of null"). That
overlay directory intentionally contains only that one file — not
`testharness.js` — so it never shadows the checkout's real test harness and
corpus runs stay comparable against prior baselines. A manual invocation of
`tools/run_wpt_subset.js` against a real checkout needs the same
`--root tests/wpt/vendor-overlay --root /path/to/wpt` order to get
`test_driver` support.

Generate a broad exploratory manifest from selected upstream directories with
the manifest generator:

```sh
node tools/generate_wpt_manifest.js \
  --root /path/to/wpt \
  --prefix url/ \
  --prefix dom/nodes/ \
  --prefix css/cssom-view/ \
  --output /tmp/pagecore-wpt-generated.json
```

Then run it directly:

```sh
node tools/run_wpt_subset.js \
  --case-runner build/pagecore_wpt_case \
  --manifest /tmp/pagecore-wpt-generated.json \
  --root tests/wpt/vendor-overlay \
  --root /path/to/wpt
```

Or wire it into the opt-in CTest target:

```sh
cmake -S . -B build \
  -DPAGECORE_ENABLE_WPT_EXTENDED=ON \
  -DPAGECORE_WPT_ROOT=/path/to/wpt \
  -DPAGECORE_WPT_EXTENDED_MANIFEST=/tmp/pagecore-wpt-generated.json
cmake --build build
ctest --test-dir build -R pagecore_wpt_extended --output-on-failure
```

Generated entries use `expected.subtests: "all-pass"`, which means the harness
must report `OK` and every reported subtest must pass, without manually listing
each subtest name. Curated manifests can still use explicit per-subtest
expectations to lock known failures and detect unexpected passes.

The v1 runner supports manifest-listed `.html`, `.window.js`, and
window-variant `.any.js` tests. The generator filters out common unsupported WPT
infrastructure such as workers, service workers, Python handlers, wptserve
template substitutions, iframe navigation, JavaScript URL navigation, persistent
network APIs, cross-origin helpers, and HTML tests that reference missing local
script or stylesheet resources in the selected WPT checkout.

It also drops `.html` files that never load `testharness.js`, because they can
never report a harness result and would otherwise be permanent false failures.
WPT uses bare HTML for reftests (`<link rel=match>`, verified by comparing
rendering), crashtests (which only assert "did not crash"), and data documents
that a real test loads as a fixture — the `encoding/legacy-mb-*` byte tables are
the largest group. The check is content-based, not name-based, on purpose: WPT
has files named `-crash.html` that *are* driven by testharness, and `.any.js`
files under `crashtests/` that are ordinary `promise_test`s, and both must be
kept. `.any.js`/`.window.js` are exempt because the runner injects testharness
into the document it generates for them.

The generator does not implement WPT's full
manifest discovery, workers, service workers, Python handlers, websockets,
HTTPS certificates, the full wptserve feature set, or cross-origin/multi-origin
requests — every test is served from the single synthetic origin
`https://web-platform.test`, so tests that reference alternate hosts
(`www1.web-platform.test` and similar) will fail to resolve those sub-resources.
This keeps the runner deterministic and avoids weakening PageCore's default
resource security model for local loopback servers.

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
