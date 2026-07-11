#!/usr/bin/env node
'use strict';

const assert = require('assert');
const childProcess = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

const { compareExpected, actualSubtestMap, failureSignature } = require(path.resolve(__dirname, '..', '..', 'tools', 'run_wpt_subset.js'));

const RUNNER_SCRIPT = path.resolve(__dirname, '..', '..', 'tools', 'run_wpt_subset.js');
const CASE_RUNNER = path.resolve(__dirname, 'fixtures', 'fake_wpt_case_runner.js');

function fixtureManifest(name) {
  return path.resolve(__dirname, 'fixtures', name);
}

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
  const result = childProcess.spawnSync(process.execPath, [
    RUNNER_SCRIPT,
    '--case-runner', CASE_RUNNER,
    '--manifest', fixtureManifest('run_wpt_subset_isolation_manifest.json'),
    '--root', __dirname
  ], { encoding: 'utf8' });

  const stdout = result.stdout;
  assert.ok(/FAIL broken-first/.test(stdout), `expected the crashing test to be reported, got:\n${stdout}\n${result.stderr}`);
  assert.ok(/FAIL no-expected-harness-errors/.test(stdout), `expected the harness-default fix to flag the ERROR result, got:\n${stdout}\n${result.stderr}`);
  assert.ok(/PASS zero-wait/.test(stdout), `expected the third test to run and pass (proving isolation + waitMs:0 pass-through), got:\n${stdout}\n${result.stderr}`);
  assert.strictEqual(result.status, 1, 'overall exit code must reflect the real failures without swallowing the rest of the run');
});

// --- --jobs: real concurrency, stable output order ---

test('CLI: --jobs actually runs cases concurrently', () => {
  const syncDir = fs.mkdtempSync(path.join(os.tmpdir(), 'pagecore-wpt-barrier-'));
  try {
    const result = childProcess.spawnSync(process.execPath, [
      RUNNER_SCRIPT,
      '--case-runner', CASE_RUNNER,
      '--manifest', fixtureManifest('run_wpt_subset_parallel_manifest.json'),
      '--root', __dirname,
      '--jobs', '4'
    ], {
      encoding: 'utf8',
      env: { ...process.env, FAKE_RUNNER_SYNC_DIR: syncDir, FAKE_RUNNER_BARRIER: '4' }
    });

    // Each barrier case blocks until all four are running at once, so this can
    // only pass if the pool overlaps them. No wall-clock assertion involved.
    assert.strictEqual(result.status, 0, `expected a parallel run to release the 4-way barrier, got:\n${result.stdout}\n${result.stderr}`);
  } finally {
    fs.rmSync(syncDir, { recursive: true, force: true });
  }
});

test('CLI: --jobs 1 cannot release a concurrency barrier (guards the barrier fixture itself)', () => {
  const syncDir = fs.mkdtempSync(path.join(os.tmpdir(), 'pagecore-wpt-barrier-'));
  try {
    const result = childProcess.spawnSync(process.execPath, [
      RUNNER_SCRIPT,
      '--case-runner', CASE_RUNNER,
      '--manifest', fixtureManifest('run_wpt_subset_parallel_manifest.json'),
      '--root', __dirname,
      '--jobs', '1'
    ], {
      encoding: 'utf8',
      env: {
        ...process.env,
        FAKE_RUNNER_SYNC_DIR: syncDir,
        FAKE_RUNNER_BARRIER: '2',
        // The barrier can never be released here, so this is pure dead wait:
        // keep it short. It is an upper bound on a failure path, not a
        // must-exceed timing assertion.
        FAKE_RUNNER_BARRIER_TIMEOUT_MS: '1500'
      }
    });

    assert.strictEqual(result.status, 1, 'a sequential run must not be able to satisfy a 2-way barrier');
  } finally {
    fs.rmSync(syncDir, { recursive: true, force: true });
  }
});

