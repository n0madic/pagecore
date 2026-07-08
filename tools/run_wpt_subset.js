#!/usr/bin/env node
'use strict';

const childProcess = require('child_process');
const fs = require('fs');
const path = require('path');

function usage() {
  console.error([
    'usage: run_wpt_subset.js --case-runner PATH --manifest PATH [--root PATH] [--wait-ms N]',
    '',
    'Manifest format:',
    '  {',
    '    "root": "optional/default/root/relative/to/manifest",',
    '    "tests": [',
    '      {',
    '        "name": "stable id",',
    '        "path": "/dom/example.window.js",',
    '        "url": "/dom/example.window.html?variant",',
    '        "requiresRendering": false,',
    '        "expected": { "harness": "OK", "subtests": { "subtest name": "PASS" } }',
    '        "expected": { "harness": "OK", "subtests": "all-pass" }',
    '      }',
    '    ]',
    '  }'
  ].join('\n'));
}

function parseArgs(argv) {
  const args = {
    waitMs: 15000
  };
  for (let i = 2; i < argv.length; i++) {
    const arg = argv[i];
    const next = () => {
      if (i + 1 >= argv.length) throw new Error(`missing value for ${arg}`);
      return argv[++i];
    };

    if (arg === '--case-runner') args.caseRunner = next();
    else if (arg === '--manifest') args.manifest = next();
    else if (arg === '--root') args.root = next();
    else if (arg === '--wait-ms') args.waitMs = Number(next());
    else if (arg === '--help' || arg === '-h') {
      usage();
      process.exit(0);
    } else {
      throw new Error(`unknown argument: ${arg}`);
    }
  }
  if (!args.caseRunner) throw new Error('--case-runner is required');
  if (!args.manifest) throw new Error('--manifest is required');
  if (!Number.isInteger(args.waitMs) || args.waitMs <= 0) throw new Error('--wait-ms must be a positive integer');
  return args;
}

function readJson(file) {
  return JSON.parse(fs.readFileSync(file, 'utf8'));
}

function renderingEnabled() {
  const value = String(process.env.PAGECORE_ENABLE_RENDERING || '1').toLowerCase();
  return value !== '0' && value !== 'false' && value !== 'off' && value !== 'no';
}

// The case runner itself reports this exact message when the binary was built
// with PAGECORE_ENABLE_RENDERING=OFF (see src/rendering_disabled.cpp). Treating
// it as a skip signal (rather than relying solely on the PAGECORE_ENABLE_RENDERING
// env var, which manual/documented invocations may not set) keeps a
// requiresRendering test from failing when the binary genuinely can't render.
const RENDERING_UNAVAILABLE_PATTERN = /rendering is not available/;

function runCase(caseRunner, root, test, waitMs) {
  const runnerArgs = [
    '--root', root,
    '--path', test.path,
    '--wait-ms', String(test.waitMs ?? waitMs)
  ];
  if (test.url) {
    runnerArgs.push('--url', test.url);
  }

  const result = childProcess.spawnSync(caseRunner, runnerArgs, {
    encoding: 'utf8',
    stdio: ['ignore', 'pipe', 'pipe']
  });

  if (result.status !== 0) {
    throw new Error([
      `${test.name || test.path}: case runner failed with ${result.status}`,
      result.stdout.trim(),
      result.stderr.trim()
    ].filter(Boolean).join('\n'));
  }

  const stdout = result.stdout.trim();
  if (!stdout) {
    throw new Error(`${test.name || test.path}: case runner produced no JSON`);
  }

  try {
    return JSON.parse(stdout);
  } catch (error) {
    throw new Error(`${test.name || test.path}: invalid case runner JSON: ${error.message}\n${stdout}`);
  }
}

function actualSubtestMap(actual) {
  const out = new Map();
  const duplicates = new Set();
  for (const subtest of actual.subtests || []) {
    if (out.has(subtest.name)) {
      duplicates.add(subtest.name);
    }
    out.set(subtest.name, subtest);
  }
  return { byName: out, duplicates };
}

