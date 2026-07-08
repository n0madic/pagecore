#!/usr/bin/env node
'use strict';

const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');

const {
  generateManifest,
  manifestName,
  normalizePrefix,
  pathSkipReason,
  sourceSkipReason
} = require(path.resolve(__dirname, '..', '..', 'tools', 'generate_wpt_manifest.js'));

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

function writeFile(root, relativePath, body) {
  const absolutePath = path.join(root, relativePath);
  fs.mkdirSync(path.dirname(absolutePath), { recursive: true });
  fs.writeFileSync(absolutePath, body);
}

function withTempWptTree(fn) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'pagecore-wpt-manifest-'));
  try {
    return fn(root);
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
}

test('normalizePrefix accepts leading slash and adds a trailing slash', () => {
  assert.strictEqual(normalizePrefix('/dom/nodes'), 'dom/nodes/');
  assert.strictEqual(normalizePrefix('url/'), 'url/');
});

test('manifestName removes WPT-generated page suffixes', () => {
  assert.strictEqual(manifestName('url/urlsearchparams-get.any.js'), 'url/urlsearchparams-get');
  assert.strictEqual(manifestName('dom/nodes/Document-createElement.window.js'), 'dom/nodes/Document-createElement');
  assert.strictEqual(manifestName('dom/nodes/attributes-are-nodes.html'), 'dom/nodes/attributes-are-nodes');
});

test('pathSkipReason rejects unsupported runner paths', () => {
  assert.strictEqual(pathSkipReason('url/resources/data.json'), 'unsupported extension');
  assert.strictEqual(pathSkipReason('dom/nodes/support/helper.html'), 'support resource');
  assert.strictEqual(pathSkipReason('dom/nodes/foo.worker.js'), 'unsupported extension');
  assert.strictEqual(pathSkipReason('css/cssom-view/foo-ref.html'), 'reference file');
  assert.strictEqual(pathSkipReason('url/url.sub.html'), 'wptserve template');
});

test('sourceSkipReason rejects unsupported WPT infrastructure', () => {
  assert.strictEqual(sourceSkipReason('fetch("resources/data.py?x=1")'), 'python handler');
  assert.strictEqual(sourceSkipReason('const worker = new Worker("worker.js");'), 'worker API');
  assert.strictEqual(sourceSkipReason('const ws = new WebSocket("wss://example.test");'), 'persistent network API');
  assert.strictEqual(sourceSkipReason('get_host_info().HTTPS_ORIGIN'), 'multi-origin helper');
  assert.strictEqual(sourceSkipReason('document.createElement("iframe")'), 'iframe navigation');
  assert.strictEqual(sourceSkipReason('anchor.href = "javascript:globalThis.ok = true"'), 'javascript URL navigation');
});

test('generateManifest emits sorted all-pass tests and rendering markers', () => withTempWptTree((root) => {
  writeFile(root, 'resources/testharness.js', '');
  writeFile(root, 'url/b.any.js', 'test(() => {}, "b");\n');
  writeFile(root, 'url/a.window.js', 'test(() => {}, "a");\n');
  writeFile(root, 'url/resources/helper.window.js', 'test(() => {}, "helper");\n');
  writeFile(root, 'url/c.window.js', 'fetch("resources/server.py");\n');
  writeFile(root, 'dom/nodes/child.html', '<script src="/resources/testharness.js"></script>\n');
  writeFile(root, 'css/cssom-view/geometry.window.js', 'test(() => {}, "geometry");\n');

  const manifest = generateManifest({
    root,
    prefixes: ['url/', 'dom/nodes/', 'css/cssom-view/'],
    requiresRenderingPrefixes: ['css/cssom-view/'],
    includeUnsupported: false,
    limit: null
  });

  assert.deepStrictEqual(manifest.tests.map((entry) => entry.path), [
    '/css/cssom-view/geometry.window.js',
    '/dom/nodes/child.html',
    '/url/a.window.js',
    '/url/b.any.js'
  ]);
  assert.deepStrictEqual(manifest.tests.map((entry) => entry.expected), [
    { harness: 'OK', subtests: 'all-pass' },
    { harness: 'OK', subtests: 'all-pass' },
    { harness: 'OK', subtests: 'all-pass' },
    { harness: 'OK', subtests: 'all-pass' }
  ]);
  assert.strictEqual(manifest.tests[0].requiresRendering, true);
  assert.strictEqual(manifest.tests[1].requiresRendering, undefined);
  assert.strictEqual(manifest.skipped['support resource'], 1);
  assert.strictEqual(manifest.skipped['python handler'], 1);
}));

test('generateManifest skips HTML tests with missing local helper scripts', () => withTempWptTree((root) => {
  writeFile(root, 'resources/testharness.js', '');
  writeFile(root, 'css/cssom-view/has-helper.html', [
    '<script src="/resources/testharness.js"></script>',
    '<script src="resources/present-helper.js"></script>'
  ].join('\n'));
  writeFile(root, 'css/cssom-view/missing-helper.html', [
    '<script src="/resources/testharness.js"></script>',
    '<script src="resources/missing-helper.js"></script>'
  ].join('\n'));
  writeFile(root, 'css/cssom-view/resources/present-helper.js', '');

  const manifest = generateManifest({
    root,
    prefixes: ['css/cssom-view/'],
    requiresRenderingPrefixes: ['css/cssom-view/'],
    includeUnsupported: false,
    limit: null
  });

  assert.deepStrictEqual(manifest.tests.map((entry) => entry.path), [
    '/css/cssom-view/has-helper.html'
  ]);
  assert.strictEqual(manifest.skipped['missing helper script'], 1);
}));

test('generateManifest skips HTML tests with missing local stylesheet resources', () => withTempWptTree((root) => {
  writeFile(root, 'resources/testharness.js', '');
  writeFile(root, 'fonts/ahem.css', '');
  writeFile(root, 'css/cssom-view/has-font.html', [
    '<script src="/resources/testharness.js"></script>',
    '<link rel="stylesheet" href="/fonts/ahem.css">'
  ].join('\n'));
  writeFile(root, 'css/cssom-view/missing-font.html', [
    '<script src="/resources/testharness.js"></script>',
    '<link href="/fonts/missing.css" rel="stylesheet">'
  ].join('\n'));

  const manifest = generateManifest({
    root,
    prefixes: ['css/cssom-view/'],
    requiresRenderingPrefixes: ['css/cssom-view/'],
    includeUnsupported: false,
    limit: null
  });

  assert.deepStrictEqual(manifest.tests.map((entry) => entry.path), [
    '/css/cssom-view/has-font.html'
  ]);
  assert.strictEqual(manifest.skipped['missing stylesheet resource'], 1);
}));

test('generateManifest honors limit after sorting', () => withTempWptTree((root) => {
  writeFile(root, 'url/b.any.js', 'test(() => {}, "b");\n');
  writeFile(root, 'url/a.any.js', 'test(() => {}, "a");\n');

  const manifest = generateManifest({
    root,
    prefixes: ['url/'],
    requiresRenderingPrefixes: [],
    includeUnsupported: false,
    limit: 1
  });

  assert.deepStrictEqual(manifest.tests.map((entry) => entry.path), ['/url/a.any.js']);
}));

Promise.all(testPromises).catch((error) => {
  console.error(error && error.stack ? error.stack : error);
  process.exitCode = 1;
});
