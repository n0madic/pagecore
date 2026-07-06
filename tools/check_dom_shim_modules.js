#!/usr/bin/env node
'use strict';

const childProcess = require('child_process');
const path = require('path');

const files = process.argv.slice(2).map((file) => path.resolve(file));
if (files.length === 0) {
  throw new Error('Usage: check_dom_shim_modules.js <dom-shim-js>...');
}

const expected = new Map([
  ['10_core.js', { name: 'core', deps: [] }],
  ['20_events.js', { name: 'events', deps: ['core'] }],
  ['30_dom.js', { name: 'dom', deps: ['core', 'events'] }],
  ['40_web.js', { name: 'web', deps: ['core', 'events', 'dom'] }],
  ['45_forms.js', { name: 'forms', deps: ['dom', 'web'] }],
  ['50_streams.js', { name: 'streams', deps: ['events', 'web'] }],
  ['60_compat.js', { name: 'compat', deps: ['events', 'dom'] }],
  ['90_install.js', { name: 'install', deps: ['core', 'events', 'dom', 'web', 'forms', 'streams', 'compat'] }]
]);

function runNode(args) {
  const result = childProcess.spawnSync(process.execPath, args, {
    encoding: 'utf8',
    stdio: 'pipe'
  });
  if (result.status !== 0) {
    throw new Error([
      `node ${args.join(' ')} failed with ${result.status}`,
      result.stdout,
      result.stderr
    ].filter(Boolean).join('\n'));
  }
}

function sameArray(left, right) {
  return left.length === right.length && left.every((value, index) => value === right[index]);
}

for (const file of files) {
  runNode(['--check', file]);
  runNode([file]);
}

// Every known module (plus the runtime and boot files) must be present in the
// input set; silently omitting one would let a dependency regression pass.
const requiredFiles = new Set(['00_runtime.js', '99_boot.js', ...expected.keys()]);
const providedFiles = new Set(files.map((file) => path.basename(file)));
const missing = [...requiredFiles].filter((name) => !providedFiles.has(name));
if (missing.length > 0) {
  throw new Error(`Missing DOM shim module file(s): ${missing.join(', ')}`);
}

for (const file of files) {
  const base = path.basename(file);
  const exported = require(file);

  if (base === '00_runtime.js') {
    if (typeof exported !== 'function') {
      throw new Error(`${base} must export the runtime installer`);
    }
    continue;
  }

  if (base === '99_boot.js') {
    if (!exported || typeof exported.boot !== 'function') {
      throw new Error(`${base} must export boot()`);
    }
    continue;
  }

  const contract = expected.get(base);
  if (!contract) throw new Error(`Unexpected DOM shim module file: ${base}`);
  if (!exported || exported.name !== contract.name || typeof exported.install !== 'function') {
    throw new Error(`${base} must export module definition ${contract.name}`);
  }
  if (!Array.isArray(exported.deps) || !sameArray(exported.deps, contract.deps)) {
    throw new Error(`${base} has invalid deps: [${(exported.deps || []).join(',')}]`);
  }
}

// Build the actual dependency graph from the module definitions and detect any
// cycle directly (a DFS topological check), instead of trusting only the expected
// table above. A new cycle the runtime installer would reject at boot is then
// caught at build time too, and the tool's name is honest.
const graph = new Map();
for (const file of files) {
  const base = path.basename(file);
  if (base === '00_runtime.js' || base === '99_boot.js') continue;
  const exported = require(file);
  graph.set(exported.name, Array.isArray(exported.deps) ? exported.deps.slice() : []);
}

const VISITING = 1;
const DONE = 2;
const state = new Map();
const stack = [];
function visit(name) {
  const current = state.get(name);
  if (current === DONE) return;
  if (current === VISITING) {
    const start = stack.indexOf(name);
    const cycle = stack.slice(start >= 0 ? start : 0).concat(name);
    throw new Error(`Dependency cycle detected: ${cycle.join(' -> ')}`);
  }
  state.set(name, VISITING);
  stack.push(name);
  for (const dep of graph.get(name) || []) {
    if (!graph.has(dep)) {
      throw new Error(`Module '${name}' depends on unknown module '${dep}'`);
    }
    visit(dep);
  }
  stack.pop();
  state.set(name, DONE);
}
for (const name of graph.keys()) visit(name);
