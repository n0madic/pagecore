#!/usr/bin/env node
'use strict';

const assert = require('assert');
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const harnessPath = path.resolve(__dirname, 'testharness.js');
const harnessSource = fs.readFileSync(harnessPath, 'utf8');

// Each test gets a fresh V8 context so testharness.js's module-level state
// (tests[], callbacks[], pending, completed) never leaks between tests.
function freshHarness() {
  const sandbox = {};
  sandbox.self = sandbox;
  sandbox.setTimeout = setTimeout;
  sandbox.console = console;
  vm.createContext(sandbox);
  vm.runInContext(harnessSource, sandbox, { filename: harnessPath });
  return sandbox;
}

function waitForCompletion(harness) {
  return new Promise((resolve) => {
    harness.add_completion_callback((tests, status) => resolve({ tests, status }));
  });
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

test('unreached_func routes through step() instead of throwing uncaught', async () => {
  const harness = freshHarness();
  const completion = waitForCompletion(harness);

  const t = harness.async_test(() => {}, 'probe');
  const handler = t.unreached_func('should not fire');

  // Regression test for a bug where unreached_func returned a bare throwing
  // closure instead of routing through t.step(): invoking it as a real
  // callback (e.g. an event handler) would throw uncaught, never decrement
  // `pending`, and hang the whole harness instead of recording a clean FAIL.
  assert.doesNotThrow(() => handler(), 'unreached_func() must not throw synchronously when invoked');

  const report = await completion;
  assert.strictEqual(report.tests.length, 1, 'harness must complete with exactly one recorded result');
  assert.strictEqual(report.tests[0].status, 1 /* FAIL */, 'unreached_func firing must record a FAIL');
  assert.ok(/should not fire/.test(report.tests[0].message), 'FAIL message must include the description');
});

test('test() records PASS and FAIL and completes the harness', async () => {
  const harness = freshHarness();
  const completion = waitForCompletion(harness);

  harness.test(() => { harness.assert_equals(1 + 1, 2); }, 'addition works');
  harness.test(() => { harness.assert_equals(1 + 1, 3); }, 'addition is wrong on purpose');

  const report = await completion;
  assert.strictEqual(report.tests.length, 2);
  assert.strictEqual(report.tests[0].status, 0 /* PASS */);
  assert.strictEqual(report.tests[1].status, 1 /* FAIL */);
});

test('promise_test waits for async completion before the harness completes', async () => {
  const harness = freshHarness();
  const completion = waitForCompletion(harness);

  let resolveWork;
  const work = new Promise((resolve) => { resolveWork = resolve; });
  harness.promise_test(() => work, 'deferred work');

  setTimeout(() => resolveWork(), 5);

  const report = await completion;
  assert.strictEqual(report.tests.length, 1);
  assert.strictEqual(report.tests[0].status, 0 /* PASS */);
});

Promise.all(testPromises).catch((error) => {
  console.error(error && error.stack ? error.stack : error);
  process.exitCode = 1;
});
