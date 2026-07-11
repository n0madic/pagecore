#!/usr/bin/env node
'use strict';

// Minimal stand-in for the real pagecore_wpt_case binary, used by
// run_wpt_subset_unit_tests.js to exercise run_wpt_subset.js's orchestration
// logic (per-test isolation, waitMs pass-through, --jobs concurrency, --report
// clustering) without needing a full engine build. Speaks the same
// --root/--path/--wait-ms/--url CLI contract and prints the same JSON report
// shape on stdout.

const fs = require('fs');
const path = require('path');

const args = process.argv.slice(2);

function argVal(flag) {
  const index = args.indexOf(flag);
  return index === -1 ? undefined : args[index + 1];
}

const testPath = argVal('--path') || '';
const waitMs = argVal('--wait-ms');

function sleepSync(ms) {
  Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, ms);
}

if (testPath.includes('broken')) {
  console.error('fake_wpt_case_runner: simulated crash');
  process.exit(1);
}

if (testPath.includes('errored')) {
  console.log(JSON.stringify({ harness: 'ERROR', message: 'boom', subtests: [] }));
  process.exit(0);
}

if (testPath.includes('good-zero-wait')) {
  // Only succeeds if --wait-ms was passed through as the literal "0" the
  // manifest requested, not silently replaced by the CLI-wide default.
  if (waitMs !== '0') {
    console.error(`fake_wpt_case_runner: expected --wait-ms 0, got ${JSON.stringify(waitMs)}`);
    process.exit(1);
  }
  console.log(JSON.stringify({ harness: 'OK', message: null, subtests: [{ name: 'ok subtest', status: 'PASS', message: null }] }));
  process.exit(0);
}

if (testPath.includes('barrier')) {
  // Concurrency proof without a timing assertion: every barrier case registers
  // itself and then blocks until FAKE_RUNNER_BARRIER of them are running at the
  // same time. A sequential runner can never release the barrier, so it fails
  // deterministically instead of "being slow"; a parallel runner passes as soon
  // as the pool really overlaps.
  const syncDir = process.env.FAKE_RUNNER_SYNC_DIR;
  const required = Number(process.env.FAKE_RUNNER_BARRIER || '2');
  const timeoutMs = Number(process.env.FAKE_RUNNER_BARRIER_TIMEOUT_MS || '10000');
  if (!syncDir) {
    console.error('fake_wpt_case_runner: FAKE_RUNNER_SYNC_DIR is required for barrier cases');
    process.exit(1);
  }

  fs.writeFileSync(path.join(syncDir, `${process.pid}`), '');
  const deadline = Date.now() + timeoutMs;
  while (fs.readdirSync(syncDir).length < required) {
    if (Date.now() > deadline) {
      console.error(`fake_wpt_case_runner: barrier of ${required} never reached (runner is not parallel)`);
      process.exit(1);
    }
    sleepSync(10);
  }

  console.log(JSON.stringify({ harness: 'OK', message: null, subtests: [{ name: 'barrier subtest', status: 'PASS', message: null }] }));
  process.exit(0);
}

if (testPath.includes('cluster')) {
  // Two tests failing for the same underlying reason, but with different values
  // baked into the message, so --report can be checked for collapsing them into
  // one cause instead of two.
  const width = testPath.includes('cluster-a') ? 100 : 250;
  console.log(JSON.stringify({
    harness: 'OK',
    message: null,
    subtests: [{ name: 'width reflects', status: 'FAIL', message: `assert_equals: expected ${width} but got 0` }]
  }));
  process.exit(0);
}

console.error(`fake_wpt_case_runner: unknown fixture path ${testPath}`);
process.exit(1);
