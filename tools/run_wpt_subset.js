#!/usr/bin/env node
'use strict';

const childProcess = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

function defaultJobs() {
  const parallelism = typeof os.availableParallelism === 'function' ? os.availableParallelism() : os.cpus().length;
  return Math.max(1, parallelism);
}

function usage() {
  console.error([
    'usage: run_wpt_subset.js --case-runner PATH --manifest PATH [--root PATH] [--wait-ms N]',
    '                        [--jobs N] [--report PATH]',
    '',
    'Options:',
    '  --jobs N      run up to N case runners concurrently (default: available parallelism)',
    '  --report PATH write a structured JSON result/failure report to PATH',
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
    waitMs: 15000,
    jobs: defaultJobs(),
    report: null
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
    else if (arg === '--jobs') args.jobs = Number(next());
    else if (arg === '--report') args.report = next();
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
  if (!Number.isInteger(args.jobs) || args.jobs <= 0) throw new Error('--jobs must be a positive integer');
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

  return new Promise((resolve, reject) => {
    const child = childProcess.spawn(caseRunner, runnerArgs, { stdio: ['ignore', 'pipe', 'pipe'] });
    const stdoutChunks = [];
    const stderrChunks = [];
    child.stdout.on('data', (chunk) => stdoutChunks.push(chunk));
    child.stderr.on('data', (chunk) => stderrChunks.push(chunk));

    child.on('error', (error) => {
      reject(new Error(`${test.name || test.path}: could not start case runner: ${error.message}`));
    });

    child.on('close', (status) => {
      const stdout = Buffer.concat(stdoutChunks).toString('utf8').trim();
      const stderr = Buffer.concat(stderrChunks).toString('utf8').trim();

      if (status !== 0) {
        reject(new Error([
          `${test.name || test.path}: case runner failed with ${status}`,
          stdout,
          stderr
        ].filter(Boolean).join('\n')));
        return;
      }
      if (!stdout) {
        reject(new Error(`${test.name || test.path}: case runner produced no JSON`));
        return;
      }
      try {
        resolve(JSON.parse(stdout));
      } catch (error) {
        reject(new Error(`${test.name || test.path}: invalid case runner JSON: ${error.message}\n${stdout}`));
      }
    });
  });
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

// Collapse a failure line into a stable shape so that the same underlying defect
// reported across hundreds of tests (and with different values/names baked into
// the message) clusters into a single reportable cause. Deliberately
// value-agnostic: no hardcoded taxonomy of subsystems, which would rot as the
// engine changes.
function failureSignature(line, label) {
  let text = line;
  // A case-runner failure is reported as `${name}: ${error.message}` and the
  // error message already carries the same prefix, so the label can appear more
  // than once. Strip every leading occurrence, otherwise the test name leaks
  // into the signature and each file becomes its own cluster.
  while (label && text.startsWith(`${label}: `)) {
    text = text.slice(label.length + 2);
  }
  // Keep only the first line: crash reports append the runner's stdout/stderr.
  text = text.split('\n', 1)[0];
  const separator = text.indexOf(' | ');
  if (separator !== -1) {
    text = text.slice(separator + 3);
  }
  return text
    .replace(/"[^"]*"/g, '"…"')
    .replace(/'[^']*'/g, "'…'")
    .replace(/-?\d+(?:\.\d+)?/g, 'N')
    .replace(/\s+/g, ' ')
    .trim()
    .slice(0, 120);
}

function buildReport(context, results) {
  const clusters = new Map();
  for (const result of results) {
    for (const line of result.failures) {
      const signature = failureSignature(line, result.name);
      if (!clusters.has(signature)) {
        clusters.set(signature, { signature, subtests: 0, files: new Set(), sample: line });
      }
      const cluster = clusters.get(signature);
      cluster.subtests++;
      cluster.files.add(result.name);
    }
  }

  const failureClusters = [...clusters.values()]
    .map((cluster) => ({
      signature: cluster.signature,
      subtests: cluster.subtests,
      files: cluster.files.size,
      sample: cluster.sample,
      examples: [...cluster.files].slice(0, 20)
    }))
    .sort((a, b) => b.files - a.files || b.subtests - a.subtests);

  const slowest = results
    .filter((result) => result.durationMs !== null)
    .sort((a, b) => b.durationMs - a.durationMs)
    .slice(0, 25)
    .map((result) => ({ name: result.name, status: result.status, durationMs: result.durationMs }));

  return {
    generatedAt: new Date().toISOString(),
    caseRunner: context.caseRunner,
    manifest: context.manifest,
    root: context.root,
    jobs: context.jobs,
    wallClockMs: context.wallClockMs,
    summary: {
      total: results.length,
      pass: results.filter((result) => result.status === 'PASS').length,
      fail: results.filter((result) => result.status === 'FAIL').length,
      skip: results.filter((result) => result.status === 'SKIP').length,
      failureLines: results.reduce((sum, result) => sum + result.failures.length, 0)
    },
    slowest,
    failureClusters,
    tests: results.map((result) => ({
      name: result.name,
      path: result.path,
      status: result.status,
      durationMs: result.durationMs,
      harness: result.harness,
      message: result.message,
      subtests: result.subtests,
      failures: result.failures
    }))
  };
}

// Bounded worker pool. Results are emitted in manifest order (a completed test
// is only printed once every earlier test has printed), so adding --jobs never
// reorders output or makes a run non-reproducible.
async function runAll(tests, context) {
  const canRunRendering = renderingEnabled();
  const results = new Array(tests.length).fill(null);
  let nextIndex = 0;
  let flushCursor = 0;

  const flush = () => {
    while (flushCursor < tests.length && results[flushCursor] !== null) {
      const result = results[flushCursor];
      console.log(result.status === 'SKIP'
        ? `SKIP ${result.name} (requires rendering)`
        : `${result.status} ${result.name}`);
      flushCursor++;
    }
  };

  const evaluate = async (test, index) => {
    const name = (test && (test.name || test.path)) || `#${index}`;
    const base = { name, path: test && test.path, durationMs: null, harness: null, message: null, subtests: [], failures: [] };

    if (!test || !test.path) {
      return { ...base, status: 'FAIL', failures: ['manifest contains a test entry without path'] };
    }
    if (test.requiresRendering && !canRunRendering) {
      return { ...base, status: 'SKIP' };
    }

    const startedAt = Date.now();
    let actual;
    try {
      actual = await runCase(context.caseRunner, context.root, test, context.waitMs);
    } catch (error) {
      const durationMs = Date.now() - startedAt;
      if (test.requiresRendering && RENDERING_UNAVAILABLE_PATTERN.test(error.message)) {
        return { ...base, status: 'SKIP', durationMs };
      }
      return { ...base, status: 'FAIL', durationMs, failures: [`${name}: ${error.message}`] };
    }

    const durationMs = Date.now() - startedAt;
    const failures = compareExpected(test, actual);
    return {
      ...base,
      status: failures.length > 0 ? 'FAIL' : 'PASS',
      durationMs,
      harness: actual.harness ?? null,
      message: actual.message ?? null,
      subtests: actual.subtests || [],
      failures
    };
  };

  const worker = async () => {
    for (;;) {
      const index = nextIndex++;
      if (index >= tests.length) return;
      // Isolate each test: a crash, timeout, or malformed-JSON case-runner
      // failure for one manifest entry must not prevent the remaining entries
      // from running and being reported.
      results[index] = await evaluate(tests[index], index);
      flush();
    }
  };

  await Promise.all(Array.from({ length: Math.min(context.jobs, tests.length) }, worker));
  flush();
  return results;
}

async function main() {
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

  const context = {
    caseRunner: args.caseRunner,
    manifest: manifestPath,
    root,
    waitMs: args.waitMs,
    jobs: args.jobs
  };

  const startedAt = Date.now();
  const results = await runAll(tests, context);
  context.wallClockMs = Date.now() - startedAt;

  const failures = results.flatMap((result) => result.failures);
  const passed = results.filter((result) => result.status === 'PASS').length;
  const failed = results.filter((result) => result.status === 'FAIL').length;
  const skipped = results.filter((result) => result.status === 'SKIP').length;

  const report = buildReport(context, results);

  if (args.report) {
    const reportPath = path.resolve(args.report);
    fs.mkdirSync(path.dirname(reportPath), { recursive: true });
    fs.writeFileSync(reportPath, `${JSON.stringify(report, null, 2)}\n`);
    console.log(`WPT subset report: ${reportPath}`);
  }

  // "fail" counts failing test files, matching "pass"/"skip"; failureLines is
  // reported separately because one file can contribute many failing subtests.
  console.log(`WPT subset summary: pass=${passed} fail=${failed} skip=${skipped} failureLines=${failures.length} jobs=${args.jobs} in ${(context.wallClockMs / 1000).toFixed(1)}s`);

  if (failed > 0) {
    console.log('Top failure causes (files x signature):');
    for (const cluster of report.failureClusters.slice(0, 15)) {
      console.log(`  ${String(cluster.files).padStart(4)} files ${String(cluster.subtests).padStart(6)} subtests  ${cluster.signature}`);
    }
  }

  if (failures.length > 0) {
    console.error(failures.join('\n'));
    process.exit(1);
  }
}

if (require.main === module) {
  main().catch((error) => {
    console.error(`run_wpt_subset.js: ${error.message}`);
    process.exit(1);
  });
}

module.exports = { compareExpected, actualSubtestMap, renderingEnabled, failureSignature, buildReport };
