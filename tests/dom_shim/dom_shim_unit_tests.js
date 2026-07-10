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
    forgetVersion: () => 1,
    hasNode: () => true
  };
}

function base64EncodeLatin1(value) {
  return Buffer.from(String(value), 'latin1').toString('base64');
}

function base64DecodeLatin1(value) {
  return Buffer.from(String(value), 'base64').toString('latin1');
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
    // This mock detaches nodes but never deletes them from `nodes`, so hasNode
    // stays true and nothing is ever forgotten; a constant forget version is the
    // faithful behaviour and keeps syncMutationCache's prune a no-op here.
    forgetVersion: () => 1,
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
    // Mirrors DomDocument::attach_shadow_root: a real node, really appended
    // into the host, so the shim's shadow-hiding/traversal logic (exercised
    // here, not by the C++ engine) sees a genuine parent/child relationship.
    attachShadowRoot: (hostId) => {
      const containerId = createNode(1, 'pc-shadowroot');
      append(hostId, containerId);
      return containerId;
    },
    // A non-zero box for any connected node, null otherwise — enough for the
    // shim to prove getBoundingClientRect() sees a real, connected node
    // rather than the zero-geometry a JS-only fragment would give it.
    elementGeometry: (id) => {
      if (!nodes.has(id)) return null;
      for (let current = id; current != null; current = nodes.get(current).parent) {
        if (current === documentId) {
          return { borderX: 5, borderY: 10, borderWidth: 120, borderHeight: 40, paddingX: 5, paddingY: 10, paddingWidth: 120, paddingHeight: 40 };
        }
      }
      return null;
    },
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
  const logs = [];
  const scheduled = [];
  const queued = [];
  const cancelled = [];
  const { ctx, core, events } = installEvents({
    host: {
      log: (...args) => logs.push(args),
      now: () => 0,
      scheduleTimer: (id, delay, repeat, relevant) => scheduled.push({ id, delay, repeat, relevant }),
      cancelTimer: (id) => cancelled.push(id),
      queueTask: (id, kind) => queued.push({ id, kind })
    }
  });
  const dom = {
    document: { nodeType: 9, childNodes: [], textContent: '' },
    fragmentFromHTML: (html) => ({ html: String(html) })
  };
  const web = webDefinition.install(ctx, { core, events, dom });
  return { ctx, core, events, dom, web, logs, scheduled, queued, cancelled };
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
      log: (...args) => logs.push(args),
      base64Encode: base64EncodeLatin1,
      base64Decode: base64DecodeLatin1
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
  ctx.global.ReadableStream = streams.ReadableStream;
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

test('core formatErrorForLog prepends error header before stack', () => {
  const { core } = installCore();
  const error = new Error('boom');
  const formatted = core.formatErrorForLog(error);
  assert.strictEqual(typeof formatted, 'string');
  assert.ok(formatted.startsWith('Error: boom\n'), `expected header prefix, got: ${formatted}`);
  assert.ok(formatted.includes(error.stack), 'formatted value should retain the original stack');

  assert.strictEqual(core.formatErrorForLog('plain'), 'plain');
  assert.strictEqual(core.formatErrorForLog(42), 42);
  const plainObject = { a: 1 };
  assert.strictEqual(core.formatErrorForLog(plainObject), plainObject);
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

test('web Response body is a readable stream consumable via getReader', async () => {
  const { web } = installDomEnvironment();
  const { Response } = web;
  const response = new Response('icon-bytes');

  assert.ok(response.body, 'body should be a ReadableStream, not the raw string');
  assert.strictEqual(typeof response.body.getReader, 'function');

  // The consumption path Angular's Fetch backend uses (getReader + read loop).
  const reader = response.body.getReader();
  let total = 0;
  for (;;) {
    const { value, done } = await reader.read();
    if (done) break;
    total += value.length;
  }
  assert.strictEqual(total, 'icon-bytes'.length);

  // text() still resolves from the raw body; an absent body stays null.
  assert.strictEqual(await new Response('hi').text(), 'hi');
  assert.strictEqual(new Response(null).body, null);
});

test('web TextEncoder and TextDecoder round-trip UTF-8', () => {
  const { web } = installWeb();
  const encoded = new web.TextEncoder().encode('A\u20ac\u{1f642}');
  const decoded = new web.TextDecoder().decode(encoded);

  assert.ok(encoded instanceof Uint8Array);
  assert.strictEqual(decoded, 'A\u20ac\u{1f642}');
});

test('web timers register with the host scheduler and run via runTask', () => {
  const { web, scheduled, cancelled } = installWeb();
  const calls = [];

  const timeoutId = web.setTimeoutShim((value) => calls.push(`timeout:${value}`), 10, 'a');
  const intervalId = web.setIntervalShim(() => calls.push('interval'), 5);

  // Delayed one-shots and intervals go through the host timer wheel; interval
  // ticks are never readiness-relevant.
  assert.deepStrictEqual(scheduled, [
    { id: timeoutId, delay: 10, repeat: false, relevant: true },
    { id: intervalId, delay: 5, repeat: true, relevant: false }
  ]);

  // The host fires timers by id; intervals stay registered, one-shots do not.
  assert.strictEqual(web.runTask(intervalId), 1);
  assert.strictEqual(web.runTask(timeoutId), 1);
  assert.strictEqual(web.runTask(timeoutId), 0);
  assert.strictEqual(web.runTask(intervalId), 1);
  assert.deepStrictEqual(calls, ['interval', 'timeout:a', 'interval']);

  web.clearTask(intervalId);
  assert.deepStrictEqual(cancelled, [intervalId]);
  assert.strictEqual(web.runTask(intervalId), 0);
});

test('web zero-delay page tasks queue to the host in insertion order', () => {
  const { web, queued, scheduled } = installWeb();
  const calls = [];

  const first = web.queuePageTask(() => calls.push('first'), 'test');
  const second = web.queuePageTask(() => calls.push('second'), 'test');

  // Zero-delay one-shots enqueue immediately instead of arming a 0ms timer.
  assert.deepStrictEqual(queued, [
    { id: first, kind: 'test' },
    { id: second, kind: 'test' }
  ]);
  assert.deepStrictEqual(scheduled, []);

  assert.strictEqual(web.runTask(first), 1);
  assert.deepStrictEqual(calls, ['first']);
  assert.strictEqual(web.runTask(second), 1);
  assert.deepStrictEqual(calls, ['first', 'second']);
});

test('web nested zero-delay timers clamp to 4ms beyond depth 5', () => {
  const { web, scheduled, queued } = installWeb();

  // runTask() runs each callback at its task's nesting level, so a chain of
  // zero-delay timers deepens one level per hop, like the HTML event loop.
  const ids = [];
  function chain(remaining) {
    if (remaining <= 0) return;
    ids.push(web.setTimeoutShim(() => chain(remaining - 1), 0));
  }
  chain(7);
  for (let i = 0; i < ids.length; i++) web.runTask(ids[i]);

  // Levels 1-5 enqueue immediately at 0ms; levels 6+ clamp to a >=4ms timer.
  assert.strictEqual(queued.length, 5);
  assert.deepStrictEqual(scheduled.map((entry) => entry.delay), [4, 4]);
});

test('events exposes standard DOMException legacy constants', () => {
  const { events } = installEvents();
  const error = new events.DOMException('missing', 'NotFoundError');

  assert.strictEqual(events.DOMException.NOT_FOUND_ERR, 8);
  assert.strictEqual(events.DOMException.prototype.NOT_FOUND_ERR, 8);
  assert.strictEqual(error.code, 8);
});

test('dom HTMLScriptElement tracks dynamic async state', () => {
  const { dom } = installDomEnvironment();
  const { document } = dom;
  const script = document.createElement('script');
  const div = document.createElement('div');

  assert.strictEqual(script.async, true);
  assert.strictEqual(script.hasAttribute('async'), false);

  script.async = false;
  assert.strictEqual(script.async, false);
  assert.strictEqual(script.hasAttribute('async'), false);

  script.async = true;
  assert.strictEqual(script.async, true);
  assert.strictEqual(script.hasAttribute('async'), true);
  assert.strictEqual(script.defer, false);

  script.defer = true;
  assert.strictEqual(script.defer, true);
  assert.strictEqual(script.hasAttribute('defer'), true);
  assert.strictEqual('async' in div, false);
  assert.strictEqual('defer' in div, false);
});

test('dom HTMLAnchorElement exposes URL decomposition members', () => {
  const { dom } = installDomEnvironment();
  const { document } = dom;
  const anchor = document.createElement('a');

  // Relative href resolves against the document base URL
  // (https://example.test/app/index.html).
  anchor.setAttribute('href', '/emoji?q=smile#top');
  assert.strictEqual(anchor.href, 'https://example.test/emoji?q=smile#top');
  assert.strictEqual(anchor.protocol, 'https:');
  assert.strictEqual(anchor.host, 'example.test');
  assert.strictEqual(anchor.hostname, 'example.test');
  assert.strictEqual(anchor.pathname, '/emoji');
  assert.strictEqual(anchor.search, '?q=smile');
  assert.strictEqual(anchor.hash, '#top');
  assert.strictEqual(anchor.origin, 'https://example.test');

  // Regression: AngularJS urlResolve() reads pathname.charAt(0); pathname must
  // be a string starting with '/', never undefined (blank-page bug on
  // emojicopy.com).
  assert.strictEqual(anchor.pathname.charAt(0), '/');

  // Missing href attribute yields empty strings / ':' rather than undefined.
  const bare = document.createElement('a');
  assert.strictEqual(bare.href, '');
  assert.strictEqual(bare.protocol, ':');
  assert.strictEqual(bare.pathname, '');
  assert.doesNotThrow(() => bare.pathname.charAt(0));

  // Setters round-trip through the href attribute.
  anchor.hash = 'bottom';
  assert.strictEqual(anchor.hash, '#bottom');
  assert.strictEqual(anchor.getAttribute('href'), 'https://example.test/emoji?q=smile#bottom');

  // <area> shares the same HTMLHyperlinkElementUtils behaviour.
  const area = document.createElement('area');
  area.href = 'https://cdn.example.test/a/b/c.png';
  assert.strictEqual(area.pathname, '/a/b/c.png');
  assert.strictEqual(area.protocol, 'https:');
});

test('dom MutationObserver uses explicit delivery hook', () => {
  const { dom, events } = installDomEnvironment();
  const { document } = dom;
  const recordsSeen = [];
  const observer = new events.MutationObserver((records) => {
    recordsSeen.push(records.map((record) => record.type).join(','));
  });

  observer.observe(document.body, { childList: true });
  document.body.appendChild(document.createElement('span'));

  assert.deepStrictEqual(recordsSeen, []);
  assert.strictEqual(events.deliverMutationObservers(), 1);
  assert.deepStrictEqual(recordsSeen, ['childList']);
  assert.strictEqual(events.deliverMutationObservers(), 0);
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

test('dom compareDocumentPosition reports containment and document order', () => {
  const { dom } = installDomEnvironment();
  const { document } = dom;
  const CONTAINED_BY = 16;
  const CONTAINS = 8;
  const FOLLOWING = 4;
  const PRECEDING = 2;
  const DISCONNECTED = 1;

  const section = document.createElement('section');
  const first = document.createElement('p');
  const second = document.createElement('span');
  const child = document.createElement('em');
  first.appendChild(child);
  section.appendChild(first);
  section.appendChild(second);
  document.body.appendChild(section);

  assert.strictEqual(first.compareDocumentPosition(first), 0);
  // The Angular sanitizer's DOM-clobbering guard relies on this exact bit.
  assert.strictEqual(first.compareDocumentPosition(child) & CONTAINED_BY, CONTAINED_BY);
  assert.strictEqual(child.compareDocumentPosition(first) & CONTAINS, CONTAINS);
  assert.strictEqual(child.compareDocumentPosition(first) & PRECEDING, PRECEDING);
  assert.strictEqual(first.compareDocumentPosition(second) & FOLLOWING, FOLLOWING);
  assert.strictEqual(second.compareDocumentPosition(first) & PRECEDING, PRECEDING);

  const detached = document.createElement('div');
  assert.strictEqual(section.compareDocumentPosition(detached) & DISCONNECTED, DISCONNECTED);
});

test('dom setAttributeNS folds namespace into a qualified name', () => {
  const { dom } = installDomEnvironment();
  const { document } = dom;
  const xlink = 'http://www.w3.org/1999/xlink';
  const use = document.createElement('use');

  // Must not throw (the missing method previously crashed Angular's renderer).
  use.setAttributeNS(xlink, 'xlink:href', '#icon-circle-right');
  assert.strictEqual(use.getAttribute('xlink:href'), '#icon-circle-right');
  assert.strictEqual(use.getAttributeNS(xlink, 'href'), '#icon-circle-right');
  assert.strictEqual(use.hasAttributeNS(xlink, 'href'), true);

  use.removeAttributeNS(xlink, 'href');
  assert.strictEqual(use.getAttribute('xlink:href'), null);
  assert.strictEqual(use.hasAttributeNS(xlink, 'href'), false);
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

test('compat installs browser utilities', async () => {
  const { compat } = installDomEnvironment();

  assert.strictEqual(compat.btoa('abc'), 'YWJj');
  assert.strictEqual(compat.atob('YWJj'), 'abc');
  assert.strictEqual(compat.btoa('\xff'), '/w==');
  assert.strictEqual(compat.atob('/w==').charCodeAt(0), 255);
  assert.strictEqual(compat.atob('Y W\nJj'), 'abc');
  assert.strictEqual(compat.atob('YWI'), 'ab');
  assert.throws(() => compat.btoa('\u0100'), /InvalidCharacterError/);
  assert.throws(() => compat.atob('YQ='), /InvalidCharacterError/);
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

  const intl = compat.createIntlFallback({});
  assert.strictEqual(new intl.PluralRules().select(2), 'other');
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

test('dom nextSibling/previousSibling traverse DocumentFragment children', () => {
  const { dom } = installDomEnvironment();
  const { document } = dom;
  const fragment = document.createDocumentFragment();
  const a = document.createElement('a');
  const b = document.createElement('b');
  const c = document.createElement('i');
  fragment.appendChild(a);
  fragment.appendChild(b);
  fragment.appendChild(c);

  // Regression: these getters used to call parent._liveId() on the fragment
  // (which is not a Node), throwing a TypeError for any fragment/shadow child.
  assert.strictEqual(a.nextSibling, b);
  assert.strictEqual(b.nextSibling, c);
  assert.strictEqual(c.nextSibling, null);
  assert.strictEqual(c.previousSibling, b);
  assert.strictEqual(a.previousSibling, null);
});

test('dom attachShadow builds a real connected node the host hides', () => {
  const { dom } = installDomEnvironment();
  const { document } = dom;
  const host = document.createElement('shadow-host');
  document.body.appendChild(host);

  const root = host.attachShadow({ mode: 'open' });
  const child = document.createElement('span');
  root.appendChild(child);

  assert.ok(root instanceof dom.ShadowRoot);
  assert.strictEqual(root.host, host);
  assert.strictEqual(child.parentNode, root);
  assert.strictEqual(child.getRootNode(), root);

  // A real, connected bridge node: offsetWidth/offsetHeight (backed by the
  // same elementGeometry() bridge call as getBoundingClientRect()) resolve
  // non-zero, unlike the always-zero geometry a JS-only fragment child would give.
  assert.ok(child.offsetWidth > 0 && child.offsetHeight > 0,
    `expected non-zero geometry, got ${child.offsetWidth}x${child.offsetHeight}`);

  // The shadow container is a real bridge child of host, but must never leak
  // into the host's own light-DOM traversal.
  assert.strictEqual(host.childNodes.length, 0);
  assert.strictEqual(host.children.length, 0);
});

test('web URLSearchParams sort uses UTF-16 code-unit order', () => {
  const { web } = installWeb();
  const params = new web.URLSearchParams('b=1&a=2&B=3&a=4');
  params.sort();
  // Code units: 'B'(66) < 'a'(97) < 'b'(98); equal keys keep insertion order.
  // localeCompare (the old behavior) would order these differently.
  assert.strictEqual(params.toString(), 'B=3&a=2&a=4&b=1');
});

test('runtime removes native bridges even when a module install throws', () => {
  const root = { __dom: { tag: 'dom' }, __host: { tag: 'host' } };
  runtimeInstaller(root);
  root.__pagecore_dom_shim_define({
    name: 'boom',
    deps: [],
    install: () => { throw new Error('install failed'); }
  });

  assert.throws(() => root.__pagecore_dom_shim_install(root), /install failed/);
  assert.strictEqual(root.__dom, undefined, 'raw __dom bridge must be removed even on a failed install');
  assert.strictEqual(root.__host, undefined, 'raw __host bridge must be removed even on a failed install');
});

test('dom Object.prototype.toString reports real DOM interface names', () => {
  const { dom } = installDomEnvironment();
  const { document } = dom;
  const tag = (value) => Object.prototype.toString.call(value);

  // Regression: without Symbol.toStringTag these all reported "[object Object]",
  // a trivial tell that the DOM is shimmed.
  assert.strictEqual(tag(document), '[object HTMLDocument]');
  assert.strictEqual(tag(document.createElement('div')), '[object HTMLDivElement]');
  assert.strictEqual(tag(document.createElement('a')), '[object HTMLAnchorElement]');
  assert.strictEqual(tag(document.createElement('input')), '[object HTMLInputElement]');
  assert.strictEqual(tag(document.createElement('span')), '[object HTMLSpanElement]');
  assert.strictEqual(tag(document.createTextNode('x')), '[object Text]');
  assert.strictEqual(tag(document.createComment('x')), '[object Comment]');
  assert.strictEqual(tag(document.createDocumentFragment()), '[object DocumentFragment]');
  // A hyphenated unknown tag falls back to the generic HTMLElement interface.
  assert.strictEqual(tag(document.createElement('unknown-xyz')), '[object HTMLElement]');

  // The tag is inherited (non-own) and non-enumerable, like a real interface.
  const div = document.createElement('div');
  assert.strictEqual(Object.prototype.hasOwnProperty.call(div, Symbol.toStringTag), false);
});

test('dom getRootNode returns the actual root for detached and attached nodes', () => {
  const { dom } = installDomEnvironment();
  const { document } = dom;

  const parent = document.createElement('div');
  const child = document.createElement('span');
  parent.appendChild(child);

  // Regression: a detached subtree used to report the document as its root.
  assert.strictEqual(child.getRootNode(), parent);
  assert.strictEqual(parent.getRootNode(), parent);

  document.body.appendChild(parent);
  assert.strictEqual(child.getRootNode(), document);
});

test('dom form control dirty flags keep defaults independent and reset restores them', () => {
  const { dom } = installDomEnvironment();
  const { document } = dom;
  const form = document.createElement('form');

  const checkbox = document.createElement('input');
  checkbox.type = 'checkbox';
  checkbox.name = 'agree';

  const textarea = document.createElement('textarea');
  textarea.name = 'note';
  textarea.textContent = 'default text';

  const select = document.createElement('select');
  select.name = 'choice';
  const optionA = document.createElement('option');
  optionA.value = 'a';
  optionA.setAttribute('selected', '');
  const optionB = document.createElement('option');
  optionB.value = 'b';
  select.appendChild(optionA);
  select.appendChild(optionB);

  form.appendChild(checkbox);
  form.appendChild(textarea);
  form.appendChild(select);
  document.body.appendChild(form);

  // Baseline defaults.
  assert.strictEqual(checkbox.checked, false);
  assert.strictEqual(textarea.value, 'default text');
  assert.strictEqual(optionA.selected, true);
  assert.strictEqual(select.value, 'a');

  // Mutating the IDL properties must not touch the defaults or content attributes.
  checkbox.checked = true;
  textarea.value = 'typed text';
  optionA.selected = false;
  optionB.selected = true;

  assert.strictEqual(checkbox.checked, true);
  assert.strictEqual(checkbox.defaultChecked, false, 'checked must not flip defaultChecked');
  assert.strictEqual(checkbox.hasAttribute('checked'), false, 'checked must not write the content attribute');
  assert.strictEqual(textarea.value, 'typed text');
  assert.strictEqual(textarea.defaultValue, 'default text', 'value must not overwrite defaultValue');
  assert.strictEqual(optionA.defaultSelected, true, 'selected must not flip defaultSelected');
  assert.strictEqual(optionA.hasAttribute('selected'), true);
  assert.strictEqual(select.value, 'b');

  // reset() clears the dirty flags, restoring every default.
  form.reset();
  assert.strictEqual(checkbox.checked, false);
  assert.strictEqual(textarea.value, 'default text');
  assert.strictEqual(optionA.selected, true);
  assert.strictEqual(optionB.selected, false);
  assert.strictEqual(select.value, 'a');
});

test('dom MutationObserver re-observing a target replaces its options', () => {
  const { dom, events } = installDomEnvironment();
  const { document } = dom;
  const seen = [];
  const observer = new events.MutationObserver((records) => {
    for (const record of records) seen.push(record.type);
  });

  observer.observe(document.body, { childList: true });
  observer.observe(document.body, { attributes: true });

  // childList is no longer observed after the replacing observe() call.
  document.body.appendChild(document.createElement('span'));
  assert.strictEqual(events.deliverMutationObservers(), 0);
  assert.deepStrictEqual(seen, []);

  // attributes IS observed.
  document.body.setAttribute('data-x', '1');
  assert.strictEqual(events.deliverMutationObservers(), 1);
  assert.deepStrictEqual(seen, ['attributes']);
});

test('dom MutationObserver is retained by its observed node (fire-and-forget delivery)', () => {
  const { dom, events } = installDomEnvironment();
  const { document } = dom;
  const seen = [];

  // Regression: the "new MutationObserver(cb).observe(node)" form keeps no
  // reference to the observer. A purely weak global registry let an aggressive GC
  // (as in the embedded QuickJS engine) collect it before any record was queued,
  // dropping every delivery. The observed node must strongly anchor the observer,
  // so its reachability — not GC timing — governs the observer's lifetime.
  new events.MutationObserver((records) => {
    for (const record of records) seen.push(record.type);
  }).observe(document.body, { childList: true, attributes: true, subtree: true });

  const anchors = document.body.__mutationObserverAnchors;
  assert.ok(anchors && anchors.size === 1, 'the observed node must strongly reference its observer');

  document.body.appendChild(document.createElement('span'));
  document.body.setAttribute('data-order', '1');
  assert.strictEqual(events.deliverMutationObservers(), 1);
  assert.deepStrictEqual(seen, ['childList', 'attributes']);

  // disconnect() releases the node → observer anchor so it can be collected.
  const observer = new events.MutationObserver(() => {});
  observer.observe(document.body, { childList: true });
  assert.strictEqual(document.body.__mutationObserverAnchors.size, 2);
  observer.disconnect();
  assert.strictEqual(document.body.__mutationObserverAnchors.size, 1);
});

test('web URL href and toString serialize embedded credentials', () => {
  const { web } = installWeb();

  const url = new web.URL('https://user:pass@example.com/path?q=1#frag');
  assert.strictEqual(url.href, 'https://user:pass@example.com/path?q=1#frag');
  assert.strictEqual(url.toString(), 'https://user:pass@example.com/path?q=1#frag');
  assert.strictEqual(url.username, 'user');
  assert.strictEqual(url.password, 'pass');
  // origin stays userinfo-free (correct per the URL spec).
  assert.strictEqual(url.origin, 'https://example.com');

  // Username without password.
  assert.strictEqual(new web.URL('https://user@example.com/').href, 'https://user@example.com/');
  // No credentials → unchanged.
  assert.strictEqual(new web.URL('https://example.com/x').href, 'https://example.com/x');
});

test('web XHR responseType json yields null (not error) on malformed body', () => {
  const env = installDomEnvironment();
  const { web } = env;
  env.ctx.host.loadResource = () => ({ body: 'not: valid json', status: 200 });

  const xhr = new web.XMLHttpRequest();
  const fired = [];
  xhr.addEventListener('load', () => fired.push('load'));
  xhr.addEventListener('error', () => fired.push('error'));
  xhr.open('GET', '/data.json', false);
  xhr.responseType = 'json';
  xhr.send();

  assert.strictEqual(xhr.status, 200);
  assert.strictEqual(xhr.response, null);
  assert.deepStrictEqual(fired, ['load']);

  // Valid JSON still parses into a value.
  env.ctx.host.loadResource = () => ({ body: '{"ok":true}', status: 200 });
  const good = new web.XMLHttpRequest();
  good.open('GET', '/ok.json', false);
  good.responseType = 'json';
  good.send();
  assert.deepStrictEqual(good.response, { ok: true });
});

test('web XHR serializes a FormData body as multipart/form-data', () => {
  const env = installDomEnvironment();
  const { web, forms } = env;
  let captured = null;
  env.ctx.host.loadResource = (url, kind, method, body, headers) => {
    captured = { body, headers };
    return { body: '', status: 200 };
  };

  const data = new forms.FormData();
  data.append('title', 'hello');
  data.append('note', 'a b');

  const xhr = new web.XMLHttpRequest();
  xhr.open('POST', '/submit', false);
  xhr.send(data);

  // Regression: a FormData body used to be transmitted as "[object FormData]".
  const contentType = captured.headers.find((pair) => pair[0] === 'content-type');
  assert.ok(contentType, 'a content-type header is set for a FormData body');
  assert.match(contentType[1], /^multipart\/form-data; boundary=----PageCoreFormBoundary/);
  const boundary = contentType[1].split('boundary=')[1];
  assert.ok(captured.body.includes(`--${boundary}\r\n`), 'body uses the declared boundary');
  assert.match(captured.body, /Content-Disposition: form-data; name="title"\r\n\r\nhello\r\n/);
  assert.match(captured.body, /Content-Disposition: form-data; name="note"\r\n\r\na b\r\n/);
  assert.ok(captured.body.endsWith(`--${boundary}--\r\n`), 'body ends with the closing boundary');
});

Promise.all(testPromises).catch((error) => {
  console.error(error && error.stack ? error.stack : error);
  process.exitCode = 1;
});
