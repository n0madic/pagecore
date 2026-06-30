#!/usr/bin/env node
'use strict';

const assert = require('assert');
const path = require('path');

const rootDir = path.resolve(__dirname, '..', '..');
const modulePath = (name) => path.join(rootDir, 'src', 'dom_shim', name);

const runtimeInstaller = require(modulePath('00_runtime.js'));
const coreDefinition = require(modulePath('10_core.js'));
const eventsDefinition = require(modulePath('20_events.js'));
const domDefinition = require(modulePath('30_dom.js'));
const webDefinition = require(modulePath('40_web.js'));
const formsDefinition = require(modulePath('45_forms.js'));
const streamsDefinition = require(modulePath('50_streams.js'));
const compatDefinition = require(modulePath('60_compat.js'));

function makeBridge() {
  return {
    mutationVersion: () => 1,
    hasNode: () => true
  };
}

function makeDomBridge() {
  let nextId = 1;
  let version = 1;
  const nodes = new Map();

  function createNode(type, name) {
    const id = nextId++;
    nodes.set(id, {
      id,
      type,
      name,
      tagName: type === 1 ? name.toUpperCase() : name,
      attrs: Object.create(null),
      text: '',
      parent: null,
      children: []
    });
    return id;
  }

  const documentId = createNode(9, '#document');
  const htmlId = createNode(1, 'html');
  const headId = createNode(1, 'head');
  const bodyId = createNode(1, 'body');

  function append(parentId, childId) {
    const parent = nodes.get(parentId);
    const child = nodes.get(childId);
    if (child.parent != null) remove(child.parent, childId);
    child.parent = parentId;
    parent.children.push(childId);
    version++;
    return childId;
  }

  function insertBefore(parentId, childId, referenceId) {
    const parent = nodes.get(parentId);
    const child = nodes.get(childId);
    if (child.parent != null) remove(child.parent, childId);
    child.parent = parentId;
    const index = referenceId == null ? -1 : parent.children.indexOf(referenceId);
    if (index < 0) parent.children.push(childId);
    else parent.children.splice(index, 0, childId);
    version++;
    return childId;
  }

  function remove(parentId, childId) {
    const parent = nodes.get(parentId);
    const child = nodes.get(childId);
    const index = parent.children.indexOf(childId);
    if (index >= 0) parent.children.splice(index, 1);
    child.parent = null;
    version++;
    return childId;
  }

  append(documentId, htmlId);
  append(htmlId, headId);
  append(htmlId, bodyId);

  function descendants(id) {
    const out = [];
    for (const childId of nodes.get(id).children) {
      out.push(childId);
      out.push(...descendants(childId));
    }
    return out;
  }

  function textContent(id) {
    const node = nodes.get(id);
    if (node.type === 3 || node.type === 8) return node.text;
    return node.children.map(textContent).join('');
  }

  function setTextContent(id, value) {
    const node = nodes.get(id);
    if (node.type === 3 || node.type === 8) {
      node.text = String(value);
    } else {
      node.children.length = 0;
      if (value !== '') {
        const textId = createNode(3, '#text');
        nodes.get(textId).text = String(value);
        append(id, textId);
      }
    }
    version++;
  }

  function cloneSubtree(id, deep) {
    const node = nodes.get(id);
    const cloneId = createNode(node.type, node.name);
    const clone = nodes.get(cloneId);
    clone.tagName = node.tagName;
    clone.attrs = { ...node.attrs };
    clone.text = node.text;
    if (deep) {
      for (const childId of node.children) append(cloneId, cloneSubtree(childId, true));
    }
    return cloneId;
  }

  function attributeValue(id, name) {
    const attrs = nodes.get(id).attrs;
    return Object.prototype.hasOwnProperty.call(attrs, name) ? attrs[name] : null;
  }

  function splitSelectorList(selector) {
    return String(selector).split(',').map((part) => part.trim()).filter(Boolean);
  }

  function selectorMatches(id, selector) {
    const node = nodes.get(id);
    if (!node || node.type !== 1) return false;
    const text = selector.trim();
    if (text === '*') return true;

    let rest = text;
    const tag = /^[A-Za-z][A-Za-z0-9_-]*/.exec(rest);
    if (tag) {
      if (node.name !== tag[0].toLowerCase()) return false;
      rest = rest.slice(tag[0].length);
    }

    const idMatch = /#([A-Za-z0-9_-]+)/.exec(rest);
    if (idMatch && attributeValue(id, 'id') !== idMatch[1]) return false;

    const classMatches = [...rest.matchAll(/\.([A-Za-z0-9_-]+)/g)].map((match) => match[1]);
    const classes = (attributeValue(id, 'class') || '').split(/\s+/).filter(Boolean);
    for (const className of classMatches) {
      if (!classes.includes(className)) return false;
    }

    const attrMatches = [...rest.matchAll(/\[([A-Za-z0-9_-]+)(?:=(?:"([^"]*)"|'([^']*)'|([^\]]+)))?\]/g)];
    for (const match of attrMatches) {
      const expected = match[2] ?? match[3] ?? match[4];
      const actual = attributeValue(id, match[1]);
      if (actual == null) return false;
      if (expected !== undefined && actual !== String(expected).trim()) return false;
    }

    return true;
  }

  function querySelectorAll(id, selector) {
    const selectors = splitSelectorList(selector);
    const out = [];
    for (const candidateId of descendants(id)) {
      if (selectors.some((part) => selectorMatches(candidateId, part))) out.push(candidateId);
    }
    return out;
  }

  return {
    mutationVersion: () => version,
    hasNode: (id) => nodes.has(id),
    documentNode: () => documentId,
    documentElement: () => htmlId,
    head: () => headId,
    body: () => bodyId,
    nodeType: (id) => nodes.get(id).type,
    nodeName: (id) => nodes.get(id).tagName || nodes.get(id).name,
    tagName: (id) => nodes.get(id).tagName,
    parentNode: (id) => nodes.get(id).parent,
    childNodes: (id) => nodes.get(id).children.slice(),
    children: (id) => nodes.get(id).children.filter((childId) => nodes.get(childId).type === 1),
    describeNode: (id) => {
      if (!nodes.has(id)) return null;
      const type = nodes.get(id).type;
      return type === 1 ? { type, tag: nodes.get(id).tagName } : { type };
    },
    childNodesDescribed: (id) => nodes.get(id).children.map((childId) => {
      const type = nodes.get(childId).type;
      return type === 1 ? { id: childId, type, tag: nodes.get(childId).tagName } : { id: childId, type };
    }),
    childrenDescribed: (id) => nodes.get(id).children
      .filter((childId) => nodes.get(childId).type === 1)
      .map((childId) => ({ id: childId, type: 1, tag: nodes.get(childId).tagName })),
    isConnected: (id) => {
      for (let current = id; current != null; current = nodes.get(current).parent) {
        if (current === documentId) return true;
      }
      return false;
    },
    contains: (ancestorId, descendantId) => {
      for (let current = descendantId; current != null; current = nodes.get(current).parent) {
        if (current === ancestorId) return true;
      }
      return false;
    },
    textContent,
    setTextContent,
    createElement: (name) => createNode(1, String(name).toLowerCase()),
    createTextNode: (text) => {
      const id = createNode(3, '#text');
      nodes.get(id).text = String(text);
      return id;
    },
    createComment: (text) => {
      const id = createNode(8, '#comment');
      nodes.get(id).text = String(text);
      return id;
    },
    appendChild: append,
    insertBefore,
    removeChild: remove,
    cloneNode: cloneSubtree,
    getElementById: (idValue) => querySelectorAll(documentId, `[id="${idValue}"]`)[0] || 0,
    querySelectorAll,
    querySelector: (id, selector) => querySelectorAll(id, selector)[0] || 0,
    getAttribute: attributeValue,
    hasAttribute: (id, name) => attributeValue(id, name) != null,
    attributes: (id) => Object.keys(nodes.get(id).attrs).map((name) => ({ name, value: nodes.get(id).attrs[name] })),
    setAttribute: (id, name, value) => {
      nodes.get(id).attrs[String(name)] = String(value);
      version++;
    },
    removeAttribute: (id, name) => {
      delete nodes.get(id).attrs[String(name)];
      version++;
    },
    innerHTML: (id) => textContent(id),
    outerHTML: (id) => textContent(id),
    setInnerHTML: (id, value) => setTextContent(id, value)
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

function installDomEnvironment() {
  const logs = [];
  const ctx = {
    global: {
      location: { href: 'https://example.test/app/index.html' }
    },
    bridge: makeDomBridge(),
    host: {
      baseURL: 'https://example.test/app/index.html',
      userAgent: 'PageCoreTest/1.0',
      loadResource: () => ({ body: '', status: 200 }),
      log: (...args) => logs.push(args)
    }
  };
  const core = coreDefinition.install(ctx, Object.create(null));
  const events = eventsDefinition.install(ctx, { core });
  const dom = domDefinition.install(ctx, { core, events });
  const web = webDefinition.install(ctx, { core, events, dom });
  ctx.global.URL = web.URL;
  ctx.global.location = web.locationFromURL(ctx.host.baseURL);
  const forms = formsDefinition.install(ctx, { dom, web });
  const streams = streamsDefinition.install(ctx, { events, web });
  const compat = compatDefinition.install(ctx, { events, dom });
  return { ctx, core, events, dom, web, forms, streams, compat, logs };
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

test('events exposes standard DOMException legacy constants', () => {
  const { events } = installEvents();
  const error = new events.DOMException('missing', 'NotFoundError');

  assert.strictEqual(events.DOMException.NOT_FOUND_ERR, 8);
  assert.strictEqual(events.DOMException.prototype.NOT_FOUND_ERR, 8);
  assert.strictEqual(error.code, 8);
});

test('dom TreeWalker and NodeIterator traverse with filters', () => {
  const { dom } = installDomEnvironment();
  const { document, NodeFilter } = dom;
  const section = document.createElement('section');
  const first = document.createElement('p');
  const second = document.createElement('span');
  first.setAttribute('data-keep', 'yes');
  first.textContent = 'first';
  second.textContent = 'second';
  section.appendChild(first);
  section.appendChild(second);
  document.body.appendChild(section);

  const walker = document.createTreeWalker(section, NodeFilter.SHOW_ELEMENT, {
    acceptNode(node) {
      return node.getAttribute && node.getAttribute('data-keep') === 'yes'
        ? NodeFilter.FILTER_ACCEPT
        : NodeFilter.FILTER_SKIP;
    }
  });

  assert.strictEqual(walker.nextNode(), first);
  assert.strictEqual(walker.nextNode(), null);

  const iterator = document.createNodeIterator(section, NodeFilter.SHOW_TEXT);
  assert.strictEqual(iterator.nextNode().textContent, 'first');
  assert.strictEqual(iterator.nextNode().textContent, 'second');
  assert.strictEqual(iterator.nextNode(), null);
});

test('dom form controls compute validity and dispatch invalid events', () => {
  const { dom } = installDomEnvironment();
  const { document } = dom;
  const form = document.createElement('form');
  const input = document.createElement('input');
  let invalidEvents = 0;

  input.name = 'email';
  input.type = 'email';
  input.required = true;
  input.value = 'not-an-email';
  input.addEventListener('invalid', () => { invalidEvents++; });
  form.appendChild(input);
  document.body.appendChild(form);

  assert.strictEqual(input.willValidate, true);
  assert.strictEqual(input.validity.typeMismatch, true);
  assert.strictEqual(input.checkValidity(), false);
  assert.strictEqual(form.checkValidity(), false);
  assert.strictEqual(invalidEvents, 2);

  input.value = 'user@example.test';
  assert.strictEqual(input.validity.valid, true);
  assert.strictEqual(form.checkValidity(), true);

  input.setCustomValidity('blocked');
  assert.strictEqual(input.validity.customError, true);
  assert.strictEqual(input.validationMessage, 'blocked');
});

test('forms FormData collects successful controls in document order', () => {
  const { dom, forms } = installDomEnvironment();
  const { document } = dom;
  const form = document.createElement('form');
  const input = document.createElement('input');
  const checkbox = document.createElement('input');
  const skipped = document.createElement('input');
  const select = document.createElement('select');
  const option = document.createElement('option');

  input.name = 'title';
  input.value = 'hello';
  checkbox.name = 'published';
  checkbox.type = 'checkbox';
  checkbox.checked = true;
  checkbox.value = 'yes';
  skipped.name = 'skip';
  skipped.type = 'checkbox';
  option.value = 'a';
  option.selected = true;
  select.name = 'choice';
  select.appendChild(option);
  form.append(input, checkbox, skipped, select);
  document.body.appendChild(form);

  const data = new forms.FormData(form);
  assert.deepStrictEqual([...data], [
    ['title', 'hello'],
    ['published', 'yes'],
    ['choice', 'a']
  ]);

  data.set('title', 'updated');
  data.append('title', 'again');
  assert.deepStrictEqual(data.getAll('title'), ['updated', 'again']);
});

test('compat installs browser utilities without silent worker support', async () => {
  const { compat } = installDomEnvironment();

  assert.strictEqual(compat.btoa('abc'), 'YWJj');
  assert.strictEqual(compat.atob('YWJj'), 'abc');
  assert.throws(() => compat.btoa('\u0100'), /InvalidCharacterError/);
  assert.strictEqual(compat.CSS.escape('a b'), 'a\\ b');
  assert.strictEqual(compat.CSS.supports('display', 'block'), true);
  assert.strictEqual(compat.CSS.supports('display', 'block; color: red'), false);

  const audio = compat.Audio('/sound.ogg');
  const option = compat.Option('Label', 'value', true, true);
  assert.strictEqual(audio.localName, 'audio');
  assert.strictEqual(audio.getAttribute('src'), '/sound.ogg');
  assert.strictEqual(typeof audio.play, 'function');
  assert.strictEqual(option.localName, 'option');
  assert.strictEqual(option.value, 'value');
  assert.strictEqual(option.selected, true);
  assert.throws(() => new compat.Worker('/worker.js'), /NotSupportedError/);

  const intl = compat.createIntlFallback({});
  assert.strictEqual(new intl.PluralRules().select(2), 'other');
  await assert.rejects(
    () => new compat.ServiceWorkerContainer().register('/sw.js'),
    /NotSupportedError/);
});

test('streams support default read/write flows and reject BYOB mode explicitly', async () => {
  const { streams } = installDomEnvironment();
  const chunks = [];
  const readable = new streams.ReadableStream({
    start(controller) {
      controller.enqueue('a');
      controller.enqueue('b');
      controller.close();
    }
  });
  const writerTarget = new streams.WritableStream({
    write(chunk) { chunks.push(chunk); }
  });

  await readable.pipeTo(writerTarget);
  assert.deepStrictEqual(chunks, ['a', 'b']);

  const byobSource = new streams.ReadableStream();
  assert.throws(() => byobSource.getReader({ mode: 'byob' }), /NotSupportedError/);
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

Promise.all(testPromises).catch((error) => {
  console.error(error && error.stack ? error.stack : error);
  process.exitCode = 1;
});
