#!/usr/bin/env node
'use strict';

const assert = require('assert');
const path = require('path');

const rootDir = path.resolve(__dirname, '..', '..');
const modulePath = (name) => path.join(rootDir, 'src', 'dom_shim', name);

const runtimeInstaller = require(modulePath('00_runtime.js'));
const coreDefinition = require(modulePath('10_core.js'));
const eventsDefinition = require(modulePath('20_events.js'));
const webDefinition = require(modulePath('40_web.js'));

function makeBridge() {
  return {
    mutationVersion: () => 1,
    hasNode: () => true
  };
}

function installCore(overrides = {}) {
  const ctx = {
    global: { URL, location: { href: 'https://example.test/base/page.html' } },
    bridge: makeBridge(),
    host: {
      baseURL: 'https://example.test/base/page.html',
      loadResource: () => ({ body: '', status: 200 })
    },
    ...overrides
  };
  const core = coreDefinition.install(ctx, Object.create(null));
  return { ctx, core };
}

function installEvents(overrides = {}) {
  const logs = [];
  const { ctx, core } = installCore({
    global: {},
    host: {
      log: (...args) => logs.push(args)
    },
    ...overrides
  });
  const events = eventsDefinition.install(ctx, { core });
  return { ctx, core, events, logs };
}

function installWeb() {
  const { ctx, core, events } = installEvents();
  const dom = {
    document: { nodeType: 9, childNodes: [], textContent: '' },
    fragmentFromHTML: (html) => ({ html: String(html) })
  };
  const web = webDefinition.install(ctx, { core, events, dom });
  return { ctx, core, events, dom, web };
}

function test(name, fn) {
  try {
    fn();
    console.log(`ok - ${name}`);
  } catch (error) {
    console.error(`not ok - ${name}`);
    throw error;
  }
}

test('core resolves URLs against global location and host base URL', () => {
  const calls = [];
  const { core } = installCore({
    global: { URL, location: { href: 'https://example.test/app/index.html' } },
    host: {
      baseURL: 'https://fallback.test/root/',
      loadResource: (url, kind) => {
        calls.push({ url, kind });
        return { body: 'ok', status: 200 };
      }
    }
  });

  assert.strictEqual(
    core.absoluteURL('../style.css'),
    'https://example.test/style.css');
  assert.deepStrictEqual(
    core.loadHostResource('/asset.js', 'script'),
    { body: 'ok', status: 200 });
  assert.deepStrictEqual(calls, [{
    url: 'https://example.test/asset.js',
    kind: 'script'
  }]);
});

test('core loadHostResource reports unavailable host loader', () => {
  const { core } = installCore({ host: { baseURL: 'https://example.test/' } });
  assert.throws(
    () => core.loadHostResource('/missing', 'other'),
    /resource loading is not available/);
});

test('events dispatches EventTarget listeners and honors once option', () => {
  const { events } = installEvents();
  const target = new events.EventTarget();
  let calls = 0;

  target.addEventListener('tick', () => { calls++; }, { once: true });
  target.dispatchEvent(new events.Event('tick'));
  target.dispatchEvent(new events.Event('tick'));

  assert.strictEqual(calls, 1);
});

test('events removes signal-bound listeners when AbortController aborts', () => {
  const { events } = installEvents();
  const target = new events.EventTarget();
  const controller = new events.AbortController();
  let calls = 0;

  target.addEventListener('tick', () => { calls++; }, { signal: controller.signal });
  controller.abort('done');
  target.dispatchEvent(new events.Event('tick'));

  assert.strictEqual(calls, 0);
  assert.strictEqual(controller.signal.aborted, true);
  assert.strictEqual(controller.signal.reason, 'done');
});

test('events reports listener exceptions without aborting dispatch', () => {
  const { events, logs } = installEvents();
  const target = new events.EventTarget();
  let afterThrow = false;

  target.addEventListener('boom', () => { throw new Error('listener failed'); });
  target.addEventListener('boom', () => { afterThrow = true; });
  target.dispatchEvent(new events.Event('boom'));

  assert.strictEqual(afterThrow, true);
  assert.strictEqual(logs.length, 1);
  assert.strictEqual(logs[0][0], 'error');
  assert.match(String(logs[0][1]), /listener failed/);
});

test('web URLSearchParams preserves insertion order and encodes strings', () => {
  const { web } = installWeb();
  const params = new web.URLSearchParams('a=1&b=two+words');

  params.append('a', '2');
  params.set('space key', 'x y');

  assert.deepStrictEqual(params.getAll('a'), ['1', '2']);
  assert.strictEqual(params.get('b'), 'two words');
  assert.strictEqual(
    params.toString(),
    'a=1&b=two+words&a=2&space+key=x+y');
});

test('web Headers normalizes names and appends values', () => {
  const { web } = installWeb();
  const headers = new web.Headers({ 'Content-Type': 'text/plain' });

  headers.append('content-type', 'charset=utf-8');
  headers.set('X-Test', 'ok');

  assert.strictEqual(headers.get('CONTENT-TYPE'), 'text/plain, charset=utf-8');
  assert.strictEqual(headers.has('x-test'), true);
  assert.deepStrictEqual([...headers], [
    ['content-type', 'text/plain, charset=utf-8'],
    ['x-test', 'ok']
  ]);
});

test('web TextEncoder and TextDecoder round-trip UTF-8', () => {
  const { web } = installWeb();
  const encoded = new web.TextEncoder().encode('A\u20ac\u{1f642}');
  const decoded = new web.TextDecoder().decode(encoded);

  assert.ok(encoded instanceof Uint8Array);
  assert.strictEqual(decoded, 'A\u20ac\u{1f642}');
});

test('web timers run timeout and interval callbacks deterministically', () => {
  const { web } = installWeb();
  const calls = [];

  web.setTimeoutShim((value) => calls.push(`timeout:${value}`), 10, 'a');
  const intervalId = web.setIntervalShim(() => {
    calls.push('interval');
    if (calls.filter((value) => value === 'interval').length === 2) {
      web.clearTimer(intervalId);
    }
  }, 5);

  assert.strictEqual(web.runTimers(4), 0);
  assert.deepStrictEqual(calls, []);
  assert.strictEqual(web.runTimers(6), 3);
  assert.deepStrictEqual(calls, ['interval', 'timeout:a', 'interval']);
});

test('runtime reports missing dependencies', () => {
  const root = {};
  runtimeInstaller(root);
  root.__pagecore_dom_shim_define({
    name: 'needs-missing',
    deps: ['missing'],
    install: () => ({})
  });

  assert.throws(
    () => root.__pagecore_dom_shim_install(root),
    /Missing PageCore DOM shim module: missing/);
});

test('runtime reports duplicate modules', () => {
  const root = {};
  runtimeInstaller(root);
  root.__pagecore_dom_shim_define({ name: 'dup', deps: [], install: () => ({}) });
  root.__pagecore_dom_shim_define({ name: 'dup', deps: [], install: () => ({}) });

  assert.throws(
    () => root.__pagecore_dom_shim_install(root),
    /Duplicate PageCore DOM shim module: dup/);
});

test('runtime reports circular dependencies', () => {
  const root = {};
  runtimeInstaller(root);
  root.__pagecore_dom_shim_define({ name: 'a', deps: ['b'], install: () => ({}) });
  root.__pagecore_dom_shim_define({ name: 'b', deps: ['a'], install: () => ({}) });

  assert.throws(
    () => root.__pagecore_dom_shim_install(root),
    /Circular PageCore DOM shim module dependency: a/);
});
