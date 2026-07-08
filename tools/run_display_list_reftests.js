#!/usr/bin/env node
'use strict';

const childProcess = require('child_process');
const fs = require('fs');
const path = require('path');
const { fileURLToPath } = require('url');

function usage() {
  console.error('usage: run_display_list_reftests.js --pagecore-cli PATH --manifest PATH [--root PATH] [--update]');
}

function parseArgs(argv) {
  const args = {
    update: false
  };
  for (let i = 2; i < argv.length; i++) {
    const arg = argv[i];
    const next = () => {
      if (i + 1 >= argv.length) throw new Error(`missing value for ${arg}`);
      return argv[++i];
    };
    if (arg === '--pagecore-cli') args.pagecoreCli = next();
    else if (arg === '--manifest') args.manifest = next();
    else if (arg === '--root') args.root = next();
    else if (arg === '--update') args.update = true;
    else if (arg === '--help' || arg === '-h') {
      usage();
      process.exit(0);
    } else {
      throw new Error(`unknown argument: ${arg}`);
    }
  }
  if (!args.pagecoreCli) throw new Error('--pagecore-cli is required');
  if (!args.manifest) throw new Error('--manifest is required');
  return args;
}

function readJson(file) {
  return JSON.parse(fs.readFileSync(file, 'utf8'));
}

function isUnderRoot(root, candidate) {
  const relative = path.relative(root, candidate);
  return relative === '' || (relative !== '..' && !relative.startsWith(`..${path.sep}`) && !path.isAbsolute(relative));
}

function slashPath(value) {
  return value.split(path.sep).join('/');
}

function markerRelativePath(filePath) {
  const normalized = filePath.replace(/\\/g, '/');
  for (const marker of ['/examples/', '/tests/', '/src/', '/include/', '/third_party/']) {
    const index = normalized.indexOf(marker);
    if (index >= 0) {
      return normalized.slice(index + 1);
    }
  }
  return null;
}

function normalizeFileUrl(value, root) {
  if (typeof value !== 'string' || !value.startsWith('file://')) {
    return value;
  }

  try {
    const filePath = fileURLToPath(value);
    const resolved = path.resolve(filePath);
    if (isUnderRoot(root, resolved)) {
      return `file://<repo>/${slashPath(path.relative(root, resolved))}`;
    }
    const markerRelative = markerRelativePath(resolved);
    if (markerRelative) {
      return `file://<repo>/${markerRelative}`;
    }
  } catch (_error) {
    // Leave malformed file URLs untouched; the comparison will report them.
  }
  return value;
}

function normalize(value, root) {
  if (Array.isArray(value)) {
    return value.map((entry) => normalize(entry, root));
  }
  if (value && typeof value === 'object') {
    const out = {};
    for (const key of Object.keys(value).sort()) {
      out[key] = normalize(value[key], root);
    }
    return out;
  }
  if (typeof value === 'string') {
    return normalizeFileUrl(value, root);
  }
  return value;
}

function compare(expected, actual, location, tolerance) {
  if (typeof expected === 'number' && typeof actual === 'number') {
    if (!Number.isFinite(expected) || !Number.isFinite(actual)) {
      if (Object.is(expected, actual)) return;
      throw new Error(`${location}: expected ${expected}, got ${actual}`);
    }
    if (Math.abs(expected - actual) > tolerance) {
      throw new Error(`${location}: expected ${expected}, got ${actual}, tolerance ${tolerance}`);
    }
    return;
  }

  if (Array.isArray(expected) || Array.isArray(actual)) {
    if (!Array.isArray(expected) || !Array.isArray(actual)) {
      throw new Error(`${location}: type mismatch`);
    }
    if (expected.length !== actual.length) {
      throw new Error(`${location}: expected array length ${expected.length}, got ${actual.length}`);
    }
    for (let i = 0; i < expected.length; i++) {
      compare(expected[i], actual[i], `${location}[${i}]`, tolerance);
    }
    return;
  }

  if (expected && actual && typeof expected === 'object' && typeof actual === 'object') {
    const expectedKeys = Object.keys(expected).sort();
    const actualKeys = Object.keys(actual).sort();
    if (expectedKeys.join('\0') !== actualKeys.join('\0')) {
      throw new Error(`${location}: expected keys [${expectedKeys.join(', ')}], got [${actualKeys.join(', ')}]`);
    }
    for (const key of expectedKeys) {
      compare(expected[key], actual[key], location === '$' ? `$.${key}` : `${location}.${key}`, tolerance);
    }
    return;
  }

  if (expected !== actual) {
    throw new Error(`${location}: expected ${JSON.stringify(expected)}, got ${JSON.stringify(actual)}`);
  }
}

function runCli(pagecoreCli, root, test) {
  const args = [
    '--file', path.resolve(root, test.input),
    '--dump-display-list', '-'
  ];
  if (test.viewport) {
    args.push('--viewport', test.viewport);
  }
  if (test.fullPage) {
    args.push('--full-page');
  }
  if (test.waitUntil) {
    args.push('--wait-until', test.waitUntil);
  }
  if (test.waitMs != null) {
    args.push('--wait-ms', String(test.waitMs));
  }

  const result = childProcess.spawnSync(pagecoreCli, args, {
    encoding: 'utf8',
    stdio: ['ignore', 'pipe', 'pipe']
  });
  if (result.status !== 0) {
    throw new Error([
      `${test.name}: pagecore_cli failed with ${result.status}`,
      result.stdout.trim(),
      result.stderr.trim()
    ].filter(Boolean).join('\n'));
  }
  try {
    return JSON.parse(result.stdout.trim());
  } catch (error) {
    throw new Error(`${test.name}: invalid display-list JSON: ${error.message}\n${result.stdout}`);
  }
}

function main() {
  const args = parseArgs(process.argv);
  const manifestPath = path.resolve(args.manifest);
  const manifest = readJson(manifestPath);
  const root = path.resolve(args.root || path.resolve(path.dirname(manifestPath), manifest.root || '..'));
  const tests = Array.isArray(manifest.tests) ? manifest.tests : [];
  const defaultTolerance = Number(manifest.tolerance ?? 0.01);
  if (tests.length === 0) {
    throw new Error(`manifest has no tests: ${manifestPath}`);
  }

  const failures = [];
  for (const test of tests) {
    try {
      const actual = normalize(runCli(args.pagecoreCli, root, test), root);
      const referencePath = path.resolve(root, test.reference);
      if (args.update) {
        fs.writeFileSync(referencePath, `${JSON.stringify(actual)}\n`);
        console.log(`UPDATE ${test.name}`);
        continue;
      }
      const expected = normalize(readJson(referencePath), root);
      compare(expected, actual, '$', Number(test.tolerance ?? defaultTolerance));
      console.log(`PASS ${test.name}`);
    } catch (error) {
      failures.push(`${test.name}: ${error.message}`);
      console.log(`FAIL ${test.name}`);
    }
  }

  console.log(`display-list reftest summary: pass=${tests.length - failures.length} fail=${failures.length}`);
  if (failures.length > 0) {
    console.error(failures.join('\n'));
    process.exit(1);
  }
}

if (require.main === module) {
  try {
    main();
  } catch (error) {
    console.error(`run_display_list_reftests.js: ${error.message}`);
    process.exit(1);
  }
}

module.exports = { isUnderRoot, compare, normalize };
