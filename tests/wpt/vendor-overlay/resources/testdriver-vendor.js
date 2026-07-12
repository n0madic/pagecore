// Extended-corpus-only overlay: same content as tests/wpt/resources/testdriver-vendor.js.
// This directory intentionally holds *only* this one file so it can be listed
// as the first --root ahead of a real external WPT checkout (see
// pagecore_wpt_extended in CMakeLists.txt) without shadowing anything else --
// in particular testharness.js/testharnessreport.js, which must keep coming
// from the real checkout so a corpus run stays comparable against prior
// baselines measured against the same upstream harness. Keep this in sync
// with tests/wpt/resources/testdriver-vendor.js if that one ever changes.
window.test_driver_internal = window.test_driver_internal || {};