test('CLI: --jobs preserves manifest order in the streamed output', () => {
  const syncDir = fs.mkdtempSync(path.join(os.tmpdir(), 'pagecore-wpt-barrier-'));
  try {
    const result = childProcess.spawnSync(process.execPath, [
      RUNNER_SCRIPT,
      '--case-runner', CASE_RUNNER,
      '--manifest', fixtureManifest('run_wpt_subset_parallel_manifest.json'),
      '--root', __dirname,
      '--jobs', '4'
    ], {
      encoding: 'utf8',
      env: { ...process.env, FAKE_RUNNER_SYNC_DIR: syncDir, FAKE_RUNNER_BARRIER: '4' }
    });

    const reported = result.stdout.split('\n')
      .map((line) => /^(?:PASS|FAIL|SKIP) (\S+)/.exec(line))
      .filter(Boolean)
      .map((match) => match[1]);
    assert.deepStrictEqual(reported, ['barrier-1', 'barrier-2', 'barrier-3', 'barrier-4'], `parallel output must still be emitted in manifest order, got:\n${result.stdout}`);
  } finally {
    fs.rmSync(syncDir, { recursive: true, force: true });
  }
});

// --- --report: structured results + failure clustering ---

test('failureSignature collapses the same defect reported with different values', () => {
  const a = failureSignature('cluster-a: "width reflects" expected PASS, got FAIL | assert_equals: expected 100 but got 0', 'cluster-a');
  const b = failureSignature('cluster-b: "width reflects" expected PASS, got FAIL | assert_equals: expected 250 but got 0', 'cluster-b');
  assert.strictEqual(a, b, 'value-only differences must not split one cause into two clusters');
  assert.strictEqual(a, 'assert_equals: expected N but got N');
});

test('failureSignature strips the repeated label and trailing output from a case-runner crash', () => {
  // The crash path reports `${name}: ${error.message}`, and error.message
  // already starts with the same name, so the label appears twice.
  const signature = failureSignature(
    'dom/x: dom/x: case runner failed with 1\npagecore_wpt_case: WPT did not complete',
    'dom/x'
  );
  assert.strictEqual(signature, 'case runner failed with N', `the test name must not leak into the signature, got: ${signature}`);
});

test('CLI: --report writes structured results and clusters failures by cause', () => {
  const reportDir = fs.mkdtempSync(path.join(os.tmpdir(), 'pagecore-wpt-report-'));
  const reportPath = path.join(reportDir, 'nested', 'report.json');
  try {
    const result = childProcess.spawnSync(process.execPath, [
      RUNNER_SCRIPT,
      '--case-runner', CASE_RUNNER,
      '--manifest', fixtureManifest('run_wpt_subset_report_manifest.json'),
      '--root', __dirname,
      '--report', reportPath
    ], { encoding: 'utf8' });

    assert.strictEqual(result.status, 1, `expected the two failing fixtures to fail the run, got:\n${result.stdout}\n${result.stderr}`);

    const report = JSON.parse(fs.readFileSync(reportPath, 'utf8'));
    assert.deepStrictEqual(
      { total: report.summary.total, pass: report.summary.pass, fail: report.summary.fail, skip: report.summary.skip },
      { total: 3, pass: 1, fail: 2, skip: 0 },
      'summary must count test files, not failure lines'
    );

    assert.strictEqual(report.failureClusters.length, 1, `the two fixtures fail for one reason and must form a single cluster, got: ${JSON.stringify(report.failureClusters)}`);
    assert.strictEqual(report.failureClusters[0].files, 2);
    assert.deepStrictEqual(report.failureClusters[0].examples.sort(), ['cluster-a', 'cluster-b']);

    const passing = report.tests.find((entry) => entry.name === 'zero-wait');
    assert.strictEqual(passing.status, 'PASS');
    assert.strictEqual(passing.harness, 'OK');
    assert.ok(typeof passing.durationMs === 'number', 'per-test durations must be recorded so slow outliers are visible');
    assert.ok(report.slowest.length > 0, 'report must surface the slowest tests');
  } finally {
    fs.rmSync(reportDir, { recursive: true, force: true });
  }
});

Promise.all(testPromises).catch((error) => {
  console.error(error && error.stack ? error.stack : error);
  process.exitCode = 1;
});
