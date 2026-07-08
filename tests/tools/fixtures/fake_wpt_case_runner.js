#!/usr/bin/env node
'use strict';

// Minimal stand-in for the real pagecore_wpt_case binary, used by
// run_wpt_subset_unit_tests.js to exercise run_wpt_subset.js's orchestration
// logic (per-test isolation, waitMs pass-through) without needing a full
// engine build. Speaks the same --root/--path/--wait-ms/--url CLI contract
// and prints the same JSON report shape on stdout.

const args = process.argv.slice(2);

function argVal(flag) {
  const index = args.indexOf(flag);
  return index === -1 ? undefined : args[index + 1];
}

const testPath = argVal('--path') || '';
const waitMs = argVal('--wait-ms');

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

console.error(`fake_wpt_case_runner: unknown fixture path ${testPath}`);
process.exit(1);