function compareExpected(test, actual) {
  const expected = test.expected || {};
  const failures = [];
  const label = test.name || test.path;

  // Default to "OK" (not "skip the check") when a manifest entry omits an
  // explicit harness expectation, so a test that errors/times out before
  // recording any subtests still fails loudly instead of reporting PASS.
  const expectedHarness = Object.prototype.hasOwnProperty.call(expected, 'harness') ? expected.harness : 'OK';
  if (actual.harness !== expectedHarness) {
    failures.push(`${label}: harness expected ${expectedHarness}, got ${actual.harness}${actual.message ? ` (${actual.message})` : ''}`);
  }

  const expectedSubtests = expected.subtests || {};
  const { byName: actualByName, duplicates } = actualSubtestMap(actual);

  for (const name of duplicates) {
    failures.push(`${label}: duplicate subtest name "${name}" recorded more than once (results for it are ambiguous)`);
  }

  if (expectedSubtests === 'all-pass') {
    for (const subtest of actual.subtests || []) {
      if (subtest.status !== 'PASS') {
        const message = subtest.message ? ` | ${String(subtest.message).replace(/\s+/g, ' ').trim()}` : '';
        failures.push(`${label}: "${subtest.name}" expected PASS, got ${subtest.status}${message}`);
      }
    }
    return failures;
  }

  for (const [name, expectedStatus] of Object.entries(expectedSubtests)) {
    const subtest = actualByName.get(name);
    if (!subtest) {
      failures.push(`${label}: missing subtest "${name}" (expected ${expectedStatus})`);
      continue;
    }
    if (subtest.status !== expectedStatus) {
      const message = subtest.message ? ` | ${String(subtest.message).replace(/\s+/g, ' ').trim()}` : '';
      failures.push(`${label}: "${name}" expected ${expectedStatus}, got ${subtest.status}${message}`);
    }
  }

  for (const subtest of actual.subtests || []) {
    if (!Object.prototype.hasOwnProperty.call(expectedSubtests, subtest.name)) {
      failures.push(`${label}: unexpected subtest "${subtest.name}" with status ${subtest.status}`);
    }
  }

  return failures;
}

function main() {
  const args = parseArgs(process.argv);
  const manifestPath = path.resolve(args.manifest);
  const manifest = readJson(manifestPath);
  const root = path.resolve(args.root || path.resolve(path.dirname(manifestPath), manifest.root || '.'));
  const tests = Array.isArray(manifest.tests) ? manifest.tests : [];
  if (tests.length === 0) {
    if (manifest.allowEmpty) {
      console.log(`WPT subset summary: pass=0 fail=0 skip=0 (${manifestPath} is empty)`);
      return;
    }
    throw new Error(`manifest has no tests: ${manifestPath}`);
  }
  if (!fs.existsSync(root)) {
    throw new Error(`WPT root does not exist: ${root}`);
  }

  const canRunRendering = renderingEnabled();
  const failures = [];
  let passed = 0;
  let skipped = 0;

  for (const test of tests) {
    if (!test || !test.path) {
      failures.push('manifest contains a test entry without path');
      continue;
    }
    if (test.requiresRendering && !canRunRendering) {
      skipped++;
      console.log(`SKIP ${test.name || test.path} (requires rendering)`);
      continue;
    }

    // Isolate each test: a crash, timeout, or malformed-JSON case-runner failure
    // for one manifest entry must not prevent the remaining entries from running
    // and being reported.
    let actual;
    try {
      actual = runCase(args.caseRunner, root, test, args.waitMs);
    } catch (error) {
      if (test.requiresRendering && RENDERING_UNAVAILABLE_PATTERN.test(error.message)) {
        skipped++;
        console.log(`SKIP ${test.name || test.path} (requires rendering)`);
        continue;
      }
      failures.push(`${test.name || test.path}: ${error.message}`);
      console.log(`FAIL ${test.name || test.path}`);
      continue;
    }

    const testFailures = compareExpected(test, actual);
    if (testFailures.length > 0) {
      failures.push(...testFailures);
      console.log(`FAIL ${test.name || test.path}`);
    } else {
      passed++;
      console.log(`PASS ${test.name || test.path}`);
    }
  }

  console.log(`WPT subset summary: pass=${passed} fail=${failures.length} skip=${skipped}`);
  if (failures.length > 0) {
    console.error(failures.join('\n'));
    process.exit(1);
  }
}

if (require.main === module) {
  try {
    main();
  } catch (error) {
    console.error(`run_wpt_subset.js: ${error.message}`);
    process.exit(1);
  }
}

module.exports = { compareExpected, actualSubtestMap, renderingEnabled };
