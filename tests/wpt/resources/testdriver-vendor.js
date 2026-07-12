// Upstream WPT's own testdriver-vendor.js is an empty extension point that a
// vendor (browser embedder) is expected to override with real automation
// glue. PageCore's override just needs to *exist* at this URL and assign to
// `test_driver_internal` -- the assignment itself is intercepted by a
// property setter installed by installWptHook() (src/dom_shim/40_web.js),
// which merges in the real click/send_keys/action_sequence implementation.
//
// Used by pagecore_wpt_smoke (--root tests/wpt alone). The extended corpus
// (a real external WPT checkout) uses the standalone copy at
// tests/wpt/vendor-overlay/resources/testdriver-vendor.js instead, so that
// listing an overlay root first doesn't also shadow testharness.js and drift
// the extended run off the checkout's real test harness. Keep both in sync.
window.test_driver_internal = window.test_driver_internal || {};
