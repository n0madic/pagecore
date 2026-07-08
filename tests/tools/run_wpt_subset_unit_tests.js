#!/usr/bin/env node
'use strict';

const assert = require('assert');
const childProcess = require('child_process');
const path = require('path');

const { compareExpected, actualSubtestMap } = require(path.resolve(__dirname, '..', '..', 'tools', 'run_wpt_subset.js'));

const testPromises = [];

function test(name, fn) {
  const promise = Promise.resolve()
    .then(fn)
    .then(() => {
      console.log(`ok - ${name}`);
    }, (error) => {
      console.error(`not ok - ${name}`);
      throw error;
    });
  testPromises.push(promise);
}

// --- compareExpected / actualSubtestMap: pure orchestration logic ---

test('compareExpected flags a harness ERROR even when the manifest omits expected.harness', () => {
  const failures = compareExpected(
    { name: 'no-expected-harness', expected: {} },
    { harness: 'ERROR', message: 'boom', subtests: [] }
  );
  assert.ok(failures.some((line) => /harness expected OK, got ERROR/.test(line)), `expected a harness mismatch failure, got: ${JSON.stringify(failures)}`);
});

test('compareExpected respects an explicit expected.harness value', () => {
  const failures = compareExpected(
    { name: 'explicit-harness', expected: { harness: 'ERROR' } },
    { harness: 'ERROR', message: 'boom', subtests: [] }
  );
  assert.deepStrictEqual(failures, []);
});

test('compareExpected reports a duplicate subtest name instead of silently keeping only the last result', () => {
  const failures = compareExpected(
    { name: 'dup', expected: { subtests: { x: 'PASS' } } },
    { harness: 'OK', subtests: [{ name: 'x', status: 'FAIL' }, { name: 'x', status: 'PASS' }] }
  );
  assert.ok(failures.some((line) => /duplicate subtest name "x"/.test(line)), `expected a duplicate-subtest failure, got: ${JSON.stringify(failures)}`);
});

test('compareExpected can require every reported subtest to pass without listing names', () => {
  const failures = compareExpected(
    { name: 'generated-baseline', expected: { harness: 'OK', subtests: 'all-pass' } },
    { harness: 'OK', subtests: [{ name: 'a', status: 'PASS' }, { name: 'b', status: 'FAIL', message: 'boom' }] }
  );
  assert.deepStrictEqual(failures, ['generated-baseline: "b" expected PASS, got FAIL | boom']);
});

test('compareExpected accepts generated all-pass manifests when all reported subtests pass', () => {
  const failures = compareExpected(
    { name: 'generated-baseline', expected: { harness: 'OK', subtests: 'all-pass' } },
    { harness: 'OK', subtests: [{ name: 'a', status: 'PASS' }, { name: 'b', status: 'PASS' }] }
  );
  assert.deepStrictEqual(failures, []);
});

test('actualSubtestMap surfaces which names were duplicated', () => {
  const { byName, duplicates } = actualSubtestMap({ subtests: [{ name: 'a', status: 'PASS' }, { name: 'a', status: 'FAIL' }, { name: 'b', status: 'PASS' }] });
  assert.deepStrictEqual([...duplicates], ['a']);
  assert.strictEqual(byName.size, 2);
});

// --- CLI integration: per-test isolation and waitMs pass-through ---

test('CLI: a crashing test does not abort the rest of the manifest, and waitMs:0 is honored', () => {
  const runnerScript = path.resolve(__dirname, '..', '..', 'tools', 'run_wpt_subset.js');
  const caseRunner = path.resolve(__dirname, 'fixtures', 'fake_wpt_case_runner.js');
  const manifest = path.resolve(__dirname, 'fixtures', 'run_wpt_subset_isolation_manifest.json');

  const result = childProcess.spawnSync(process.execPath, [
    runnerScript,
    '--case-runner', caseRunner,
    '--manifest', manifest,
    '--root', __dirname
  ], { encoding: 'utf8' });

  const stdout = result.stdout;
  assert.ok(/FAIL broken-first/.test(stdout), `expected the crashing test to be reported, got:\n${stdout}\n${result.stderr}`);
  assert.ok(/FAIL no-expected-harness-errors/.test(stdout), `expected the harness-default fix to flag the ERROR result, got:\n${stdout}\n${result.stderr}`);
  assert.ok(/PASS zero-wait/.test(stdout), `expected the third test to run and pass (proving isolation + waitMs:0 pass-through), got:\n${stdout}\n${result.stderr}`);
  assert.strictEqual(result.status, 1, 'overall exit code must reflect the real failures without swallowing the rest of the run');
});

Promise.all(testPromises).catch((error) => {
  console.error(error && error.stack ? error.stack : error);
  process.exitCode = 1;
});
