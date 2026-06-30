(function(global) {
  'use strict';

  const bridge = global.__dom;
  const host = global.__host;
  const wrapperCache = new Map();
  let observedMutationVersion = bridge.mutationVersion();
  let activeElement = null;
  let suppressMutationRecords = 0;
  let customElementsRegistry = null;
  let pendingCustomElementNodeId = null;
  let nextCssRootId = 1;
  const cookieJar = new Map();
  const absoluteURLCache = new Map();
  const stylesheetTextCache = new Map();
  const stylesheetCache = new Map();
  let stylesheetListKey = '';
  let stylesheetListCache = null;
  let adoptedStyleSheets = [];
  const computedStyleCache = new Map();
  let cssomVersion = 0;
  let stylesheetListMutationVersion = -1;
  let cascadeEntriesKey = '';
  let cascadeEntriesCache = [];
  let documentReadyState = 'loading';

  const DOM_EXCEPTION_CODES = {
    IndexSizeError: 1,
    HierarchyRequestError: 3,
    WrongDocumentError: 4,
    InvalidCharacterError: 5,
    NoModificationAllowedError: 7,
    NotFoundError: 8,
    NotSupportedError: 9,
    InUseAttributeError: 10,
    InvalidStateError: 11,
    SyntaxError: 12,
    InvalidModificationError: 13,
    NamespaceError: 14,
    InvalidAccessError: 15,
    SecurityError: 18,
    NetworkError: 19,
    AbortError: 20,
    URLMismatchError: 21,
    QuotaExceededError: 22,
    TimeoutError: 23,
    InvalidNodeTypeError: 24,
    DataCloneError: 25
  };

  function defineValue(target, property, value, enumerable = false) {
    Object.defineProperty(target, property, {
      value,
      writable: true,
      configurable: true,
      enumerable
    });
  }

  function defineMethod(target, property, fn) {
    if (typeof target[property] === 'function') return;
    defineValue(target, property, fn);
  }

  function syncMutationCache() {
    const current = bridge.mutationVersion();
    if (current === observedMutationVersion) return;

    for (const [id] of wrapperCache) {
      if (!bridge.hasNode(id)) wrapperCache.delete(id);
    }

    observedMutationVersion = current;
  }

  function afterMutation(value, record = null) {
    syncMutationCache();
    if (record && suppressMutationRecords === 0) queueMutation(record);
    return value;
  }

  function setDocumentReadyState(value) {
    documentReadyState = String(value);
  }

  function isNodeWrapper(value) {
    return Boolean(value && typeof value.__id === 'number');
  }

  function attachNodeId(target, id) {
    Object.defineProperty(target, '__id', {
      value: id,
      configurable: true
    });
    return target;
  }

  function assertNode(value) {
    if (!isNodeWrapper(value)) {
      throw new TypeError('Expected a DOM Node');
    }
    if (!bridge.hasNode(value.__id)) {
      throw new TypeError('DOM Node is no longer valid');
    }
    return value;
  }

  function liveId(value) {
    return assertNode(value).__id;
  }

  function memo(target, property, factory) {
    if (!Object.prototype.hasOwnProperty.call(target, property)) {
      Object.defineProperty(target, property, {
        value: factory(),
        configurable: true
      });
    }
    return target[property];
  }

  function toArray(ids) {
    return ids.map((id) => wrapNode(id)).filter(Boolean);
  }

  class DOMException extends Error {
    constructor(message = '', name = 'Error') {
      super(String(message));
      this.name = String(name);
      this.code = DOM_EXCEPTION_CODES[this.name] || 0;
    }
  }

  for (const [name, code] of Object.entries(DOM_EXCEPTION_CODES)) {
    defineValue(DOMException, `${name}_CODE`, code, true);
  }

  class Event {
    constructor(type, init = {}) {
      defineValue(this, 'type', String(type));
      defineValue(this, 'bubbles', Boolean(init.bubbles));
      defineValue(this, 'cancelable', Boolean(init.cancelable));
      defineValue(this, 'composed', Boolean(init.composed));
      defineValue(this, 'defaultPrevented', false);
      defineValue(this, 'target', null);
      defineValue(this, 'currentTarget', null);
      defineValue(this, 'cancelBubble', false);
      defineValue(this, 'eventPhase', 0);
      defineValue(this, 'isTrusted', false);
      defineValue(this, 'timeStamp', Date.now());
      defineValue(this, '_immediateStopped', false);
      defineValue(this, '_path', null);
    }

    preventDefault() {
      if (this.cancelable) this.defaultPrevented = true;
    }

    stopPropagation() {
      this.cancelBubble = true;
    }

    stopImmediatePropagation() {
      this.cancelBubble = true;
      this._immediateStopped = true;
    }

    composedPath() {
      return this._path ? [...this._path] : eventPath(this.target || this.currentTarget);
    }

    initEvent(type, bubbles = false, cancelable = false) {
      this.type = String(type);
      this.bubbles = Boolean(bubbles);
      this.cancelable = Boolean(cancelable);
      this.defaultPrevented = false;
      this.cancelBubble = false;
      this._immediateStopped = false;
    }
  }

  defineValue(Event, 'NONE', 0, true);
  defineValue(Event, 'CAPTURING_PHASE', 1, true);
  defineValue(Event, 'AT_TARGET', 2, true);
  defineValue(Event, 'BUBBLING_PHASE', 3, true);
  defineValue(Event.prototype, 'NONE', 0, true);
  defineValue(Event.prototype, 'CAPTURING_PHASE', 1, true);
  defineValue(Event.prototype, 'AT_TARGET', 2, true);
  defineValue(Event.prototype, 'BUBBLING_PHASE', 3, true);

  class CustomEvent extends Event {
    constructor(type, init = {}) {
      super(type, init);
      this.detail = init.detail;
    }
  }

  class MessageEvent extends Event {
    constructor(type, init = {}) {
      super(type, init);
      this.data = init.data === undefined ? null : init.data;
      this.origin = init.origin === undefined ? '' : String(init.origin);
      this.lastEventId = init.lastEventId === undefined ? '' : String(init.lastEventId);
      this.source = init.source || null;
      this.ports = init.ports || [];
    }
  }

  class UIEvent extends Event {
    constructor(type, init = {}) {
      super(type, init);
      this.view = init.view || global;
      this.detail = Number(init.detail || 0);
    }
  }

  class MouseEvent extends UIEvent {
    constructor(type, init = {}) {
      super(type, init);
      this.screenX = Number(init.screenX || 0);
      this.screenY = Number(init.screenY || 0);
      this.clientX = Number(init.clientX || 0);
      this.clientY = Number(init.clientY || 0);
      this.ctrlKey = Boolean(init.ctrlKey);
      this.shiftKey = Boolean(init.shiftKey);
      this.altKey = Boolean(init.altKey);
      this.metaKey = Boolean(init.metaKey);
      this.button = Number(init.button || 0);
      this.buttons = Number(init.buttons || 0);
      this.relatedTarget = init.relatedTarget || null;
    }
  }

  class KeyboardEvent extends UIEvent {
    constructor(type, init = {}) {
      super(type, init);
      this.key = init.key === undefined ? '' : String(init.key);
      this.code = init.code === undefined ? '' : String(init.code);
      this.location = Number(init.location || 0);
      this.ctrlKey = Boolean(init.ctrlKey);
      this.shiftKey = Boolean(init.shiftKey);
      this.altKey = Boolean(init.altKey);
      this.metaKey = Boolean(init.metaKey);
      this.repeat = Boolean(init.repeat);
      this.isComposing = Boolean(init.isComposing);
    }
  }

  class PointerEvent extends MouseEvent {
    constructor(type, init = {}) {
      super(type, init);
      this.pointerId = Number(init.pointerId || 1);
      this.width = Number(init.width || 1);
      this.height = Number(init.height || 1);
      this.pressure = init.pressure === undefined ? 0 : Number(init.pressure);
      this.tangentialPressure = Number(init.tangentialPressure || 0);
      this.tiltX = Number(init.tiltX || 0);
      this.tiltY = Number(init.tiltY || 0);
      this.twist = Number(init.twist || 0);
      this.pointerType = init.pointerType === undefined ? '' : String(init.pointerType);
      this.isPrimary = init.isPrimary === undefined ? true : Boolean(init.isPrimary);
    }
  }

  function listenerOptions(options) {
    if (typeof options === 'boolean') {
      return { capture: options, once: false, passive: false, signal: null };
    }
    if (!options || typeof options !== 'object') {
      return { capture: false, once: false, passive: false, signal: null };
    }
    return {
      capture: Boolean(options.capture),
      once: Boolean(options.once),
      passive: Boolean(options.passive),
      signal: options.signal || null
    };
  }

  function ensureListeners(target) {
    if (!Object.prototype.hasOwnProperty.call(target, '__listeners')) {
      Object.defineProperty(target, '__listeners', {
        value: new Map(),
        configurable: true
      });
    }
    return target.__listeners;
  }

  function eventPath(start) {
    const path = [];
    for (let node = start; node; node = node.parentNode || null) {
      path.push(node);
    }
    if (start !== global && path[path.length - 1] !== global) {
      path.push(global);
    }
    return path;
  }

  function connectCustomElementTree(node) {
    if (!customElementsRegistry) return;
    if (node instanceof Element) {
      customElementsRegistry._upgrade(node);
      invokeCustomElementConnected(wrapperCache.get(node.__id) || node);
    }
    if (node && node.querySelectorAll) {
      for (const child of node.querySelectorAll('*')) {
        customElementsRegistry._upgrade(child);
        invokeCustomElementConnected(wrapperCache.get(child.__id) || child);
      }
    }
  }

  function notifyCustomElementAttributeChanged(element, name, oldValue, newValue) {
    const ctor = element && element.__customElementConstructor;
    if (!ctor || typeof element.attributeChangedCallback !== 'function') return;
    const observed = ctor.observedAttributes || [];
    if (Array.prototype.indexOf.call(observed, name) < 0) return;
    element.attributeChangedCallback(name, oldValue, newValue);
  }

  function invokeCustomElementConnected(element) {
    if (!element || !element.__customElementConstructor || !element.isConnected) return;
    if (element.__customElementConnected) return;
    defineValue(element, '__customElementConnected', true);
    if (typeof element.connectedCallback === 'function') {
      try {
        element.connectedCallback();
      } catch (error) {
        console.error(error);
      }
    }
  }

  function reportEventListenerError(error) {
    try {
      if (global.console && typeof global.console.error === 'function') {
        global.console.error(error);
      } else if (host && typeof host.log === 'function') {
        host.log('error', error);
      }
    } catch (_reportError) {
    }
  }

  function callEventListener(target, entry, event) {
    try {
      if (typeof entry.callback === 'function') {
        entry.callback.call(target, event);
      } else {
        entry.callback.handleEvent.call(entry.callback, event);
      }
    } catch (error) {
      reportEventListenerError(error);
    }
  }

  function dispatchAtTarget(target, event) {
    const listeners = [...(ensureListeners(target).get(event.type) || [])];
    for (const entry of listeners) {
      callEventListener(target, entry, event);
      if (entry.once) target.removeEventListener(event.type, entry.callback, { capture: entry.capture });
      if (event._immediateStopped) break;
    }

    if (!event._immediateStopped) {
      const handler = target['on' + event.type];
      if (typeof handler === 'function') {
        try {
          handler.call(target, event);
        } catch (error) {
          reportEventListenerError(error);
        }
      }
    }
  }

  class EventTarget {
    constructor() {
      ensureListeners(this);
    }

    addEventListener(type, callback, options = undefined) {
      if (typeof callback !== 'function' && !(callback && typeof callback.handleEvent === 'function')) return;
      const key = String(type);
      const opts = listenerOptions(options);
      if (opts.signal && opts.signal.aborted) return;

      const listeners = ensureListeners(this);
      const list = listeners.get(key) || [];
      if (!list.some((entry) => entry.callback === callback && entry.capture === opts.capture)) {
        const entry = { callback, ...opts };
        list.push(entry);
        if (opts.signal && typeof opts.signal.addEventListener === 'function') {
          const remove = () => this.removeEventListener(key, callback, { capture: opts.capture });
          opts.signal.addEventListener('abort', remove, { once: true });
          entry.abortRemove = remove;
        }
      }
      listeners.set(key, list);
    }

    removeEventListener(type, callback, options = undefined) {
      const key = String(type);
      const opts = listenerOptions(options);
      const listeners = ensureListeners(this);
      const list = listeners.get(key) || [];
      listeners.set(key, list.filter((entry) => entry.callback !== callback || entry.capture !== opts.capture));
    }

    dispatchEvent(event) {
      if (!(event instanceof Event)) {
        throw new TypeError('dispatchEvent expects Event');
      }
      if (!event.target) event.target = this;
      event._path = event.bubbles ? eventPath(this) : [this];

      for (let i = 0; i < event._path.length; ++i) {
        event.currentTarget = event._path[i];
        event.eventPhase = i === 0 ? Event.AT_TARGET : Event.BUBBLING_PHASE;
        dispatchAtTarget(event.currentTarget, event);
        if (event.cancelBubble) break;
      }

      event.currentTarget = null;
      event.eventPhase = Event.NONE;
      return !event.defaultPrevented;
    }
  }

  class Window extends EventTarget {
    constructor() {
      super();
      throw new TypeError('Illegal constructor');
    }
  }

  defineValue(Window.prototype, Symbol.toStringTag, 'Window');

  function installWindowIdentity(target) {
    if (Object.getPrototypeOf(target) !== Window.prototype) {
      Object.setPrototypeOf(target, Window.prototype);
    }
    target.Window = Window;
    target.window = target;
    target.self = target;
    target.globalThis = target;
    target.frames = target;
    try {
      defineValue(target, Symbol.toStringTag, 'Window');
    } catch (_error) {
      // Some JS engines expose a non-configurable global toStringTag.
    }
    return target;
  }

  class MessagePort extends EventTarget {
    constructor() {
      super();
      this.onmessage = null;
      this.onmessageerror = null;
      this._entangledPort = null;
      this._closed = false;
    }

    postMessage(message) {
      if (this._closed || !this._entangledPort || this._entangledPort._closed) return;
      const target = this._entangledPort;
      global.setTimeout(() => {
        target.dispatchEvent(new MessageEvent('message', {
          data: message,
          origin: global.location.origin,
          source: null
        }));
      }, 0);
    }

    start() {}

    close() {
      this._closed = true;
      this._entangledPort = null;
    }
  }

  class MessageChannel {
    constructor() {
      this.port1 = new MessagePort();
      this.port2 = new MessagePort();
      this.port1._entangledPort = this.port2;
      this.port2._entangledPort = this.port1;
    }
  }

  class AbortSignal extends EventTarget {
    constructor() {
      super();
      this.aborted = false;
      this.reason = undefined;
      this.onabort = null;
    }

    throwIfAborted() {
      if (this.aborted) throw this.reason || new DOMException('The operation was aborted.', 'AbortError');
    }

    _abort(reason = undefined) {
      if (this.aborted) return;
      this.aborted = true;
      this.reason = reason === undefined ? new DOMException('The operation was aborted.', 'AbortError') : reason;
      this.dispatchEvent(new Event('abort'));
    }

    static abort(reason = undefined) {
      const signal = new AbortSignal();
      signal._abort(reason);
      return signal;
    }

    static timeout(milliseconds) {
      const signal = new AbortSignal();
      setTimeout(() => signal._abort(new DOMException('The operation timed out.', 'TimeoutError')), milliseconds);
      return signal;
    }
  }

  class AbortController {
    constructor() {
      this.signal = new AbortSignal();
    }

    abort(reason = undefined) {
      this.signal._abort(reason);
    }
  }

  const mutationObservers = new Set();
  let mutationFlushQueued = false;

  function queueMutation(record) {
    if (mutationObservers.size === 0) return;

    for (const observer of mutationObservers) {
      if (observer._matches(record)) observer._records.push(record);
    }

    if (mutationFlushQueued) return;
    mutationFlushQueued = true;
    Promise.resolve().then(() => {
      mutationFlushQueued = false;
      for (const observer of [...mutationObservers]) {
        const records = observer.takeRecords();
        if (records.length > 0) observer._callback(records, observer);
      }
    });
  }

  class MutationObserver {
    constructor(callback) {
      if (typeof callback !== 'function') throw new TypeError('MutationObserver callback must be a function');
      this._callback = callback;
      this._observations = [];
      this._records = [];
    }

    observe(target, options = {}) {
      if (!isNodeWrapper(target) && !isDocumentFragment(target)) {
        throw new TypeError('Expected a DOM Node');
      }
      const normalized = Object.assign({}, options);
      if ((normalized.attributeOldValue || normalized.attributeFilter) && normalized.attributes === undefined) {
        normalized.attributes = true;
      }
      if (normalized.characterDataOldValue && normalized.characterData === undefined) {
        normalized.characterData = true;
      }
      if (normalized.attributes === false && (normalized.attributeOldValue || normalized.attributeFilter)) {
        throw new TypeError('MutationObserver attribute options require attributes to be true');
      }
      if (normalized.characterData === false && normalized.characterDataOldValue) {
        throw new TypeError('MutationObserver characterDataOldValue requires characterData to be true');
      }
      if (!normalized.childList && !normalized.attributes && !normalized.characterData) {
        throw new TypeError('MutationObserver options must observe at least one mutation type');
      }
      if (normalized.attributeFilter) {
        normalized.attributeFilter = Array.from(normalized.attributeFilter, (name) => String(name));
      }
      this._observations.push({ target, options: normalized });
      mutationObservers.add(this);
    }

    disconnect() {
      this._observations = [];
      this._records = [];
      mutationObservers.delete(this);
    }

    takeRecords() {
      const records = this._records;
      this._records = [];
      return records;
    }

    _matches(record) {
      return this._observations.some(({ target, options }) => {
        if (record.type === 'childList' && !options.childList) return false;
        if (record.type === 'attributes' && !options.attributes) return false;
        if (record.type === 'characterData' && !options.characterData) return false;
        if (record.type === 'attributes' && options.attributeFilter && !options.attributeFilter.includes(record.attributeName)) return false;
        if (target === record.target) return true;
        return Boolean(options.subtree && target.contains && target.contains(record.target));
      });
    }
  }

  class Node extends EventTarget {
    constructor(id) {
      super();
      const nodeId = id === undefined ? pendingCustomElementNodeId : id;
      if (typeof nodeId === 'number') attachNodeId(this, nodeId);
    }

    _liveId() { return liveId(this); }

    get nodeType() { return bridge.nodeType(this._liveId()); }
    get nodeName() { return bridge.nodeName(this._liveId()); }
    get nodeValue() { return this.nodeType === 3 || this.nodeType === 8 ? this.textContent : null; }
    set nodeValue(value) { if (this.nodeType === 3 || this.nodeType === 8) this.textContent = value; }
    get parentNode() {
      if (this.__fragmentParent) return this.__fragmentParent;
      if (!bridge.hasNode(this.__id)) return null;
      return wrapNode(bridge.parentNode(this.__id));
    }
    get parentElement() {
      const parent = this.parentNode;
      return parent instanceof Element ? parent : null;
    }
    get ownerDocument() { return this instanceof Document ? null : document; }
    get childNodes() { return toArray(bridge.childNodes(this._liveId())); }
    get firstChild() { return this.childNodes[0] || null; }
    get lastChild() {
      const nodes = this.childNodes;
      return nodes[nodes.length - 1] || null;
    }
    get previousSibling() {
      const parent = this.parentNode;
      if (!parent) return null;
      const nodes = parent.childNodes;
      const index = nodes.findIndex((node) => node.__id === this.__id);
      return index > 0 ? nodes[index - 1] : null;
    }
    get nextSibling() {
      const parent = this.parentNode;
      if (!parent) return null;
      const nodes = parent.childNodes;
      const index = nodes.findIndex((node) => node.__id === this.__id);
      return index >= 0 && index + 1 < nodes.length ? nodes[index + 1] : null;
    }
    get isConnected() {
      if (this.__fragmentParent) {
        if (this.__fragmentParent instanceof ShadowRoot) return this.__fragmentParent.host.isConnected;
        return Boolean(this.__fragmentParent.isConnected);
      }
      return bridge.isConnected(this.__id);
    }
    get textContent() { return bridge.textContent(this._liveId()); }
    set textContent(value) {
      const text = String(value ?? '');
      const nodeType = this.nodeType;
      if (nodeType === 3 || nodeType === 8) {
        afterMutation(bridge.setTextContent(this._liveId(), text), {
          type: 'characterData',
          target: this,
          addedNodes: [],
          removedNodes: [],
          previousSibling: null,
          nextSibling: null,
          attributeName: null
        });
        return;
      }

      const removedNodes = this.childNodes;
      bridge.setTextContent(this._liveId(), text);
      syncMutationCache();
      if (suppressMutationRecords === 0) {
        queueMutation({
          type: 'childList',
          target: this,
          addedNodes: text === '' ? [] : this.childNodes,
          removedNodes,
          previousSibling: null,
          nextSibling: null,
          attributeName: null
        });
      }
    }

    appendChild(child) {
      if (isDocumentFragment(child)) {
        const addedNodes = [...child.childNodes];
        suppressMutationRecords++;
        try {
          for (const node of addedNodes) this.appendChild(node);
        } finally {
          suppressMutationRecords--;
        }
        child.childNodes.length = 0;
        queueMutation({
          type: 'childList',
          target: this,
          addedNodes,
          removedNodes: [],
          previousSibling: null,
          nextSibling: null,
          attributeName: null
        });
        return child;
      }
      if (child && child.__fragmentParent) child.__fragmentParent.removeChild(child);
      const id = bridge.appendChild(this._liveId(), liveId(child));
      const appended = wrapNode(id) || child;
      connectCustomElementTree(appended);
      return afterMutation(appended, {
        type: 'childList',
        target: this,
        addedNodes: [child],
        removedNodes: [],
        previousSibling: null,
        nextSibling: null,
        attributeName: null
      });
    }

    insertBefore(child, referenceChild) {
      if (isDocumentFragment(child)) {
        const addedNodes = [...child.childNodes];
        suppressMutationRecords++;
        try {
          for (const node of addedNodes) this.insertBefore(node, referenceChild);
        } finally {
          suppressMutationRecords--;
        }
        child.childNodes.length = 0;
        queueMutation({
          type: 'childList',
          target: this,
          addedNodes,
          removedNodes: [],
          previousSibling: null,
          nextSibling: referenceChild || null,
          attributeName: null
        });
        return child;
      }
      if (child && child.__fragmentParent) child.__fragmentParent.removeChild(child);
      const referenceId = referenceChild == null ? 0 : liveId(referenceChild);
      const id = bridge.insertBefore(this._liveId(), liveId(child), referenceId);
      const inserted = wrapNode(id) || child;
      connectCustomElementTree(inserted);
      return afterMutation(inserted, {
        type: 'childList',
        target: this,
        addedNodes: [child],
        removedNodes: [],
        previousSibling: null,
        nextSibling: referenceChild || null,
        attributeName: null
      });
    }

    removeChild(child) {
      const id = bridge.removeChild(this._liveId(), liveId(child));
      return afterMutation(wrapNode(id) || child, {
        type: 'childList',
        target: this,
        addedNodes: [],
        removedNodes: [child],
        previousSibling: null,
        nextSibling: null,
        attributeName: null
      });
    }

    replaceChildren(...nodes) {
      for (const child of [...this.childNodes]) this.removeChild(child);
      for (const node of nodes) this.appendChild(typeof node === 'string' ? document.createTextNode(node) : node);
    }

    replaceChild(child, replacedChild) {
      if (isDocumentFragment(child)) {
        this.insertBefore(child, replacedChild);
        this.removeChild(replacedChild);
        return replacedChild;
      }
      const id = bridge.replaceChild(this._liveId(), liveId(child), liveId(replacedChild));
      return afterMutation(wrapNode(id) || replacedChild, {
        type: 'childList',
        target: this,
        addedNodes: [child],
        removedNodes: [replacedChild],
        previousSibling: null,
        nextSibling: null,
        attributeName: null
      });
    }

    cloneNode(deep = false) {
      const clone = wrapNode(bridge.cloneNode(this._liveId(), Boolean(deep)));
      if (clone) copyCloneMetadata(this, clone);
      return clone;
    }

    remove() {
      const parent = this.parentNode;
      if (parent) parent.removeChild(this);
    }

    contains(candidate) {
      if (candidate == null || !isNodeWrapper(candidate)) return false;
      if (this === candidate) return true;
      for (let node = candidate.parentNode; node; node = node.parentNode) {
        if (node === this) return true;
      }
      if (!bridge.hasNode(this.__id) || !bridge.hasNode(candidate.__id)) return false;
      return bridge.contains(this.__id, candidate.__id);
    }

    getRootNode(options = {}) {
      let root = this;
      while (root.parentNode) root = root.parentNode;
      if (options && options.composed && root instanceof ShadowRoot) return root.host.getRootNode(options);
      return root instanceof ShadowRoot || root instanceof DocumentFragment ? root : document;
    }

    hasChildNodes() { return this.firstChild !== null; }
    isSameNode(candidate) {
      return isNodeWrapper(candidate) && candidate.__id === this.__id;
    }
    isEqualNode(candidate) { return this.isSameNode(candidate); }
    normalize() {}
  }

  class Text extends Node {
    get data() { return this.textContent; }
    set data(value) { this.textContent = value; }
    get length() { return this.data.length; }
  }

  class Comment extends Text {}

  function validateToken(token) {
    token = String(token);
    if (token.length === 0 || /\s/.test(token)) {
      throw new DOMException('Invalid token', 'SyntaxError');
    }
    return token;
  }

  class DOMTokenList {
    constructor(element, attribute) {
      this.element = element;
      this.attribute = attribute;
    }

    _tokens() {
      return (this.element.getAttribute(this.attribute) || '').split(/\s+/).filter(Boolean);
    }

    _write(tokens) {
      this.element.setAttribute(this.attribute, [...new Set(tokens)].join(' '));
    }

    contains(token) { return this._tokens().includes(validateToken(token)); }
    add(...tokens) { this._write([...this._tokens(), ...tokens.map(validateToken)]); }
    remove(...tokens) {
      const remove = new Set(tokens.map(validateToken));
      this._write(this._tokens().filter((token) => !remove.has(token)));
    }
    toggle(token, force = undefined) {
      token = validateToken(token);
      const has = this.contains(token);
      if (force === true || (!has && force !== false)) {
        this.add(token);
        return true;
      }
      if (has && force !== true) this.remove(token);
      return false;
    }
    replace(oldToken, newToken) {
      oldToken = validateToken(oldToken);
      newToken = validateToken(newToken);
      if (!this.contains(oldToken)) return false;
      this._write(this._tokens().map((token) => token === oldToken ? newToken : token));
      return true;
    }
    item(index) { return this._tokens()[Number(index)] || null; }
    get length() { return this._tokens().length; }
    get value() { return this._tokens().join(' '); }
    set value(value) { this._write(String(value).split(/\s+/).filter(Boolean).map(validateToken)); }
    forEach(callback, thisArg = undefined) {
      this._tokens().forEach((token, index) => callback.call(thisArg, token, index, this));
    }
    toString() { return this.value; }
    [Symbol.iterator]() { return this._tokens()[Symbol.iterator](); }
  }

  class DocumentFragment extends EventTarget {
    constructor() {
      super();
      this.nodeType = 11;
      this.nodeName = '#document-fragment';
      this.ownerDocument = document;
      this.childNodes = [];
      this._adoptedStyleSheets = [];
      this._cssomRootId = nextCssRootId++;
    }

    get firstChild() { return this.childNodes[0] || null; }
    get lastChild() { return this.childNodes[this.childNodes.length - 1] || null; }
    get children() { return this.childNodes.filter((node) => node instanceof Element); }
    get textContent() { return this.childNodes.map((node) => node.textContent || '').join(''); }
    set textContent(value) {
      this.childNodes.length = 0;
      if (value !== '') this.appendChild(document.createTextNode(String(value ?? '')));
    }

    appendChild(child) {
      assertNode(child);
      if (child.parentNode) child.parentNode.removeChild(child);
      const existing = this.childNodes.indexOf(child);
      if (existing >= 0) this.childNodes.splice(existing, 1);
      this.childNodes.push(child);
      defineValue(child, '__fragmentParent', this);
      return child;
    }

    insertBefore(child, referenceChild) {
      assertNode(child);
      if (child.parentNode) child.parentNode.removeChild(child);
      const existing = this.childNodes.indexOf(child);
      if (existing >= 0) this.childNodes.splice(existing, 1);
      const index = referenceChild == null ? -1 : this.childNodes.indexOf(referenceChild);
      if (index < 0) this.childNodes.push(child);
      else this.childNodes.splice(index, 0, child);
      defineValue(child, '__fragmentParent', this);
      return child;
    }

    removeChild(child) {
      const index = this.childNodes.indexOf(child);
      if (index < 0) throw new DOMException('Node was not found.', 'NotFoundError');
      this.childNodes.splice(index, 1);
      if (child.__fragmentParent === this) defineValue(child, '__fragmentParent', null);
      return child;
    }

    append(...nodes) {
      for (const node of nodes) this.appendChild(typeof node === 'string' ? document.createTextNode(node) : node);
    }

    prepend(...nodes) {
      for (const node of nodes.reverse()) {
        this.insertBefore(typeof node === 'string' ? document.createTextNode(node) : node, this.firstChild);
      }
    }

    replaceChildren(...nodes) {
      for (const child of [...this.childNodes]) this.removeChild(child);
      this.append(...nodes);
    }

    get adoptedStyleSheets() { return this._adoptedStyleSheets.slice(); }
    set adoptedStyleSheets(value) {
      if (!Array.isArray(value)) throw new TypeError('adoptedStyleSheets must be an array');
      for (const sheet of value) {
        if (!(sheet instanceof CSSStyleSheet)) throw new TypeError('adoptedStyleSheets entries must be CSSStyleSheet');
      }
      this._adoptedStyleSheets = value.slice();
      cssomVersion++;
    }

    contains(candidate) {
      if (candidate == null) return false;
      if (candidate === this) return true;
      for (let node = candidate.parentNode; node; node = node.parentNode) {
        if (node === this) return true;
      }
      return false;
    }

    getRootNode() { return this; }

    querySelector(selector) { return this.querySelectorAll(selector)[0] || null; }
    querySelectorAll(selector) {
      const out = [];
      for (const child of this.children) {
        if (child.matches(selector)) out.push(child);
        out.push(...child.querySelectorAll(selector));
      }
      return out;
    }

    cloneNode(deep = false) {
      const fragment = new DocumentFragment();
      if (deep) {
        for (const child of this.childNodes) fragment.appendChild(child.cloneNode(true));
      }
      return fragment;
    }
  }

  class ShadowRoot extends DocumentFragment {
    constructor(host, init = {}) {
      super();
      this.nodeName = '#shadow-root';
      this.host = host;
      this.mode = init && init.mode === 'closed' ? 'closed' : 'open';
      this.delegatesFocus = Boolean(init && init.delegatesFocus);
    }
  }

  class ElementInternals {
    constructor(target) {
      this._target = target;
    }

    get shadowRoot() { return this._target.__shadowRoot || null; }
    get form() { return null; }
    get labels() { return []; }
    get validity() { return {}; }
    get validationMessage() { return ''; }
    get willValidate() { return false; }
    checkValidity() { return true; }
    reportValidity() { return true; }
    setFormValue() {}
    setValidity() {}
  }

  function isDocumentFragment(value) {
    return value instanceof DocumentFragment;
  }

  function parseHTMLFragment(html) {
    const container = document.createElement('div');
    container.innerHTML = String(html ?? '');
    return [...container.childNodes];
  }

  function fragmentFromHTML(html) {
    const fragment = document.createDocumentFragment();
    for (const child of parseHTMLFragment(html)) fragment.appendChild(child);
    return fragment;
  }

  function insertNodesBefore(parent, referenceNode, nodes) {
    let lastInserted = null;
    for (const node of nodes) {
      parent.insertBefore(node, referenceNode || null);
      lastInserted = node;
    }
    return lastInserted;
  }

  class Attr {
    constructor(element, name) {
      this.ownerElement = element;
      this.name = String(name);
    }

    get localName() { return this.name; }
    get nodeName() { return this.name; }
    get value() { return this.ownerElement.getAttribute(this.name) ?? ''; }
    set value(value) { this.ownerElement.setAttribute(this.name, String(value)); }
    get nodeValue() { return this.value; }
    set nodeValue(value) { this.value = value; }
    get specified() { return true; }
  }

  class NamedNodeMap {
    constructor(element) {
      this.element = element;
    }

    _attrs() {
      return bridge.attributes(this.element._liveId()).map((attr) => new Attr(this.element, attr.name));
    }

    get length() { return this._attrs().length; }
    item(index) { return this._attrs()[Number(index)] || null; }
    getNamedItem(name) {
      const lookup = String(name);
      return this._attrs().find((attr) => attr.name === lookup) || null;
    }
    setNamedItem(attr) {
      if (!(attr instanceof Attr)) throw new TypeError('setNamedItem expects Attr');
      const old = this.getNamedItem(attr.name);
      this.element.setAttribute(attr.name, attr.value);
      return old;
    }
    removeNamedItem(name) {
      const old = this.getNamedItem(name);
      if (!old) throw new DOMException('Attribute not found', 'NotFoundError');
      this.element.removeAttribute(name);
      return old;
    }
    [Symbol.iterator]() { return this._attrs()[Symbol.iterator](); }
  }

  function namedNodeMap(element) {
    return new Proxy(new NamedNodeMap(element), {
      get(target, property, receiver) {
        if (typeof property === 'string' && /^(0|[1-9]\d*)$/.test(property)) {
          return target.item(Number(property));
        }
        if (property in target) {
          const value = Reflect.get(target, property, receiver);
          return typeof value === 'function' ? value.bind(target) : value;
        }
        if (typeof property === 'string') return target.getNamedItem(property);
        return undefined;
      }
    });
  }

  function kebab(name) {
    return String(name).replace(/[A-Z]/g, (c) => '-' + c.toLowerCase());
  }

  function cssPropertyName(name) {
    const text = String(name || '');
    if (text.startsWith('--')) return text;
    if (text === 'cssFloat' || text === 'styleFloat') return 'float';
    return kebab(text).toLowerCase();
  }

  function splitTopLevel(text, separator) {
    const parts = [];
    let current = '';
    let depth = 0;
    let quote = '';
    const source = String(text || '');
    for (let index = 0; index < source.length; index++) {
      const char = source[index];
      if (quote) {
        current += char;
        if (char === '\\') {
          if (index + 1 < source.length) current += source[++index];
          continue;
        }
        if (char === quote) quote = '';
        continue;
      }
      if (char === '"' || char === "'") {
        quote = char;
        current += char;
        continue;
      }
      if (char === '(' || char === '[') depth++;
      if (char === ')' || char === ']') depth = Math.max(0, depth - 1);
      if (char === separator && depth === 0) {
        parts.push(current);
        current = '';
      } else {
        current += char;
      }
    }
    parts.push(current);
    return parts;
  }

  function parseDeclarations(cssText) {
    const out = [];
    for (const part of splitTopLevel(cssText, ';')) {
      const index = part.indexOf(':');
      if (index === -1) continue;
      const name = cssPropertyName(part.slice(0, index).trim());
      if (!name) continue;
      let value = part.slice(index + 1).trim();
      let priority = '';
      const important = value.match(/\s*!important\s*$/i);
      if (important) {
        value = value.slice(0, important.index).trim();
        priority = 'important';
      }
      if (value !== '') out.push({ name, value, priority });
    }
    return out;
  }

  function declarationsToMap(declarations) {
    const out = Object.create(null);
    for (const declaration of declarations) {
      out[declaration.name] = declaration.value;
    }
    return out;
  }

  function parseStyle(cssText) {
    return declarationsToMap(parseDeclarations(cssText));
  }

  function serializeDeclarations(declarations) {
    return declarations
      .map((decl) => `${decl.name}: ${decl.value}${decl.priority ? ' !' + decl.priority : ''};`)
      .join(' ');
  }

  function makeCSSStyleDeclaration(readCssText, writeCssText = null, readOnly = false) {
    const declaration = new CSSStyleDeclaration(readCssText, writeCssText, readOnly);
    return new Proxy(declaration, {
      get(target, property, receiver) {
        if (typeof property === 'string' && /^(0|[1-9]\d*)$/.test(property)) {
          return target.item(Number(property));
        }
        if (property in target) {
          const value = Reflect.get(target, property, receiver);
          return typeof value === 'function' ? value.bind(target) : value;
        }
        if (typeof property === 'string') return target.getPropertyValue(cssPropertyName(property));
        return undefined;
      },
      set(target, property, value) {
        if (property === 'cssText') {
          target.cssText = String(value);
          return true;
        }
        if (typeof property === 'string') {
          target.setProperty(cssPropertyName(property), String(value));
          return true;
        }
        return false;
      },
      ownKeys(target) {
        return target._declarations().map((decl) => decl.name);
      },
      has(target, property) {
        if (property in target) return true;
        if (typeof property !== 'string') return false;
        return target._hasProperty(cssPropertyName(property));
      },
      getOwnPropertyDescriptor(target, property) {
        if (typeof property !== 'string') return undefined;
        const name = cssPropertyName(property);
        const value = target._propertyValue(name);
        if (value === '') return undefined;
        return {
          enumerable: true,
          configurable: true,
          value
        };
      }
    });
  }

  class CSSStyleDeclaration {
    constructor(readCssText = () => '', writeCssText = null, readOnly = false) {
      this._readCssText = readCssText;
      this._writeCssText = writeCssText;
      this._readOnly = readOnly;
      this._cachedCssText = null;
      this._cachedDeclarations = null;
      this._cachedValues = null;
      this._cachedPriorities = null;
    }

    _snapshot() {
      const text = String(this._readCssText() || '');
      if (text !== this._cachedCssText) {
        const declarations = parseDeclarations(text);
        const values = Object.create(null);
        const priorities = Object.create(null);
        for (const declaration of declarations) {
          values[declaration.name] = declaration.value;
          priorities[declaration.name] = declaration.priority;
        }
        this._cachedCssText = text;
        this._cachedDeclarations = declarations;
        this._cachedValues = values;
        this._cachedPriorities = priorities;
      }
      return {
        declarations: this._cachedDeclarations || [],
        values: this._cachedValues || Object.create(null),
        priorities: this._cachedPriorities || Object.create(null)
      };
    }

    _declarations() { return this._snapshot().declarations; }
    _propertyValue(name) {
      const property = cssPropertyName(name);
      if (!property) return '';
      return this._snapshot().values[property] || '';
    }
    _propertyPriority(name) {
      const property = cssPropertyName(name);
      if (!property) return '';
      return this._snapshot().priorities[property] || '';
    }
    _hasProperty(name) { return this._propertyValue(name) !== ''; }
    _invalidate() {
      this._cachedCssText = null;
      this._cachedDeclarations = null;
      this._cachedValues = null;
      this._cachedPriorities = null;
    }

    _write(declarations) {
      if (this._readOnly || typeof this._writeCssText !== 'function') return;
      this._writeCssText(serializeDeclarations(declarations));
      this._invalidate();
      cssomVersion++;
    }

    get cssText() { return String(this._readCssText() || ''); }
    set cssText(value) {
      if (this._readOnly) return;
      this._write(parseDeclarations(value));
    }

    get length() { return this._declarations().length; }
    item(index) {
      const declaration = this._declarations()[Number(index)];
      return declaration ? declaration.name : '';
    }
    getPropertyValue(name) {
      return this._propertyValue(name);
    }
    getPropertyPriority(name) {
      return this._propertyPriority(name);
    }
    setProperty(name, value, priority = '') {
      if (this._readOnly) return;
      const property = cssPropertyName(name);
      if (!property) return;
      const text = String(value ?? '').trim();
      const normalizedPriority = String(priority || '').trim().toLowerCase();
      if (text === '') {
        this.removeProperty(property);
        return;
      }
      const declarations = this._declarations().filter((decl) => decl.name !== property);
      declarations.push({
        name: property,
        value: text,
        priority: normalizedPriority === 'important' ? 'important' : ''
      });
      this._write(declarations);
    }
    removeProperty(name) {
      if (this._readOnly) return '';
      const property = cssPropertyName(name);
      const declarations = this._declarations();
      let old = '';
      const next = [];
      for (const declaration of declarations) {
        if (declaration.name === property) old = declaration.value;
        else next.push(declaration);
      }
      this._write(next);
      return old;
    }
  }

  function styleDeclaration(element) {
    return makeCSSStyleDeclaration(
      () => element.getAttribute('style') || '',
      (cssText) => {
        if (cssText) element.setAttribute('style', cssText);
        else element.removeAttribute('style');
      });
  }

  function dataAttrName(property) {
    if (typeof property !== 'string' || property === '') return null;
    return 'data-' + property.replace(/[A-Z]/g, (char) => '-' + char.toLowerCase());
  }

  function dataPropertyName(attribute) {
    return attribute.slice(5).replace(/-([a-z])/g, (_match, char) => char.toUpperCase());
  }

  function datasetFor(element) {
    return new Proxy({}, {
      get(_target, property) {
        const attr = dataAttrName(property);
        if (!attr) return undefined;
        const value = element.getAttribute(attr);
        return value === null ? undefined : value;
      },
      set(_target, property, value) {
        const attr = dataAttrName(property);
        if (!attr) return false;
        element.setAttribute(attr, String(value));
        return true;
      },
      deleteProperty(_target, property) {
        const attr = dataAttrName(property);
        if (!attr) return false;
        element.removeAttribute(attr);
        return true;
      },
      has(_target, property) {
        const attr = dataAttrName(property);
        return Boolean(attr && element.hasAttribute(attr));
      },
      ownKeys() {
        return bridge.attributes(element._liveId())
          .map((attr) => attr.name)
          .filter((name) => name.startsWith('data-'))
          .map(dataPropertyName);
      },
      getOwnPropertyDescriptor(_target, property) {
        const attr = dataAttrName(property);
        if (!attr || !element.hasAttribute(attr)) return undefined;
        return {
          enumerable: true,
          configurable: true,
          value: element.getAttribute(attr)
        };
      }
    });
  }

  function absoluteURL(value) {
    const text = String(value ?? '');
    if (!text) return '';
    const base = global.location && global.location.href ? global.location.href : (host.baseURL || undefined);
    const cacheKey = `${base || ''}\n${text}`;
    if (absoluteURLCache.has(cacheKey)) return absoluteURLCache.get(cacheKey);
    if (absoluteURLCache.size > 2048) absoluteURLCache.clear();
    try {
      const resolved = new URL(text, base).href;
      absoluteURLCache.set(cacheKey, resolved);
      return resolved;
    } catch (_error) {
      absoluteURLCache.set(cacheKey, text);
      return text;
    }
  }

  function stripCssComments(css) {
    const text = String(css || '');
    const chunks = [];
    let start = 0;
    let index = 0;
    while (index < text.length) {
      if (text[index] === '/' && text[index + 1] === '*') {
        if (index > start) chunks.push(text.slice(start, index));
        const end = text.indexOf('*/', index + 2);
        if (end === -1) return chunks.join('');
        index = end + 2;
        start = index;
      } else {
        index++;
      }
    }
    if (start < text.length) chunks.push(text.slice(start));
    return chunks.join('');
  }

  function findMatchingBrace(text, open) {
    let depth = 0;
    let quote = '';
    for (let index = open; index < text.length; index++) {
      const char = text[index];
      if (quote) {
        if (char === '\\') {
          index++;
          continue;
        }
        if (char === quote) quote = '';
        continue;
      }
      if (char === '"' || char === "'") {
        quote = char;
        continue;
      }
      if (char === '{') depth++;
      else if (char === '}') {
        depth--;
        if (depth === 0) return index;
      }
    }
    return -1;
  }

  function parseCSSRuleList(cssText, parentStyleSheet = null, startOrder = 0) {
    const rules = [];
    const text = stripCssComments(cssText);
    let offset = 0;
    let order = startOrder;
    while (offset < text.length) {
      const open = text.indexOf('{', offset);
      if (open === -1) break;
      const close = findMatchingBrace(text, open);
      if (close === -1) break;
      const prelude = text.slice(offset, open).trim();
      const body = text.slice(open + 1, close);
      offset = close + 1;
      if (!prelude) continue;

      if (prelude.startsWith('@media')) {
        rules.push(new CSSMediaRule(prelude.slice(6).trim(), body, parentStyleSheet, order++));
        continue;
      }
      if (prelude.startsWith('@')) continue;
      rules.push(new CSSStyleRule(prelude, body, parentStyleSheet, order++));
    }
    return rules;
  }

  function cssRuleList(rules) {
    const list = rules;
    if (typeof list.item !== 'function') {
      Object.defineProperty(list, 'item', {
        configurable: true,
        value(index) { return this[Number(index)] || null; }
      });
    }
    return list;
  }

  class CSSRule {
    constructor(type, parentStyleSheet = null, sourceOrder = 0) {
      this.type = type;
      this.parentStyleSheet = parentStyleSheet;
      this.parentRule = null;
      this._sourceOrder = sourceOrder;
    }
  }

  defineValue(CSSRule, 'STYLE_RULE', 1, true);
  defineValue(CSSRule, 'IMPORT_RULE', 3, true);
  defineValue(CSSRule, 'MEDIA_RULE', 4, true);
  defineValue(CSSRule.prototype, 'STYLE_RULE', 1, true);
  defineValue(CSSRule.prototype, 'IMPORT_RULE', 3, true);
  defineValue(CSSRule.prototype, 'MEDIA_RULE', 4, true);

  class CSSStyleRule extends CSSRule {
    constructor(selectorText = '', cssText = '', parentStyleSheet = null, sourceOrder = 0) {
      super(CSSRule.STYLE_RULE, parentStyleSheet, sourceOrder);
      this.selectorText = String(selectorText).trim();
      this._styleText = String(cssText || '').trim();
      this._compiledSelectors = null;
      this.style = makeCSSStyleDeclaration(
        () => this._styleText,
        (value) => {
          this._styleText = String(value || '');
          cssomVersion++;
          if (this.parentStyleSheet) this.parentStyleSheet._syncOwnerText();
        });
    }

    get cssText() { return `${this.selectorText} { ${this.style.cssText} }`; }
    set cssText(value) {
      const parsed = parseCSSRuleList(value, this.parentStyleSheet, this._sourceOrder)
        .find((rule) => rule.type === CSSRule.STYLE_RULE);
      if (!parsed) throw new DOMException('Invalid CSS rule', 'SyntaxError');
      this.selectorText = parsed.selectorText;
      this._styleText = parsed.style.cssText;
      this._compiledSelectors = null;
      cssomVersion++;
      if (this.parentStyleSheet) this.parentStyleSheet._syncOwnerText();
    }

    _selectors() {
      if (!this._compiledSelectors) {
        this._compiledSelectors = splitSelectorList(this.selectorText).map((selector) => {
          const tokens = selectorTokens(selector);
          return {
            selector,
            tokens,
            filter: selectorRightmostFilter(tokens),
            specificity: selectorSpecificity(selector)
          };
        });
      }
      return this._compiledSelectors;
    }
  }

  class CSSMediaRule extends CSSRule {
    constructor(conditionText = '', cssText = '', parentStyleSheet = null, sourceOrder = 0) {
      super(CSSRule.MEDIA_RULE, parentStyleSheet, sourceOrder);
      this.conditionText = String(conditionText || '').trim();
      this.media = {
        mediaText: this.conditionText,
        length: this.conditionText ? 1 : 0,
        item: (index) => Number(index) === 0 ? this.conditionText : null,
        toString: () => this.conditionText
      };
      this.cssRules = cssRuleList(parseCSSRuleList(cssText, parentStyleSheet, sourceOrder + 1));
      for (const rule of this.cssRules) rule.parentRule = this;
    }

    get cssText() {
      return `@media ${this.conditionText} { ${this.cssRules.map((rule) => rule.cssText).join(' ')} }`;
    }
    insertRule(ruleText, index = this.cssRules.length) {
      const parsed = parseCSSRuleList(ruleText, this.parentStyleSheet, this.cssRules.length)
        .filter((rule) => rule.type === CSSRule.STYLE_RULE || rule.type === CSSRule.MEDIA_RULE);
      if (parsed.length !== 1) throw new DOMException('Invalid CSS rule', 'SyntaxError');
      const position = Number(index);
      if (position < 0 || position > this.cssRules.length) throw new DOMException('Invalid rule index', 'IndexSizeError');
      parsed[0].parentRule = this;
      this.cssRules.splice(position, 0, parsed[0]);
      cssomVersion++;
      if (this.parentStyleSheet) this.parentStyleSheet._markDirty();
      if (this.parentStyleSheet) this.parentStyleSheet._syncOwnerText();
      return position;
    }
    deleteRule(index) {
      const position = Number(index);
      if (position < 0 || position >= this.cssRules.length) throw new DOMException('Invalid rule index', 'IndexSizeError');
      this.cssRules.splice(position, 1);
      cssomVersion++;
      if (this.parentStyleSheet) this.parentStyleSheet._markDirty();
      if (this.parentStyleSheet) this.parentStyleSheet._syncOwnerText();
    }
  }

  function stylesheetLinkHref(link) {
    return absoluteURL(link.getAttribute('href') || '');
  }

  function loadStylesheetTextForLink(link) {
    const href = stylesheetLinkHref(link);
    if (!href) return '';
    if (!stylesheetTextCache.has(href)) {
      try {
        const loaded = loadHostResource(href, 'stylesheet');
        stylesheetTextCache.set(href, loaded.body || '');
      } catch (_error) {
        stylesheetTextCache.set(href, '');
      }
    }
    return stylesheetTextCache.get(href) || '';
  }

  class CSSStyleSheet {
    constructor({ ownerNode = null, href = null, text = '', disabled = false } = {}) {
      this.ownerNode = ownerNode;
      this.href = href;
      this.ownerRule = null;
      this.media = { mediaText: '', length: 0, item: () => null, toString: () => '' };
      this.title = ownerNode ? ownerNode.getAttribute('title') || '' : '';
      this._disabled = Boolean(disabled);
      this._sourceText = String(text || '');
      this._rules = cssRuleList(parseCSSRuleList(this._sourceText, this, 0));
      this._version = 0;
    }

    get disabled() { return this._disabled; }
    set disabled(value) {
      const next = Boolean(value);
      if (this._disabled !== next) {
        this._disabled = next;
        this._version++;
        cssomVersion++;
      }
    }

    _currentOwnerText() {
      if (!this.ownerNode) return this._sourceText;
      if (this.ownerNode.localName === 'style') return this.ownerNode.textContent || '';
      if (this.ownerNode.localName === 'link') return loadStylesheetTextForLink(this.ownerNode);
      return this._sourceText;
    }

    _syncFromOwner() {
      const text = this._currentOwnerText();
      if (text !== this._sourceText) {
        this._sourceText = text;
        this._rules = cssRuleList(parseCSSRuleList(text, this, 0));
        this._version++;
        cssomVersion++;
      }
    }

    _syncOwnerText() {
      this._sourceText = this._rules.map((rule) => rule.cssText).join('\n');
      if (this.ownerNode && this.ownerNode.localName === 'style') {
        this.ownerNode.textContent = this._sourceText;
      }
    }

    _markDirty() {
      this._version++;
    }

    _cacheKey() {
      return `${this.href || ''}:${this._disabled ? 1 : 0}:${this._version}`;
    }

    get cssRules() {
      this._syncFromOwner();
      return this._rules;
    }
    get rules() { return this.cssRules; }

    insertRule(ruleText, index = this.cssRules.length) {
      const parsed = parseCSSRuleList(ruleText, this, this.cssRules.length)
        .filter((rule) => rule.type === CSSRule.STYLE_RULE || rule.type === CSSRule.MEDIA_RULE);
      if (parsed.length !== 1) throw new DOMException('Invalid CSS rule', 'SyntaxError');
      const position = Number(index);
      if (position < 0 || position > this.cssRules.length) throw new DOMException('Invalid rule index', 'IndexSizeError');
      this._rules.splice(position, 0, parsed[0]);
      this._version++;
      cssomVersion++;
      this._syncOwnerText();
      return position;
    }
    deleteRule(index) {
      const position = Number(index);
      if (position < 0 || position >= this.cssRules.length) throw new DOMException('Invalid rule index', 'IndexSizeError');
      this._rules.splice(position, 1);
      this._version++;
      cssomVersion++;
      this._syncOwnerText();
    }
    addRule(selector, style = '', index = this.cssRules.length) {
      return this.insertRule(`${selector} { ${style} }`, index);
    }
    removeRule(index = 0) { this.deleteRule(index); }
    replaceSync(text) {
      this._sourceText = String(text || '');
      this._rules = cssRuleList(parseCSSRuleList(this._sourceText, this, 0));
      this._version++;
      cssomVersion++;
      this._syncOwnerText();
    }
    replace(text) {
      this.replaceSync(text);
      return Promise.resolve(this);
    }
  }

  function styleSheetForNode(node) {
    if (!node) return null;
    const href = node.localName === 'link' ? stylesheetLinkHref(node) : '';
    const key = node.localName === 'link'
      ? `link:${node.__id}:${href}`
      : `style:${node.__id}`;
    const text = node.localName === 'link' ? loadStylesheetTextForLink(node) : (node.textContent || '');
    let sheet = stylesheetCache.get(key);
    if (!sheet) {
      sheet = new CSSStyleSheet({
        ownerNode: node,
        href: node.localName === 'link' ? href : null,
        text,
        disabled: node.disabled || node.hasAttribute('disabled')
      });
      stylesheetCache.set(key, sheet);
    } else {
      sheet.disabled = sheet.disabled || Boolean(node.disabled || node.hasAttribute('disabled'));
      sheet._syncFromOwner();
    }
    return sheet;
  }

  function relListContainsToken(value, token) {
    const text = String(value || '').toLowerCase();
    const wanted = String(token || '').toLowerCase();
    let index = 0;
    while (index < text.length) {
      while (index < text.length && /\s/.test(text[index])) index++;
      const start = index;
      while (index < text.length && !/\s/.test(text[index])) index++;
      if (text.slice(start, index) === wanted) return true;
    }
    return false;
  }

  function styleSheetsForDocument() {
    const mutationVersion = bridge.mutationVersion();
    if (mutationVersion === stylesheetListMutationVersion && stylesheetListCache) return stylesheetListCache;

    const nodes = [];
    for (const node of document.querySelectorAll('style,link')) {
      if (node.localName === 'style') {
        nodes.push(node);
      } else if (node.localName === 'link' && relListContainsToken(node.getAttribute('rel') || '', 'stylesheet')) {
        nodes.push(node);
      }
    }
    const key = nodes.map((node) => `${node.localName}:${node.__id}:${node.localName === 'style' ? '' : stylesheetLinkHref(node)}:${node.disabled ? 1 : 0}`).join('|');
    if (key === stylesheetListKey && stylesheetListCache) {
      stylesheetListMutationVersion = mutationVersion;
      return stylesheetListCache;
    }
    const list = nodes.map(styleSheetForNode).filter(Boolean);
    Object.defineProperty(list, 'item', {
      configurable: true,
      value(index) { return this[Number(index)] || null; }
    });
    stylesheetListKey = key;
    stylesheetListCache = list;
    stylesheetListMutationVersion = mutationVersion;
    return list;
  }

  function rootAdoptedStyleSheets(root) {
    if (root === document) return adoptedStyleSheets;
    if (root && Array.isArray(root._adoptedStyleSheets)) return root._adoptedStyleSheets;
    return [];
  }

  function cssRootKey(root) {
    if (root === document) return 'document';
    if (!root) return 'none';
    if (!root._cssomRootId) defineValue(root, '_cssomRootId', nextCssRootId++);
    return `root:${root._cssomRootId}`;
  }

  function cascadeStyleSheets(root = document) {
    const rootSheets = root === document ? styleSheetsForDocument() : [];
    return [...rootSheets, ...rootAdoptedStyleSheets(root)];
  }

  function defaultDisplayFor(element) {
    const name = element && element.localName;
    if (name === 'script' || name === 'style' || name === 'template' || name === 'head' || name === 'meta' || name === 'link' || name === 'title') {
      return 'none';
    }
    if (name === 'span' || name === 'a' || name === 'b' || name === 'i' || name === 'strong' || name === 'em' || name === 'small' || name === 'label') {
      return 'inline';
    }
    if (name === 'img' || name === 'input' || name === 'button' || name === 'select' || name === 'textarea' || name === 'canvas' || name === 'iframe') {
      return 'inline-block';
    }
    return 'block';
  }

  function computedDisplay(element) {
    return computedPropertyValue(element, 'display');
  }

  const inheritedProperties = new Set([
    'color',
    'font',
    'font-family',
    'font-size',
    'font-style',
    'font-weight',
    'line-height',
    'text-align',
    'visibility',
    'white-space'
  ]);

  const commonComputedProperties = [
    'background-color',
    'color',
    'display',
    'float',
    'font',
    'font-family',
    'font-size',
    'font-style',
    'font-weight',
    'height',
    'line-height',
    'margin',
    'margin-bottom',
    'margin-left',
    'margin-right',
    'margin-top',
    'opacity',
    'padding',
    'padding-bottom',
    'padding-left',
    'padding-right',
    'padding-top',
    'position',
    'text-align',
    'visibility',
    'white-space',
    'width'
  ];

  function defaultComputedPropertyValue(element, property) {
    if (property === 'display') return defaultDisplayFor(element);
    if (property === 'float') return 'none';
    if (property === 'opacity') return '1';
    if (property === 'position') return 'static';
    if (property === 'visibility') return 'visible';
    return '';
  }

  function mediaRuleApplies(rule) {
    const text = String(rule.conditionText || '').trim().toLowerCase();
    return !text || text === 'all' || text === 'screen' || text === 'only screen' || text.includes('min-width');
  }

  function collectStyleRules(rules, out = []) {
    for (const rule of rules) {
      if (rule.type === CSSRule.STYLE_RULE) out.push(rule);
      else if (rule.type === CSSRule.MEDIA_RULE && mediaRuleApplies(rule)) collectStyleRules(rule.cssRules, out);
    }
    return out;
  }

  function cascadeEntries(root = document) {
    const sheets = cascadeStyleSheets(root);
    const key = `${cssRootKey(root)}:${bridge.mutationVersion()}:${cssomVersion}:` + sheets.map((sheet) => {
      if (!sheet) return '';
      return sheet._cacheKey();
    }).join('|');
    if (key === cascadeEntriesKey) return cascadeEntriesCache;

    const entries = [];
    let sourceOrder = 0;
    for (const sheet of sheets) {
      if (!sheet || sheet.disabled) continue;
      for (const rule of collectStyleRules(sheet.cssRules)) {
        const declarations = parseDeclarations(rule.style.cssText);
        if (declarations.length === 0) continue;
        for (const selector of rule._selectors()) {
          entries.push({
            selector: selector.selector,
            tokens: selector.tokens,
            filter: selector.filter,
            specificity: selector.specificity,
            declarations,
            sourceOrder: sourceOrder++
          });
        }
      }
    }

    cascadeEntriesKey = key;
    cascadeEntriesCache = entries;
    return entries;
  }

  function selectorSpecificity(selector) {
    let text = String(selector || '');
    text = text.replace(/:where\([^)]*\)/g, '');
    const ids = (text.match(/#[A-Za-z0-9_-]+/g) || []).length;
    const classes = (text.match(/\.[A-Za-z0-9_-]+/g) || []).length;
    const attributes = (text.match(/\[[^\]]+\]/g) || []).length;
    const pseudos = (text.match(/:(?!:)[A-Za-z0-9_-]+(?:\([^)]*\))?/g) || [])
      .filter((pseudo) => !pseudo.startsWith(':where')).length;
    text = text
      .replace(/#[A-Za-z0-9_-]+/g, ' ')
      .replace(/\.[A-Za-z0-9_-]+/g, ' ')
      .replace(/\[[^\]]+\]/g, ' ')
      .replace(/::?[A-Za-z0-9_-]+(?:\([^)]*\))?/g, ' ')
      .replace(/[>+~*,]/g, ' ');
    const types = (text.match(/\b[A-Za-z][A-Za-z0-9_-]*\b/g) || []).length;
    return [ids, classes + attributes + pseudos, types];
  }

  function compareSpecificity(left, right) {
    for (let index = 0; index < 3; index++) {
      if (left[index] !== right[index]) return left[index] - right[index];
    }
    return 0;
  }

  function selectorTokens(selector) {
    const parts = [];
    let current = '';
    let depth = 0;
    let quote = '';
    let combinator = ' ';
    const text = String(selector || '').trim();

    for (let index = 0; index < text.length; index++) {
      const char = text[index];
      if (quote) {
        current += char;
        if (char === '\\') {
          if (index + 1 < text.length) current += text[++index];
          continue;
        }
        if (char === quote) quote = '';
        continue;
      }
      if (char === '"' || char === "'") {
        quote = char;
        current += char;
        continue;
      }
      if (char === '(' || char === '[') {
        depth++;
        current += char;
        continue;
      }
      if (char === ')' || char === ']') {
        depth = Math.max(0, depth - 1);
        current += char;
        continue;
      }
      if (depth === 0 && (char === '>' || /\s/.test(char))) {
        if (current.trim()) {
          parts.push({ selector: current.trim(), combinator });
          current = '';
        }
        if (char === '>') combinator = '>';
        else if (combinator !== '>') combinator = ' ';
        while (index + 1 < text.length && /\s/.test(text[index + 1])) index++;
        continue;
      }
      current += char;
    }
    if (current.trim()) parts.push({ selector: current.trim(), combinator });
    return parts;
  }

  function unquoteCssValue(value) {
    const text = String(value || '').trim();
    if ((text.startsWith('"') && text.endsWith('"')) || (text.startsWith("'") && text.endsWith("'"))) {
      return text.slice(1, -1);
    }
    return text;
  }

  function selectorRightmostFilter(tokens) {
    const rightmost = tokens.length ? tokens[tokens.length - 1].selector : '';
    const tag = (rightmost.replace(/\[[^\]]+\]/g, ' ').match(/^[A-Za-z][A-Za-z0-9_-]*/) || [])[0] || '';
    const id = (rightmost.match(/#[A-Za-z0-9_-]+/) || [''])[0].slice(1);
    const classes = (rightmost.match(/(?:\.|\.\\:)[A-Za-z0-9_:-]+/g) || []).map((classSelector) =>
      classSelector.startsWith('.\\:') ? ':' + classSelector.slice(3) : classSelector.slice(1));
    return {
      tag: tag.toLowerCase(),
      id,
      classes
    };
  }

  function selectorFilterMayMatch(element, filter) {
    if (!filter) return true;
    if (filter.tag && filter.tag !== element.localName) return false;
    if (filter.id && filter.id !== element.id) return false;
    for (const className of filter.classes || []) {
      if (!element.classList.contains(className)) return false;
    }
    return true;
  }

  function matchesCompoundSelector(element, selector) {
    if (!(element instanceof Element)) return false;
    let text = String(selector || '').trim();
    if (!text || text === '*') return true;
    if (text.includes('::')) return false;

    text = text.replace(/:where\(([^()]*)\)/g, (_match, inner) => inner.trim());
    text = text.replace(/:not\(([^()]*)\)/g, (_match, inner) => {
      if (matchesCompoundSelector(element, inner.trim())) return '\u0000';
      return '';
    });
    if (text.includes('\u0000')) return false;
    text = text.replace(/:target\b/g, () => {
      const hash = (global.location && global.location.hash) || '';
      const targetId = hash.startsWith('#') ? decodeURIComponent(hash.slice(1)) : '';
      return targetId && element.id === targetId ? '' : '\u0000';
    });
    if (text.includes('\u0000')) return false;
    if (/:(hover|active|focus|visited|link|checked|disabled|enabled|first-child|last-child|nth-child|nth-of-type|first-of-type|last-of-type)\b/.test(text)) {
      return false;
    }

    const attrPattern = /\[([^\]=~|^$*!\s]+)(?:\s*([~|^$*]?=)\s*([^\]]+))?\]/g;
    text = text.replace(attrPattern, (_match, rawName, operator, rawValue) => {
      const name = String(rawName);
      if (!element.hasAttribute(name)) return '\u0000';
      if (!operator) return '';
      const actual = element.getAttribute(name) || '';
      const expected = unquoteCssValue(rawValue);
      let ok = false;
      if (operator === '=') ok = actual === expected;
      else if (operator === '~=') ok = actual.split(/\s+/).includes(expected);
      else if (operator === '|=') ok = actual === expected || actual.startsWith(expected + '-');
      else if (operator === '^=') ok = actual.startsWith(expected);
      else if (operator === '$=') ok = actual.endsWith(expected);
      else if (operator === '*=') ok = actual.includes(expected);
      return ok ? '' : '\u0000';
    });
    if (text.includes('\u0000')) return false;

    const idMatches = text.match(/#[A-Za-z0-9_-]+/g) || [];
    for (const id of idMatches) {
      if (element.id !== id.slice(1)) return false;
    }
    text = text.replace(/#[A-Za-z0-9_-]+/g, '');

    const classMatches = text.match(/(?:\.|\.\\:)[A-Za-z0-9_:-]+/g) || [];
    for (const classSelector of classMatches) {
      const className = classSelector.startsWith('.\\:')
        ? ':' + classSelector.slice(3)
        : classSelector.slice(1);
      if (!element.classList.contains(className)) return false;
    }
    text = text.replace(/(?:\.|\.\\:)[A-Za-z0-9_:-]+/g, '');

    const tag = text.trim();
    if (tag && tag !== '*' && tag.toLowerCase() !== element.localName) return false;
    return true;
  }

  function selectorMatchesElementFast(element, selectorOrTokens) {
    const tokens = Array.isArray(selectorOrTokens) ? selectorOrTokens : selectorTokens(selectorOrTokens);
    if (tokens.length === 0) return false;

    function matchFrom(index, candidate) {
      if (!(candidate instanceof Element)) return false;
      if (!matchesCompoundSelector(candidate, tokens[index].selector)) return false;
      if (index === 0) return true;

      const relation = tokens[index].combinator;
      if (relation === '>') {
        return matchFrom(index - 1, candidate.parentElement);
      }

      for (let ancestor = candidate.parentElement; ancestor; ancestor = ancestor.parentElement) {
        if (matchFrom(index - 1, ancestor)) return true;
      }
      return false;
    }

    return matchFrom(tokens.length - 1, element);
  }

  function applyCascadeValue(values, name, value, priority, specificity, sourceOrder) {
    if (value === '') return;
    const important = priority === 'important';
    const existing = values.get(name);
    let wins = !existing;
    if (existing) {
      if (important !== existing.important) wins = important;
      else {
        const specificityCompare = compareSpecificity(specificity, existing.specificity);
        wins = specificityCompare > 0 || (specificityCompare === 0 && sourceOrder >= existing.sourceOrder);
      }
    }
    if (wins) values.set(name, { value, important, specificity, sourceOrder });
  }

  function computeCascade(element) {
    if (!(element instanceof Element)) return Object.create(null);
    const root = element.getRootNode ? element.getRootNode() : document;
    const entries = cascadeEntries(root);
    const cacheKey = `${element.__id}:${cssRootKey(root)}:${bridge.mutationVersion()}:${cssomVersion}:${cascadeEntriesKey}`;
    const cached = computedStyleCache.get(cacheKey);
    if (cached) return cached;

    const values = new Map();

    for (const entry of entries) {
      try {
        if (!selectorFilterMayMatch(element, entry.filter)) continue;
        if (!selectorMatchesElementFast(element, entry.tokens)) continue;
      } catch (_error) {
        continue;
      }
      for (const declaration of entry.declarations) {
        applyCascadeValue(values, declaration.name, declaration.value, declaration.priority, entry.specificity, entry.sourceOrder);
      }
    }

    let sourceOrder = entries.length;
    for (const declaration of parseDeclarations(element.getAttribute('style') || '')) {
      applyCascadeValue(values, declaration.name, declaration.value, declaration.priority, [1, 0, 0], sourceOrder++);
    }

    if (element.hasAttribute('hidden')) {
      applyCascadeValue(values, 'display', 'none', 'important', [1, 0, 0], sourceOrder++);
    }
    const out = Object.create(null);
    for (const [name, record] of values) out[name] = record.value;
    computedStyleCache.set(cacheKey, out);
    if (computedStyleCache.size > 4096) computedStyleCache.clear();
    return out;
  }

  function inheritedPropertyValue(element, property, depth) {
    if (depth > 64) return defaultComputedPropertyValue(element, property);
    const parent = element && element.parentElement;
    return parent ? computedPropertyValue(parent, property, depth + 1) : defaultComputedPropertyValue(element, property);
  }

  function resolveCSSWideValue(element, property, value, depth) {
    const keyword = String(value || '').trim().toLowerCase();
    if (keyword === 'inherit') return inheritedPropertyValue(element, property, depth);
    if (keyword === 'initial') return defaultComputedPropertyValue(element, property);
    if (keyword === 'unset') {
      return inheritedProperties.has(property)
        ? inheritedPropertyValue(element, property, depth)
        : defaultComputedPropertyValue(element, property);
    }
    return value;
  }

  function computedPropertyValue(element, name, depth = 0) {
    if (!(element instanceof Element)) return '';
    const property = cssPropertyName(name);
    if (!property) return '';
    const values = computeCascade(element);
    if (Object.prototype.hasOwnProperty.call(values, property)) {
      return resolveCSSWideValue(element, property, values[property], depth);
    }
    if (inheritedProperties.has(property)) {
      return inheritedPropertyValue(element, property, depth);
    }
    return defaultComputedPropertyValue(element, property);
  }

  function computedPropertyNames(element) {
    const names = new Set(Object.keys(computeCascade(element)));
    for (const property of commonComputedProperties) names.add(property);
    for (const property of inheritedProperties) names.add(property);
    return Array.from(names);
  }

  function computedStyleFor(element) {
    const propertyNames = () => computedPropertyNames(element);
    const propertyValue = (name) => computedPropertyValue(element, name);
    const declaration = Object.create(CSSStyleDeclaration.prototype);
    defineValue(declaration, 'item', (index) => propertyNames()[Number(index)] || '');
    defineValue(declaration, 'getPropertyValue', (name) => propertyValue(name));
    defineValue(declaration, 'getPropertyPriority', () => '');
    defineValue(declaration, 'setProperty', () => {});
    defineValue(declaration, 'removeProperty', () => '');
    Object.defineProperty(declaration, 'length', {
      configurable: true,
      enumerable: false,
      get() { return propertyNames().length; }
    });
    Object.defineProperty(declaration, 'cssText', {
      configurable: true,
      enumerable: false,
      get() {
        return serializeDeclarations(propertyNames().map((name) => ({ name, value: propertyValue(name), priority: '' })));
      },
      set() {}
    });

    return new Proxy(declaration, {
      get(target, property, receiver) {
        if (typeof property === 'string' && /^(0|[1-9]\d*)$/.test(property)) {
          return propertyNames()[Number(property)] || '';
        }
        if (property === 'length') {
          return propertyNames().length;
        }
        if (property === 'item') {
          return (index) => propertyNames()[Number(index)] || '';
        }
        if (property === 'getPropertyValue') {
          return (name) => propertyValue(name);
        }
        if (property === 'getPropertyPriority') {
          return () => '';
        }
        if (property === 'cssText') {
          return Reflect.get(target, property, receiver);
        }
        if (property in target) {
          const value = Reflect.get(target, property, receiver);
          return typeof value === 'function' ? value.bind(target) : value;
        }
        if (typeof property === 'string') return propertyValue(cssPropertyName(property));
        return undefined;
      },
      ownKeys() {
        return propertyNames();
      },
      getOwnPropertyDescriptor(_target, property) {
        if (typeof property !== 'string') return undefined;
        const value = propertyValue(property);
        if (value === '') return undefined;
        return {
          enumerable: true,
          configurable: true,
          value
        };
      }
    });
  }

  function getCookieString() {
    return Array.from(cookieJar.entries()).map(([name, value]) => `${name}=${value}`).join('; ');
  }

  function setCookieString(cookie) {
    const parts = String(cookie ?? '').split(';').map((part) => part.trim());
    const pair = parts.shift() || '';
    const equals = pair.indexOf('=');
    if (equals <= 0) return;

    const name = pair.slice(0, equals);
    const value = pair.slice(equals + 1);
    const expires = parts.find((part) => part.toLowerCase().startsWith('expires='));
    if (expires) {
      const when = Date.parse(expires.slice(8));
      if (Number.isFinite(when) && when <= Date.now()) {
        cookieJar.delete(name);
        return;
      }
    }
    cookieJar.set(name, value);
  }

  function splitSelectorList(selector) {
    const parts = [];
    let current = '';
    let depth = 0;
    let quote = '';
    for (const char of String(selector)) {
      if (quote) {
        current += char;
        if (char === quote) quote = '';
        continue;
      }
      if (char === '"' || char === "'") {
        quote = char;
        current += char;
        continue;
      }
      if (char === '(' || char === '[') depth++;
      if (char === ')' || char === ']') depth = Math.max(0, depth - 1);
      if (char === ',' && depth === 0) {
        parts.push(current.trim());
        current = '';
      } else {
        current += char;
      }
    }
    if (current.trim()) parts.push(current.trim());
    return parts;
  }

  function nativeElementMatches(element, selector) {
    const text = String(selector || '').trim();
    if (!text || text === '*') return true;
    const parent = element.parentNode || document;
    if (!parent || typeof parent._liveId !== 'function') return false;
    try {
      return bridge.querySelectorAll(parent._liveId(), text).some((id) => id === element.__id);
    } catch (_error) {
      return false;
    }
  }

  function selectorPartMatchesEscapedClasses(element, selector) {
    const required = [];
    const forbidden = [];
    let requiresTarget = false;
    let normalized = String(selector);
    normalized = normalized.replace(/:not\(\s*\.\\:([A-Za-z0-9_-]+)\s*\)/g, (_match, name) => {
      forbidden.push(`:${name}`);
      return '';
    });
    normalized = normalized.replace(/\.\\:([A-Za-z0-9_-]+)/g, (_match, name) => {
      required.push(`:${name}`);
      return '';
    });
    normalized = normalized.replace(/:target\b/g, () => {
      requiresTarget = true;
      return '';
    });
    normalized = normalized.replace(/:where\(\s*\)/g, '').replace(/\s+/g, ' ').trim();

    if (requiresTarget) {
      const hash = (global.location && global.location.hash) || '';
      const targetId = hash.startsWith('#') ? decodeURIComponent(hash.slice(1)) : '';
      if (!targetId || element.id !== targetId) return false;
    }
    for (const className of required) {
      if (!element.classList.contains(className)) return false;
    }
    for (const className of forbidden) {
      if (element.classList.contains(className)) return false;
    }
    return nativeElementMatches(element, normalized);
  }

  function selectorMatchesEscapedClasses(element, selector) {
    return splitSelectorList(selector).some((part) => selectorPartMatchesEscapedClasses(element, part));
  }

  function querySelectorAllEscapedClassFallback(root, selector) {
    let candidates = [];
    try {
      candidates = toArray(bridge.querySelectorAll(root._liveId(), '*'));
    } catch (_error) {
      return [];
    }
    const out = [];
    const seen = new Set();
    for (const candidate of candidates) {
      if (candidate instanceof Element && selectorMatchesEscapedClasses(candidate, selector) && !seen.has(candidate.__id)) {
        seen.add(candidate.__id);
        out.push(candidate);
      }
    }
    return out;
  }

  function querySelectorAllCompat(root, selector) {
    const text = String(selector);
    try {
      return toArray(bridge.querySelectorAll(root._liveId(), text));
    } catch (error) {
      if (!text.includes('.\\:') && !text.includes(':target')) throw new DOMException(`Invalid selector: ${text}`, 'SyntaxError');
      const fallback = querySelectorAllEscapedClassFallback(root, text);
      if (fallback.length || /(^|[^\\])\.\\:/.test(text) || text.includes(':target')) return fallback;
      throw new DOMException(`Invalid selector: ${text}`, 'SyntaxError');
    }
  }

  function querySelectorCompat(root, selector) {
    const text = String(selector);
    try {
      return wrapNode(bridge.querySelector(root._liveId(), text));
    } catch (error) {
      if (!text.includes('.\\:') && !text.includes(':target')) throw new DOMException(`Invalid selector: ${text}`, 'SyntaxError');
      const fallback = querySelectorAllEscapedClassFallback(root, text);
      if (fallback.length || /(^|[^\\])\.\\:/.test(text) || text.includes(':target')) return fallback[0] || null;
      throw new DOMException(`Invalid selector: ${text}`, 'SyntaxError');
    }
  }

  class Element extends Node {
    get tagName() { return bridge.tagName(this._liveId()); }
    get localName() { return this.tagName.toLowerCase(); }
    get namespaceURI() {
      if (this.__namespaceURI) return this.__namespaceURI;
      if (typeof SVGElement !== 'undefined' && this instanceof SVGElement) return 'http://www.w3.org/2000/svg';
      return 'http://www.w3.org/1999/xhtml';
    }
    get id() { return this.getAttribute('id') || ''; }
    set id(value) { this.setAttribute('id', String(value)); }
    get className() { return this.getAttribute('class') || ''; }
    set className(value) { this.setAttribute('class', String(value)); }
    get src() { return absoluteURL(this.getAttribute('src') || ''); }
    set src(value) { this.setAttribute('src', String(value)); }
    get href() { return absoluteURL(this.getAttribute('href') || ''); }
    set href(value) { this.setAttribute('href', String(value)); }
    get action() { return absoluteURL(this.getAttribute('action') || ''); }
    set action(value) { this.setAttribute('action', String(value)); }
    get rel() { return this.getAttribute('rel') || ''; }
    set rel(value) { this.setAttribute('rel', String(value)); }
    get type() { return this.getAttribute('type') || ''; }
    set type(value) { this.setAttribute('type', String(value)); }
    get crossOrigin() { return this.getAttribute('crossorigin') || ''; }
    set crossOrigin(value) {
      if (value == null) this.removeAttribute('crossorigin');
      else this.setAttribute('crossorigin', String(value));
    }
    get nonce() { return this.getAttribute('nonce') || ''; }
    set nonce(value) { this.setAttribute('nonce', String(value)); }
    get async() { return this.hasAttribute('async'); }
    set async(value) { this.toggleAttribute('async', Boolean(value)); }
    get defer() { return this.hasAttribute('defer'); }
    set defer(value) { this.toggleAttribute('defer', Boolean(value)); }
    get hidden() { return this.hasAttribute('hidden'); }
    set hidden(value) { this.toggleAttribute('hidden', Boolean(value)); }
    get classList() { return memo(this, '__classList', () => new DOMTokenList(this, 'class')); }
    get attributes() { return memo(this, '__attributes', () => namedNodeMap(this)); }
    get dataset() { return memo(this, '__dataset', () => datasetFor(this)); }
    get children() { return toArray(bridge.children(this._liveId())); }
    get firstElementChild() { return this.children[0] || null; }
    get lastElementChild() {
      const children = this.children;
      return children[children.length - 1] || null;
    }
    get childElementCount() { return this.children.length; }
    get innerHTML() { return bridge.innerHTML(this._liveId()); }
    set innerHTML(value) {
      afterMutation(bridge.setInnerHTML(this._liveId(), String(value ?? '')), {
        type: 'childList',
        target: this,
        addedNodes: [],
        removedNodes: [],
        previousSibling: null,
        nextSibling: null,
        attributeName: null
      });
    }
    get outerHTML() { return bridge.outerHTML(this._liveId()); }
    get style() { return memo(this, '__style', () => styleDeclaration(this)); }
    get shadowRoot() {
      const root = this.__shadowRoot || null;
      return root && root.mode === 'open' ? root : null;
    }
    attachShadow(init = {}) {
      if (this.__shadowRoot) throw new DOMException('Shadow root already attached.', 'NotSupportedError');
      const root = new ShadowRoot(this, init);
      defineValue(this, '__shadowRoot', root);
      return root;
    }
    attachInternals() {
      return memo(this, '__elementInternals', () => new ElementInternals(this));
    }

    getAttribute(name) {
      const value = bridge.getAttribute(this._liveId(), String(name));
      return value === null ? null : value;
    }
    hasAttribute(name) { return bridge.hasAttribute(this._liveId(), String(name)); }
    getAttributeNames() { return bridge.attributes(this._liveId()).map((attr) => attr.name); }
    setAttribute(name, value) {
      const attributeName = String(name);
      const oldValue = this.getAttribute(attributeName);
      const newValue = String(value);
      const result = afterMutation(bridge.setAttribute(this._liveId(), attributeName, newValue), {
        type: 'attributes',
        target: this,
        addedNodes: [],
        removedNodes: [],
        previousSibling: null,
        nextSibling: null,
        attributeName
      });
      if (oldValue !== newValue) notifyCustomElementAttributeChanged(this, attributeName, oldValue, newValue);
      return result;
    }
    removeAttribute(name) {
      const attributeName = String(name);
      const oldValue = this.getAttribute(attributeName);
      const result = afterMutation(bridge.removeAttribute(this._liveId(), attributeName), {
        type: 'attributes',
        target: this,
        addedNodes: [],
        removedNodes: [],
        previousSibling: null,
        nextSibling: null,
        attributeName
      });
      if (oldValue !== null) notifyCustomElementAttributeChanged(this, attributeName, oldValue, null);
      return result;
    }
    toggleAttribute(name, force = undefined) {
      const has = this.hasAttribute(name);
      if (force === true || (!has && force !== false)) {
        this.setAttribute(name, '');
        return true;
      }
      if (has && force !== true) this.removeAttribute(name);
      return false;
    }

    querySelector(selector) { return querySelectorCompat(this, selector); }
    querySelectorAll(selector) { return querySelectorAllCompat(this, selector); }
    getElementsByTagName(tagName) { return this.querySelectorAll(String(tagName)); }
    getElementsByClassName(classNames) {
      const selector = String(classNames).trim().split(/\s+/).filter(Boolean).map((name) => `.${name}`).join('');
      return selector ? this.querySelectorAll(selector) : [];
    }
    matches(selector) {
      const text = String(selector);
      if (text.includes('.\\:')) return selectorMatchesEscapedClasses(this, text);
      try {
        return splitSelectorList(text).some((part) => {
          const tokens = selectorTokens(part);
          if (!selectorFilterMayMatch(this, selectorRightmostFilter(tokens))) return false;
          return selectorMatchesElementFast(this, tokens);
        });
      } catch (_error) {
        const parent = this.parentNode || document;
        return parent.querySelectorAll(text).some((node) => node.__id === this.__id);
      }
    }
    closest(selector) {
      for (let node = this; node; node = node.parentElement) {
        if (node.matches(selector)) return node;
      }
      return null;
    }

    insertAdjacentElement(position, element) {
      assertNode(element);
      const where = String(position).toLowerCase();
      if (where === 'beforebegin') {
        if (!this.parentNode) return null;
        this.parentNode.insertBefore(element, this);
      } else if (where === 'afterbegin') {
        this.insertBefore(element, this.firstChild);
      } else if (where === 'beforeend') {
        this.appendChild(element);
      } else if (where === 'afterend') {
        if (!this.parentNode) return null;
        this.parentNode.insertBefore(element, this.nextSibling);
      } else {
        throw new DOMException('Invalid insertAdjacentElement position.', 'SyntaxError');
      }
      return element;
    }

    insertAdjacentHTML(position, html) {
      const where = String(position).toLowerCase();
      const nodes = parseHTMLFragment(html);
      if (!nodes.length) return;

      if (where === 'beforebegin') {
        if (!this.parentNode) return;
        insertNodesBefore(this.parentNode, this, nodes);
      } else if (where === 'afterbegin') {
        insertNodesBefore(this, this.firstChild, nodes);
      } else if (where === 'beforeend') {
        insertNodesBefore(this, null, nodes);
      } else if (where === 'afterend') {
        if (!this.parentNode) return;
        insertNodesBefore(this.parentNode, this.nextSibling, nodes);
      } else {
        throw new DOMException('Invalid insertAdjacentHTML position.', 'SyntaxError');
      }
    }

    insertAdjacentText(position, text) {
      this.insertAdjacentElement(position, document.createTextNode(String(text ?? '')));
    }

    append(...nodes) {
      for (const node of nodes) this.appendChild(typeof node === 'string' ? document.createTextNode(node) : node);
    }

    prepend(...nodes) {
      for (const node of nodes.reverse()) {
        this.insertBefore(typeof node === 'string' ? document.createTextNode(node) : node, this.firstChild);
      }
    }

    before(...nodes) {
      if (!this.parentNode) return;
      for (const node of nodes) this.parentNode.insertBefore(typeof node === 'string' ? document.createTextNode(node) : node, this);
    }

    after(...nodes) {
      if (!this.parentNode) return;
      let reference = this.nextSibling;
      for (const node of nodes) {
        const inserted = typeof node === 'string' ? document.createTextNode(node) : node;
        this.parentNode.insertBefore(inserted, reference);
      }
    }

    replaceWith(...nodes) {
      if (!this.parentNode) return;
      this.before(...nodes);
      this.remove();
    }

    focus() { activeElement = this; this.dispatchEvent(new Event('focus')); }
    blur() {
      if (activeElement === this) activeElement = document.body;
      this.dispatchEvent(new Event('blur'));
    }
    click() { this.dispatchEvent(new MouseEvent('click', { bubbles: true, cancelable: true, view: global })); }
    scrollIntoView() {}
    checkVisibility() {
      if (!this.isConnected || this.hidden) return false;
      const style = computedStyleFor(this);
      return style.display !== 'none' && style.visibility !== 'hidden';
    }
    getBoundingClientRect() { return new DOMRect(0, 0, 0, 0); }
    getClientRects() { return []; }
    get offsetWidth() { return 0; }
    get offsetHeight() { return 0; }
    get offsetTop() { return 0; }
    get offsetLeft() { return 0; }
    get clientWidth() { return 0; }
    get clientHeight() { return 0; }
    get scrollWidth() { return 0; }
    get scrollHeight() { return 0; }
  }

  class HTMLElement extends Element {
    get innerText() { return isNodeWrapper(this) ? this.textContent || '' : ''; }
    set innerText(value) { if (isNodeWrapper(this)) this.textContent = String(value ?? ''); }
    get offsetParent() {
      if (!isNodeWrapper(this)) return null;
      if (!this.isConnected || this.hidden || computedDisplay(this) === 'none') return null;
      const root = this.getRootNode ? this.getRootNode() : null;
      return this.parentElement || (root instanceof ShadowRoot ? root.host : null) || document.body || null;
    }
  }
  class HTMLAnchorElement extends HTMLElement {}
  class HTMLAreaElement extends HTMLElement {}
  class HTMLAudioElement extends HTMLElement {}
  class HTMLBaseElement extends HTMLElement {}
  class HTMLBodyElement extends HTMLElement {}
  class HTMLBRElement extends HTMLElement {}
  class HTMLButtonElement extends HTMLElement {}
  class HTMLCanvasElement extends HTMLElement {
    getContext() { return null; }
    toDataURL() { return 'data:,'; }
  }
  class HTMLDataElement extends HTMLElement {}
  class HTMLDataListElement extends HTMLElement {}
  class HTMLDetailsElement extends HTMLElement {}
  class HTMLDialogElement extends HTMLElement {
    show() { this.open = true; }
    showModal() { this.open = true; }
    close(returnValue = '') {
      this.returnValue = String(returnValue);
      this.open = false;
      this.dispatchEvent(new Event('close'));
    }
  }
  class HTMLDivElement extends HTMLElement {}
  class HTMLDListElement extends HTMLElement {}
  class HTMLEmbedElement extends HTMLElement {}
  class HTMLFieldSetElement extends HTMLElement {}
  class HTMLFormElement extends HTMLElement {}
  class HTMLHeadElement extends HTMLElement {}
  class HTMLHeadingElement extends HTMLElement {}
  class HTMLHRElement extends HTMLElement {}
  class HTMLHtmlElement extends HTMLElement {}
  class HTMLIFrameElement extends HTMLElement {
    get contentWindow() {
      if (!this._contentWindow) {
        const frameWindow = new EventTarget();
        installWindowIdentity(frameWindow);
        const frameDocument = new DetachedHTMLDocument('');
        frameDocument.defaultView = frameWindow;
        frameDocument.location = locationFromURL(this.getAttribute('src') || 'about:blank');

        frameWindow.top = global.top || global;
        frameWindow.parent = global;
        frameWindow.frameElement = this;
        frameWindow.document = frameDocument;
        frameWindow.location = frameDocument.location;
        frameWindow.closed = false;
        frameWindow.focus = () => {};
        frameWindow.blur = () => {};
        frameWindow.close = () => { frameWindow.closed = true; };
        frameWindow.postMessage = (message) => {
          frameWindow.dispatchEvent(new MessageEvent('message', {
            data: message,
            origin: global.location.origin,
            source: global
          }));
        };

        this._contentWindow = frameWindow;
      }
      return this._contentWindow;
    }
    get contentDocument() { return this.contentWindow.document; }
  }
  class HTMLImageElement extends HTMLElement {}
  class HTMLInputElement extends HTMLElement {
    get value() { return this.getAttribute('value') || ''; }
    set value(value) { this.setAttribute('value', String(value)); }
    get checked() { return this.hasAttribute('checked'); }
    set checked(value) { this.toggleAttribute('checked', Boolean(value)); }
    get disabled() { return this.hasAttribute('disabled'); }
    set disabled(value) { this.toggleAttribute('disabled', Boolean(value)); }
  }
  class HTMLLabelElement extends HTMLElement {}
  class HTMLLegendElement extends HTMLElement {}
  class HTMLLIElement extends HTMLElement {}
  class HTMLLinkElement extends HTMLElement {
    get disabled() { return this.hasAttribute('disabled'); }
    set disabled(value) {
      this.toggleAttribute('disabled', Boolean(value));
      const sheet = this.sheet;
      if (sheet) sheet.disabled = Boolean(value);
    }
    get sheet() {
      const rel = String(this.getAttribute('rel') || '').toLowerCase().split(/\s+/);
      return rel.includes('stylesheet') ? styleSheetForNode(this) : null;
    }
  }
  class HTMLMapElement extends HTMLElement {}
  class HTMLMediaElement extends HTMLElement {
    play() { return Promise.resolve(); }
    pause() {}
  }
  class HTMLMetaElement extends HTMLElement {}
  class HTMLMeterElement extends HTMLElement {}
  class HTMLModElement extends HTMLElement {}
  class HTMLObjectElement extends HTMLElement {}
  class HTMLOListElement extends HTMLElement {}
  class HTMLOptGroupElement extends HTMLElement {}
  class HTMLOptionElement extends HTMLElement {}
  class HTMLOutputElement extends HTMLElement {}
  class HTMLParagraphElement extends HTMLElement {}
  class HTMLParamElement extends HTMLElement {}
  class HTMLPictureElement extends HTMLElement {}
  class HTMLPreElement extends HTMLElement {}
  class HTMLProgressElement extends HTMLElement {}
  class HTMLQuoteElement extends HTMLElement {}
  class HTMLScriptElement extends HTMLElement {}
  class HTMLSelectElement extends HTMLElement {
    get value() {
      const selected = this.querySelector('option[selected]');
      const option = selected || this.querySelector('option');
      return option ? option.getAttribute('value') || option.textContent : '';
    }
    set value(value) {
      const text = String(value);
      for (const option of this.querySelectorAll('option')) {
        option.toggleAttribute('selected', (option.getAttribute('value') || option.textContent) === text);
      }
    }
  }
  class HTMLSlotElement extends HTMLElement {
    assignedNodes() { return []; }
    assignedElements() { return []; }
  }
  class HTMLSourceElement extends HTMLElement {}
  class HTMLSpanElement extends HTMLElement {}
  class HTMLStyleElement extends HTMLElement {
    get disabled() { return this.hasAttribute('disabled'); }
    set disabled(value) {
      this.toggleAttribute('disabled', Boolean(value));
      const sheet = this.sheet;
      if (sheet) sheet.disabled = Boolean(value);
    }
    get sheet() { return styleSheetForNode(this); }
  }
  class HTMLTableCaptionElement extends HTMLElement {}
  class HTMLTableCellElement extends HTMLElement {}
  class HTMLTableColElement extends HTMLElement {}
  class HTMLTableElement extends HTMLElement {}
  class HTMLTableRowElement extends HTMLElement {}
  class HTMLTableSectionElement extends HTMLElement {}
  class HTMLTemplateElement extends HTMLElement {
    get content() { return memo(this, '__templateContent', () => new DocumentFragment()); }
  }
  class HTMLTextAreaElement extends HTMLElement {
    get value() { return this.getAttribute('value') || this.textContent || ''; }
    set value(value) { this.textContent = String(value); }
  }
  class HTMLTimeElement extends HTMLElement {}
  class HTMLTitleElement extends HTMLElement {}
  class HTMLTrackElement extends HTMLElement {}
  class HTMLUListElement extends HTMLElement {}
  class HTMLUnknownElement extends HTMLElement {}
  class HTMLVideoElement extends HTMLMediaElement {}

  class SVGElement extends Element {}
  class SVGGraphicsElement extends SVGElement {}
  class SVGSVGElement extends SVGGraphicsElement {}
  class SVGPathElement extends SVGGraphicsElement {}
  class SVGCircleElement extends SVGGraphicsElement {}
  class SVGEllipseElement extends SVGGraphicsElement {}
  class SVGLineElement extends SVGGraphicsElement {}
  class SVGRectElement extends SVGGraphicsElement {}
  class SVGPolygonElement extends SVGGraphicsElement {}
  class SVGPolylineElement extends SVGGraphicsElement {}
  class SVGGElement extends SVGGraphicsElement {}
  class SVGDefsElement extends SVGElement {}
  class SVGUseElement extends SVGGraphicsElement {}
  class SVGSymbolElement extends SVGElement {}
  class SVGTitleElement extends SVGElement {}
  class SVGDescElement extends SVGElement {}

  const htmlElementConstructors = {
    a: HTMLAnchorElement,
    area: HTMLAreaElement,
    audio: HTMLAudioElement,
    base: HTMLBaseElement,
    body: HTMLBodyElement,
    br: HTMLBRElement,
    button: HTMLButtonElement,
    canvas: HTMLCanvasElement,
    data: HTMLDataElement,
    datalist: HTMLDataListElement,
    details: HTMLDetailsElement,
    dialog: HTMLDialogElement,
    div: HTMLDivElement,
    dl: HTMLDListElement,
    embed: HTMLEmbedElement,
    fieldset: HTMLFieldSetElement,
    form: HTMLFormElement,
    h1: HTMLHeadingElement,
    h2: HTMLHeadingElement,
    h3: HTMLHeadingElement,
    h4: HTMLHeadingElement,
    h5: HTMLHeadingElement,
    h6: HTMLHeadingElement,
    head: HTMLHeadElement,
    hr: HTMLHRElement,
    html: HTMLHtmlElement,
    iframe: HTMLIFrameElement,
    img: HTMLImageElement,
    input: HTMLInputElement,
    label: HTMLLabelElement,
    legend: HTMLLegendElement,
    li: HTMLLIElement,
    link: HTMLLinkElement,
    map: HTMLMapElement,
    meta: HTMLMetaElement,
    meter: HTMLMeterElement,
    del: HTMLModElement,
    ins: HTMLModElement,
    object: HTMLObjectElement,
    ol: HTMLOListElement,
    optgroup: HTMLOptGroupElement,
    option: HTMLOptionElement,
    output: HTMLOutputElement,
    p: HTMLParagraphElement,
    param: HTMLParamElement,
    picture: HTMLPictureElement,
    pre: HTMLPreElement,
    progress: HTMLProgressElement,
    q: HTMLQuoteElement,
    blockquote: HTMLQuoteElement,
    script: HTMLScriptElement,
    select: HTMLSelectElement,
    slot: HTMLSlotElement,
    source: HTMLSourceElement,
    span: HTMLSpanElement,
    style: HTMLStyleElement,
    caption: HTMLTableCaptionElement,
    td: HTMLTableCellElement,
    th: HTMLTableCellElement,
    col: HTMLTableColElement,
    colgroup: HTMLTableColElement,
    table: HTMLTableElement,
    tr: HTMLTableRowElement,
    tbody: HTMLTableSectionElement,
    thead: HTMLTableSectionElement,
    tfoot: HTMLTableSectionElement,
    template: HTMLTemplateElement,
    textarea: HTMLTextAreaElement,
    time: HTMLTimeElement,
    title: HTMLTitleElement,
    track: HTMLTrackElement,
    ul: HTMLUListElement,
    video: HTMLVideoElement
  };

  const svgElementConstructors = {
    svg: SVGSVGElement,
    path: SVGPathElement,
    circle: SVGCircleElement,
    ellipse: SVGEllipseElement,
    line: SVGLineElement,
    rect: SVGRectElement,
    polygon: SVGPolygonElement,
    polyline: SVGPolylineElement,
    g: SVGGElement,
    defs: SVGDefsElement,
    use: SVGUseElement,
    symbol: SVGSymbolElement,
    title: SVGTitleElement,
    desc: SVGDescElement
  };

  function applyElementNamespace(element, namespace) {
    if (!(element instanceof Element)) return element;
    const value = String(namespace || '');
    defineValue(element, '__namespaceURI', value);
    if (value === 'http://www.w3.org/2000/svg') {
      const Constructor = svgElementConstructors[element.localName] || SVGElement;
      Object.setPrototypeOf(element, Constructor.prototype);
    } else if (value === 'http://www.w3.org/1999/xhtml') {
      const Constructor = htmlElementConstructors[element.localName] || (element.localName.includes('-') ? HTMLElement : HTMLUnknownElement);
      Object.setPrototypeOf(element, Constructor.prototype);
    }
    return element;
  }

  function setDocumentWriteState(ownerDocument, state) {
    defineValue(ownerDocument, '__writeState', state);
  }

  function documentOpen(ownerDocument) {
    if (ownerDocument.head) ownerDocument.head.innerHTML = '';
    if (ownerDocument.body) ownerDocument.body.innerHTML = '';
    setDocumentReadyState('loading');
    setDocumentWriteState(ownerDocument, {
      mode: 'open',
      parent: ownerDocument.body || ownerDocument.documentElement,
      anchor: null
    });
    return ownerDocument;
  }

  function documentClose(ownerDocument) {
    setDocumentWriteState(ownerDocument, null);
  }

  function documentWrite(ownerDocument, text) {
    const value = String(text ?? '');
    if (!value) return;

    const nodes = parseHTMLFragment(value);
    if (!nodes.length) return;

    const current = ownerDocument.currentScript;
    let state = ownerDocument.__writeState || null;
    if (current && current.parentNode) {
      if (!state || state.mode !== 'script' || state.script !== current) {
        state = {
          mode: 'script',
          script: current,
          parent: current.parentNode,
          anchor: current
        };
        setDocumentWriteState(ownerDocument, state);
      }
    } else if (!state || state.mode !== 'open') {
      if (documentReadyState !== 'loading') {
        documentOpen(ownerDocument);
        state = ownerDocument.__writeState || null;
      } else {
        state = {
          mode: 'open',
          parent: ownerDocument.body || ownerDocument.documentElement,
          anchor: null
        };
        setDocumentWriteState(ownerDocument, state);
      }
    }

    if (!state) return;

    if (state.mode === 'script') {
      const anchor = state.anchor && state.anchor.parentNode ? state.anchor : current;
      const parent = anchor && anchor.parentNode ? anchor.parentNode : (ownerDocument.body || ownerDocument.documentElement);
      if (!parent) return;
      const last = insertNodesBefore(parent, anchor ? anchor.nextSibling : null, nodes);
      if (last) {
        state.anchor = last;
        state.parent = parent;
      }
      return;
    }

    const parent = state.parent && state.parent.isConnected
      ? state.parent
      : (ownerDocument.body || ownerDocument.documentElement);
    if (!parent) return;
    const last = insertNodesBefore(parent, null, nodes);
    if (last) {
      state.anchor = last;
      state.parent = parent;
    }
  }

  function copyCloneMetadata(source, clone) {
    if (!source || !clone) return;
    if (source.__namespaceURI) applyElementNamespace(clone, source.__namespaceURI);
    const sourceChildren = source.childNodes || [];
    const cloneChildren = clone.childNodes || [];
    for (let index = 0; index < sourceChildren.length && index < cloneChildren.length; index++) {
      copyCloneMetadata(sourceChildren[index], cloneChildren[index]);
    }
  }

  class Document extends Node {
    get documentElement() { return wrapNode(bridge.documentElement()); }
    get head() { return wrapNode(bridge.head()); }
    get body() { return wrapNode(bridge.body()); }
    get activeElement() { return activeElement || this.body || this.documentElement; }
    get readyState() { return documentReadyState; }
    set readyState(value) { setDocumentReadyState(value); }
    get characterSet() { return 'UTF-8'; }
    get charset() { return 'UTF-8'; }
    get contentType() { return 'text/html'; }
    get compatMode() { return 'CSS1Compat'; }
    get hidden() { return false; }
    get visibilityState() { return 'visible'; }
    get currentScript() { return this._currentScript || null; }
    set currentScript(value) { this._currentScript = value; }
    get baseURI() { return global.location.href || ''; }
    get URL() { return this.baseURI; }
    get documentURI() { return this.baseURI; }
    get referrer() { return ''; }
    get location() { return global.location; }
    set location(value) { global.location.assign(value); }
    get defaultView() { return global; }
    get implementation() { return domImplementation; }
    get domain() { return this._domain || (global.location && global.location.hostname) || ''; }
    set domain(value) { this._domain = String(value); }
    get cookie() { return getCookieString(); }
    set cookie(value) { setCookieString(value); }
    get scripts() { return this.getElementsByTagName('script'); }
    get links() { return this.getElementsByTagName('link'); }
    get images() { return this.getElementsByTagName('img'); }
    get styleSheets() { return styleSheetsForDocument(); }
    get adoptedStyleSheets() { return adoptedStyleSheets.slice(); }
    set adoptedStyleSheets(value) {
      if (!Array.isArray(value)) throw new TypeError('adoptedStyleSheets must be an array');
      for (const sheet of value) {
        if (!(sheet instanceof CSSStyleSheet)) throw new TypeError('adoptedStyleSheets entries must be CSSStyleSheet');
      }
      adoptedStyleSheets = value.slice();
      cssomVersion++;
    }
    get title() {
      const title = this.querySelector('title');
      return title ? title.textContent : '';
    }
    set title(value) {
      let title = this.querySelector('title');
      if (!title) {
        title = this.createElement('title');
        (this.head || this.documentElement).appendChild(title);
      }
      title.textContent = String(value);
    }

    createElement(tagName) {
      const localName = String(tagName).toLowerCase();
      const id = bridge.createElement(localName);
      if (customElementsRegistry) {
        const customElement = customElementsRegistry._construct(localName, id);
        if (customElement) return customElement;
      }
      return wrapNode(id);
    }
    createElementNS(namespaceURI, qualifiedName) {
      const namespace = String(namespaceURI || '');
      const localName = String(qualifiedName).split(':').pop().toLowerCase();
      const id = bridge.createElement(localName);
      if (customElementsRegistry && namespace === 'http://www.w3.org/1999/xhtml') {
        const customElement = customElementsRegistry._construct(localName, id);
        if (customElement) return applyElementNamespace(customElement, namespace);
      }
      const element = wrapNode(id);
      applyElementNamespace(element, namespace);
      return element;
    }
    createTextNode(text) { return wrapNode(bridge.createTextNode(String(text ?? ''))); }
    createComment(text) { return wrapNode(bridge.createComment(String(text ?? ''))); }
    createDocumentFragment() { return new DocumentFragment(); }
    getElementById(id) { return wrapNode(bridge.getElementById(String(id))); }
    querySelector(selector) { return querySelectorCompat(this, selector); }
    querySelectorAll(selector) { return querySelectorAllCompat(this, selector); }
    getElementsByTagName(tagName) { return this.querySelectorAll(String(tagName)); }
    getElementsByClassName(classNames) {
      const selector = String(classNames).trim().split(/\s+/).filter(Boolean).map((name) => `.${name}`).join('');
      return selector ? this.querySelectorAll(selector) : [];
    }
    getElementsByName(name) { return this.querySelectorAll(`[name="${String(name).replace(/"/g, '\\"')}"]`); }
    createEvent(type) {
      if (/mouse/i.test(type)) return new MouseEvent('');
      if (/keyboard/i.test(type)) return new KeyboardEvent('');
      return new Event('');
    }
    createRange() { return new Range(this); }
    getSelection() { return selection; }
    hasFocus() { return true; }
    elementFromPoint() { return this.body; }
    adoptNode(node) { return node; }
    importNode(node, deep = false) { return node.cloneNode ? node.cloneNode(Boolean(deep)) : node; }
    open() { return documentOpen(this); }
    close() { documentClose(this); }
    write(...parts) { documentWrite(this, parts.map((part) => String(part ?? '')).join('')); }
    writeln(...parts) { documentWrite(this, parts.map((part) => String(part ?? '')).join('') + '\n'); }
    __setCurrentScript(id) {
      this.currentScript = id == null ? null : wrapNode(id);
      setDocumentWriteState(this, null);
    }
  }

  class DetachedHTMLDocument extends EventTarget {
    constructor(title = '') {
      super();
      this.nodeType = 9;
      this.nodeName = '#document';
      this.readyState = 'complete';
      this.characterSet = 'UTF-8';
      this.charset = 'UTF-8';
      this.contentType = 'text/html';
      this.compatMode = 'CSS1Compat';
      this.implementation = domImplementation;
      this.location = global.location;
      this.defaultView = global;

      this.documentElement = document.createElement('html');
      this.head = document.createElement('head');
      this.body = document.createElement('body');
      this.documentElement.appendChild(this.head);
      this.documentElement.appendChild(this.body);
      this.title = String(title ?? '');
    }

    get childNodes() { return [this.documentElement]; }
    get firstChild() { return this.documentElement; }
    get lastChild() { return this.documentElement; }
    get children() { return [this.documentElement]; }
    get activeElement() { return this.body; }
    get baseURI() { return global.location.href || ''; }
    get URL() { return this.baseURI; }
    get documentURI() { return this.baseURI; }
    get referrer() { return ''; }
    get scripts() { return this.getElementsByTagName('script'); }
    get links() { return this.getElementsByTagName('link'); }
    get images() { return this.getElementsByTagName('img'); }
    get styleSheets() {
      const list = [];
      defineValue(list, 'item', function(index) { return this[Number(index)] || null; });
      return list;
    }
    get adoptedStyleSheets() { return []; }
    set adoptedStyleSheets(value) {
      if (!Array.isArray(value)) throw new TypeError('adoptedStyleSheets must be an array');
    }
    get title() {
      const title = this.head.querySelector('title');
      return title ? title.textContent : '';
    }
    set title(value) {
      let title = this.head.querySelector('title');
      if (!title) {
        title = document.createElement('title');
        this.head.appendChild(title);
      }
      title.textContent = String(value ?? '');
    }

    createElement(tagName) { return document.createElement(tagName); }
    createElementNS(namespaceURI, qualifiedName) { return document.createElementNS(namespaceURI, qualifiedName); }
    createTextNode(text) { return document.createTextNode(text); }
    createComment(text) { return document.createComment(text); }
    createDocumentFragment() { return new DocumentFragment(); }
    createEvent(type) { return document.createEvent(type); }
    createRange() { return document.createRange(); }
    getSelection() { return selection; }
    hasFocus() { return false; }
    elementFromPoint() { return this.body; }
    adoptNode(node) { return node; }
    importNode(node, deep = false) { return node && node.cloneNode ? node.cloneNode(Boolean(deep)) : node; }

    getElementById(id) {
      const escaped = String(id).replace(/\\/g, '\\\\').replace(/"/g, '\\"');
      return this.querySelector(`[id="${escaped}"]`);
    }

    querySelector(selector) { return this.querySelectorAll(selector)[0] || null; }
    querySelectorAll(selector) {
      const root = this.documentElement;
      const results = root.querySelectorAll(selector);
      return root.matches(selector) ? [root, ...results] : results;
    }
    getElementsByTagName(tagName) {
      const name = String(tagName).toLowerCase();
      if (name === '*') return [this.documentElement, ...this.documentElement.querySelectorAll('*')];
      const results = this.documentElement.getElementsByTagName(name);
      return this.documentElement.localName === name ? [this.documentElement, ...results] : results;
    }
    getElementsByClassName(classNames) {
      const selector = String(classNames).trim().split(/\s+/).filter(Boolean).map((name) => `.${name}`).join('');
      return selector ? this.querySelectorAll(selector) : [];
    }
    getElementsByName(name) { return this.querySelectorAll(`[name="${String(name).replace(/"/g, '\\"')}"]`); }
  }

  class DOMImplementation {
    createHTMLDocument(title = '') { return new DetachedHTMLDocument(title); }
    hasFeature() { return true; }
  }

  const domImplementation = new DOMImplementation();

  function wrapNode(id) {
    if (!id) return null;
    syncMutationCache();
    if (!bridge.hasNode(id)) return null;
    if (wrapperCache.has(id)) return wrapperCache.get(id);

    const type = bridge.nodeType(id);
    let node;
    if (type === 9) node = new Document(id);
    else if (type === 3) node = new Text(id);
    else if (type === 8) node = new Comment(id);
    else if (type === 1) {
      const tagName = bridge.tagName(id).toLowerCase();
      const Constructor = htmlElementConstructors[tagName] || (tagName.includes('-') ? HTMLElement : HTMLUnknownElement);
      node = new Constructor(id);
    } else node = new Node(id);

    wrapperCache.set(id, node);
    return node;
  }

  const document = wrapNode(bridge.documentNode());

  class CustomElementRegistry {
    constructor() {
      this._definitions = new Map();
      this._whenDefined = new Map();
    }

    define(name, constructor) {
      const tagName = this._normalizeName(name);
      if (typeof constructor !== 'function') {
        throw new TypeError('Custom element constructor must be a function');
      }
      if (!(constructor.prototype instanceof HTMLElement)) {
        throw new TypeError('Custom element constructor must extend HTMLElement');
      }
      if (this._definitions.has(tagName)) {
        throw new DOMException(`Custom element already defined: ${tagName}`, 'NotSupportedError');
      }

      this._definitions.set(tagName, constructor);
      const deferred = this._whenDefined.get(tagName);
      if (deferred) deferred.resolve(constructor);

      for (const element of document.querySelectorAll(tagName)) {
        this._upgrade(element);
      }
    }

    get(name) {
      return this._definitions.get(String(name).toLowerCase()) || undefined;
    }

    whenDefined(name) {
      const tagName = this._normalizeName(name);
      const found = this.get(tagName);
      if (found) return Promise.resolve(found);
      if (!this._whenDefined.has(tagName)) {
        let resolve;
        const promise = new Promise((done) => { resolve = done; });
        this._whenDefined.set(tagName, { promise, resolve });
      }
      return this._whenDefined.get(tagName).promise;
    }

    upgrade(root) {
      if (root instanceof Element) this._upgrade(root);
      if (root && root.querySelectorAll) {
        for (const element of root.querySelectorAll('*')) this._upgrade(element);
      }
    }

    _normalizeName(name) {
      const tagName = String(name).toLowerCase();
      if (!tagName.includes('-')) {
        throw new DOMException('Custom element name must contain a hyphen.', 'SyntaxError');
      }
      return tagName;
    }

    _construct(tagName, id) {
      const constructor = this.get(tagName);
      if (!constructor) return null;

      let element = null;
      pendingCustomElementNodeId = id;
      try {
        element = new constructor();
      } catch (error) {
        console.error(error);
        return null;
      } finally {
        pendingCustomElementNodeId = null;
      }

      if (!(element instanceof HTMLElement)) return null;
      if (!isNodeWrapper(element)) attachNodeId(element, id);
      wrapperCache.set(id, element);
      defineValue(element, '__customElementConstructor', constructor);
      return element;
    }

    _upgrade(element) {
      if (!(element instanceof Element)) return;
      const constructor = this.get(element.localName);
      if (!constructor) return;
      if (element.__customElementConstructor === constructor) {
        invokeCustomElementConnected(element);
        return element;
      }

      const upgraded = this._construct(element.localName, element.__id);
      if (!upgraded) return;

      invokeCustomElementConnected(upgraded);
      return upgraded;
    }
  }

  class DOMRectReadOnly {
    constructor(x = 0, y = 0, width = 0, height = 0) {
      this.x = Number.isFinite(Number(x)) ? Number(x) : 0;
      this.y = Number.isFinite(Number(y)) ? Number(y) : 0;
      this.width = Number.isFinite(Number(width)) ? Number(width) : 0;
      this.height = Number.isFinite(Number(height)) ? Number(height) : 0;
    }

    get top() { return Math.min(this.y, this.y + this.height); }
    get right() { return Math.max(this.x, this.x + this.width); }
    get bottom() { return Math.max(this.y, this.y + this.height); }
    get left() { return Math.min(this.x, this.x + this.width); }
    toJSON() {
      return {
        x: this.x,
        y: this.y,
        width: this.width,
        height: this.height,
        top: this.top,
        right: this.right,
        bottom: this.bottom,
        left: this.left
      };
    }

    static fromRect(other = {}) {
      return new DOMRectReadOnly(other.x, other.y, other.width, other.height);
    }
  }

  class DOMRect extends DOMRectReadOnly {
    static fromRect(other = {}) {
      return new DOMRect(other.x, other.y, other.width, other.height);
    }
  }

  function nodeLength(node) {
    if (!node) return 0;
    if (node.nodeType === 3) return node.data.length;
    return node.childNodes.length;
  }

  class Range {
    constructor(ownerDocument = document) {
      this._startContainer = ownerDocument;
      this._startOffset = 0;
      this._endContainer = ownerDocument;
      this._endOffset = 0;
    }

    get startContainer() { return this._startContainer; }
    get startOffset() { return this._startOffset; }
    get endContainer() { return this._endContainer; }
    get endOffset() { return this._endOffset; }
    get collapsed() { return this._startContainer === this._endContainer && this._startOffset === this._endOffset; }
    get commonAncestorContainer() { return this._startContainer; }

    setStart(node, offset) {
      assertRangeBoundary(node, offset);
      this._startContainer = node;
      this._startOffset = Number(offset);
    }

    setEnd(node, offset) {
      assertRangeBoundary(node, offset);
      this._endContainer = node;
      this._endOffset = Number(offset);
    }

    setStartBefore(node) {
      if (!node.parentNode) throw new DOMException('Node has no parent.', 'InvalidNodeTypeError');
      this.setStart(node.parentNode, node.parentNode.childNodes.indexOf(node));
    }

    setStartAfter(node) {
      if (!node.parentNode) throw new DOMException('Node has no parent.', 'InvalidNodeTypeError');
      this.setStart(node.parentNode, node.parentNode.childNodes.indexOf(node) + 1);
    }

    setEndBefore(node) {
      if (!node.parentNode) throw new DOMException('Node has no parent.', 'InvalidNodeTypeError');
      this.setEnd(node.parentNode, node.parentNode.childNodes.indexOf(node));
    }

    setEndAfter(node) {
      if (!node.parentNode) throw new DOMException('Node has no parent.', 'InvalidNodeTypeError');
      this.setEnd(node.parentNode, node.parentNode.childNodes.indexOf(node) + 1);
    }

    selectNode(node) {
      if (!node.parentNode) throw new DOMException('Node has no parent.', 'InvalidNodeTypeError');
      const index = node.parentNode.childNodes.indexOf(node);
      this._startContainer = node.parentNode;
      this._startOffset = index;
      this._endContainer = node.parentNode;
      this._endOffset = index + 1;
    }

    selectNodeContents(node) {
      this._startContainer = node;
      this._startOffset = 0;
      this._endContainer = node;
      this._endOffset = nodeLength(node);
    }

    collapse(toStart = false) {
      if (toStart) {
        this._endContainer = this._startContainer;
        this._endOffset = this._startOffset;
      } else {
        this._startContainer = this._endContainer;
        this._startOffset = this._endOffset;
      }
    }

    cloneRange() {
      const range = new Range();
      range._startContainer = this._startContainer;
      range._startOffset = this._startOffset;
      range._endContainer = this._endContainer;
      range._endOffset = this._endOffset;
      return range;
    }

    detach() {}
    getBoundingClientRect() { return new DOMRect(0, 0, 0, 0); }
    getClientRects() { return []; }
    toString() {
      if (this._startContainer === this._endContainer && this._startContainer.nodeType === 3) {
        return this._startContainer.data.slice(this._startOffset, this._endOffset);
      }
      return this._startContainer.textContent || '';
    }

    createContextualFragment(html) {
      return fragmentFromHTML(html);
    }
  }

  defineValue(Range, 'START_TO_START', 0, true);
  defineValue(Range, 'START_TO_END', 1, true);
  defineValue(Range, 'END_TO_END', 2, true);
  defineValue(Range, 'END_TO_START', 3, true);

  function assertRangeBoundary(node, offset) {
    if (!node || typeof node.nodeType !== 'number') throw new TypeError('Boundary container must be a Node');
    const numericOffset = Number(offset);
    if (!Number.isInteger(numericOffset) || numericOffset < 0 || numericOffset > nodeLength(node)) {
      throw new DOMException('Boundary offset is outside the node.', 'IndexSizeError');
    }
  }

  class Selection {
    constructor() {
      this._ranges = [];
    }

    get rangeCount() { return this._ranges.length; }
    get isCollapsed() { return this.rangeCount === 0 || this._ranges[0].collapsed; }
    get type() { return this.rangeCount === 0 ? 'None' : (this.isCollapsed ? 'Caret' : 'Range'); }
    get anchorNode() { return this.rangeCount ? this._ranges[0].startContainer : null; }
    get anchorOffset() { return this.rangeCount ? this._ranges[0].startOffset : 0; }
    get focusNode() { return this.rangeCount ? this._ranges[0].endContainer : null; }
    get focusOffset() { return this.rangeCount ? this._ranges[0].endOffset : 0; }

    getRangeAt(index) {
      if (index < 0 || index >= this._ranges.length) throw new DOMException('Range index is invalid.', 'IndexSizeError');
      return this._ranges[index];
    }

    removeAllRanges() { this._ranges = []; }
    addRange(range) {
      if (!(range instanceof Range)) throw new TypeError('addRange expects Range');
      this._ranges = [range];
    }
    removeRange(range) { this._ranges = this._ranges.filter((candidate) => candidate !== range); }
    collapse(node, offset = 0) {
      const range = new Range();
      range.setStart(node, offset);
      range.collapse(true);
      this._ranges = [range];
    }
    selectAllChildren(node) {
      const range = new Range();
      range.selectNodeContents(node);
      this._ranges = [range];
    }
    toString() { return this.rangeCount ? this._ranges[0].toString() : ''; }
  }

  const selection = new Selection();

  function encodeFormComponent(value) {
    return encodeURIComponent(String(value)).replace(/%20/g, '+');
  }

  function decodeFormComponent(value) {
    return decodeURIComponent(String(value).replace(/\+/g, ' '));
  }

  class URLSearchParams {
    constructor(init = undefined) {
      this._entries = [];
      if (init == null) return;
      if (init instanceof URLSearchParams) {
        this._entries = init._entries.map((entry) => [entry[0], entry[1]]);
      } else if (typeof init === 'string') {
        const query = init.startsWith('?') ? init.slice(1) : init;
        if (!query) return;
        for (const part of query.split('&')) {
          if (!part) continue;
          const index = part.indexOf('=');
          if (index < 0) this.append(decodeFormComponent(part), '');
          else this.append(decodeFormComponent(part.slice(0, index)), decodeFormComponent(part.slice(index + 1)));
        }
      } else if (typeof init[Symbol.iterator] === 'function') {
        for (const pair of init) {
          if (!pair || pair.length < 2) throw new TypeError('URLSearchParams entry must be a pair');
          this.append(pair[0], pair[1]);
        }
      } else if (typeof init === 'object') {
        for (const key of Object.keys(init)) this.append(key, init[key]);
      }
    }

    append(name, value) { this._entries.push([String(name), String(value)]); }
    delete(name) {
      name = String(name);
      this._entries = this._entries.filter((entry) => entry[0] !== name);
    }
    get(name) {
      name = String(name);
      const found = this._entries.find((entry) => entry[0] === name);
      return found ? found[1] : null;
    }
    getAll(name) {
      name = String(name);
      return this._entries.filter((entry) => entry[0] === name).map((entry) => entry[1]);
    }
    has(name) {
      name = String(name);
      return this._entries.some((entry) => entry[0] === name);
    }
    set(name, value) {
      name = String(name);
      value = String(value);
      let replaced = false;
      const out = [];
      for (const entry of this._entries) {
        if (entry[0] === name) {
          if (!replaced) {
            out.push([name, value]);
            replaced = true;
          }
        } else {
          out.push(entry);
        }
      }
      if (!replaced) out.push([name, value]);
      this._entries = out;
    }
    sort() { this._entries.sort((left, right) => left[0].localeCompare(right[0])); }
    forEach(callback, thisArg = undefined) {
      for (const [name, value] of this._entries) callback.call(thisArg, value, name, this);
    }
    keys() { return this._entries.map((entry) => entry[0])[Symbol.iterator](); }
    values() { return this._entries.map((entry) => entry[1])[Symbol.iterator](); }
    entries() { return this._entries.map((entry) => [entry[0], entry[1]])[Symbol.iterator](); }
    get size() { return this._entries.length; }
    toString() {
      return this._entries.map(([name, value]) => `${encodeFormComponent(name)}=${encodeFormComponent(value)}`).join('&');
    }
    [Symbol.iterator]() { return this.entries(); }
  }

  function parseURL(input, base = undefined) {
    let raw = String(input).trim();
    if (base !== undefined && !/^[A-Za-z][A-Za-z0-9+.-]*:/.test(raw)) {
      raw = resolveURLAgainstBase(raw, base);
    }
    const match = /^([A-Za-z][A-Za-z0-9+.-]*:)(?:\/\/([^/?#]*))?([^?#]*)(\?[^#]*)?(#.*)?$/.exec(raw);
    if (!match) throw new TypeError(`Invalid URL: ${input}`);
    const authority = match[2] || '';
    let username = '';
    let password = '';
    let host = authority;
    const at = authority.lastIndexOf('@');
    if (at >= 0) {
      const auth = authority.slice(0, at);
      host = authority.slice(at + 1);
      const colon = auth.indexOf(':');
      username = decodeURIComponent(colon < 0 ? auth : auth.slice(0, colon));
      password = colon < 0 ? '' : decodeURIComponent(auth.slice(colon + 1));
    }
    const hostColon = host.lastIndexOf(':');
    const hostname = hostColon > 0 ? host.slice(0, hostColon) : host;
    const port = hostColon > 0 ? host.slice(hostColon + 1) : '';
    return {
      protocol: match[1].toLowerCase(),
      username,
      password,
      hostname,
      port,
      pathname: match[3] || '/',
      search: match[4] || '',
      hash: match[5] || ''
    };
  }

  function resolveURLAgainstBase(input, base) {
    const baseParts = typeof base === 'string' ? parseURL(base) : base._parts;
    if (input.startsWith('//')) return `${baseParts.protocol}${input}`;
    if (input.startsWith('/')) return `${baseParts.protocol}//${buildHost(baseParts)}${input}`;
    if (input.startsWith('?')) return `${baseParts.protocol}//${buildHost(baseParts)}${baseParts.pathname}${input}`;
    if (input.startsWith('#')) return `${baseParts.protocol}//${buildHost(baseParts)}${baseParts.pathname}${baseParts.search}${input}`;
    const basePath = baseParts.pathname || '/';
    const dir = basePath.slice(0, basePath.lastIndexOf('/') + 1);
    return `${baseParts.protocol}//${buildHost(baseParts)}${normalizePath(dir + input)}`;
  }

  function normalizePath(path) {
    const out = [];
    for (const segment of path.split('/')) {
      if (segment === '..') out.pop();
      else if (segment !== '.') out.push(segment);
    }
    return out.join('/') || '/';
  }

  function buildHost(parts) {
    return parts.port ? `${parts.hostname}:${parts.port}` : parts.hostname;
  }

  class URL {
    constructor(url, base = undefined) {
      this._parts = parseURL(url, base);
      this._searchParams = null;
    }

    get href() { return `${this.origin}${this.pathname}${this.search}${this.hash}`; }
    set href(value) { this._parts = parseURL(value); this._searchParams = null; }
    get origin() { return `${this.protocol}//${this.host}`; }
    get protocol() { return this._parts.protocol; }
    set protocol(value) { this._parts.protocol = String(value).replace(/:?$/, ':').toLowerCase(); }
    get username() { return this._parts.username; }
    set username(value) { this._parts.username = String(value); }
    get password() { return this._parts.password; }
    set password(value) { this._parts.password = String(value); }
    get host() { return buildHost(this._parts); }
    set host(value) {
      const text = String(value);
      const index = text.lastIndexOf(':');
      this._parts.hostname = index > 0 ? text.slice(0, index) : text;
      this._parts.port = index > 0 ? text.slice(index + 1) : '';
    }
    get hostname() { return this._parts.hostname; }
    set hostname(value) { this._parts.hostname = String(value); }
    get port() { return this._parts.port; }
    set port(value) { this._parts.port = String(value); }
    get pathname() { return this._parts.pathname; }
    set pathname(value) { this._parts.pathname = String(value).startsWith('/') ? String(value) : `/${value}`; }
    get search() { return this._parts.search; }
    set search(value) {
      const text = String(value);
      this._parts.search = text && !text.startsWith('?') ? `?${text}` : text;
      this._searchParams = null;
    }
    get hash() { return this._parts.hash; }
    set hash(value) {
      const text = String(value);
      this._parts.hash = text && !text.startsWith('#') ? `#${text}` : text;
    }
    get searchParams() {
      if (!this._searchParams) this._searchParams = new URLSearchParams(this.search);
      return this._searchParams;
    }
    toString() { return this.href; }
    toJSON() { return this.href; }
    static canParse(url, base = undefined) {
      try { new URL(url, base); return true; } catch (_error) { return false; }
    }
    static parse(url, base = undefined) {
      try { return new URL(url, base); } catch (_error) { return null; }
    }
  }

  class TextEncoder {
    get encoding() { return 'utf-8'; }

    encode(input = '') {
      const bytes = [];
      for (const char of String(input)) {
        const code = char.codePointAt(0);
        if (code <= 0x7f) {
          bytes.push(code);
        } else if (code <= 0x7ff) {
          bytes.push(0xc0 | (code >> 6), 0x80 | (code & 0x3f));
        } else if (code <= 0xffff) {
          bytes.push(0xe0 | (code >> 12), 0x80 | ((code >> 6) & 0x3f), 0x80 | (code & 0x3f));
        } else {
          bytes.push(
            0xf0 | (code >> 18),
            0x80 | ((code >> 12) & 0x3f),
            0x80 | ((code >> 6) & 0x3f),
            0x80 | (code & 0x3f)
          );
        }
      }
      return new Uint8Array(bytes);
    }

    encodeInto(source, destination) {
      if (!(destination instanceof Uint8Array)) throw new TypeError('encodeInto destination must be Uint8Array');
      let read = 0;
      let written = 0;
      for (const char of String(source)) {
        const bytes = this.encode(char);
        if (written + bytes.length > destination.length) break;
        destination.set(bytes, written);
        written += bytes.length;
        read += char.length;
      }
      return { read, written };
    }
  }

  class TextDecoder {
    constructor(label = 'utf-8', options = {}) {
      const normalized = String(label || 'utf-8').toLowerCase();
      if (normalized !== 'utf-8' && normalized !== 'utf8') {
        throw new RangeError('Only utf-8 TextDecoder is implemented');
      }
      this.encoding = 'utf-8';
      this.fatal = Boolean(options.fatal);
      this.ignoreBOM = Boolean(options.ignoreBOM);
    }

    decode(input = new Uint8Array()) {
      const bytes = input instanceof Uint8Array
        ? input
        : ArrayBuffer.isView(input)
          ? new Uint8Array(input.buffer, input.byteOffset, input.byteLength)
          : input instanceof ArrayBuffer
            ? new Uint8Array(input)
            : new Uint8Array();
      let out = '';
      for (let index = 0; index < bytes.length;) {
        const first = bytes[index++];
        let code = first;
        let needed = 0;
        if (first >= 0xf0) {
          code = first & 0x07;
          needed = 3;
        } else if (first >= 0xe0) {
          code = first & 0x0f;
          needed = 2;
        } else if (first >= 0xc0) {
          code = first & 0x1f;
          needed = 1;
        } else if (first >= 0x80) {
          if (this.fatal) throw new TypeError('Invalid UTF-8 data');
          out += '\uFFFD';
          continue;
        }

        let valid = true;
        for (let offset = 0; offset < needed; offset++) {
          const next = bytes[index++];
          if ((next & 0xc0) !== 0x80) {
            valid = false;
            break;
          }
          code = (code << 6) | (next & 0x3f);
        }
        if (!valid) {
          if (this.fatal) throw new TypeError('Invalid UTF-8 data');
          out += '\uFFFD';
        } else {
          out += String.fromCodePoint(code);
        }
      }
      return out;
    }
  }

  class Headers {
    constructor(init = undefined) {
      this._map = Object.create(null);
      if (init == null) return;
      if (init instanceof Headers) {
        init.forEach((value, name) => this.append(name, value));
      } else if (typeof init[Symbol.iterator] === 'function') {
        for (const pair of init) this.append(pair[0], pair[1]);
      } else {
        for (const name of Object.keys(init)) this.append(name, init[name]);
      }
    }

    _name(name) {
      const text = String(name).toLowerCase();
      if (!/^[!#$%&'*+\-.^_`|~0-9a-z]+$/.test(text)) throw new TypeError('Invalid header name');
      return text;
    }
    _value(value) {
      const text = String(value).trim();
      if (/[\0\r\n]/.test(text)) throw new TypeError('Invalid header value');
      return text;
    }
    append(name, value) {
      name = this._name(name);
      value = this._value(value);
      this._map[name] = this._map[name] == null ? value : `${this._map[name]}, ${value}`;
    }
    set(name, value) { this._map[this._name(name)] = this._value(value); }
    get(name) { return this._map[this._name(name)] ?? null; }
    has(name) { return this._map[this._name(name)] != null; }
    delete(name) { delete this._map[this._name(name)]; }
    forEach(callback, thisArg = undefined) {
      for (const name of Object.keys(this._map).sort()) callback.call(thisArg, this._map[name], name, this);
    }
    entries() { return Object.keys(this._map).sort().map((name) => [name, this._map[name]])[Symbol.iterator](); }
    keys() { return Object.keys(this._map).sort()[Symbol.iterator](); }
    values() { return Object.keys(this._map).sort().map((name) => this._map[name])[Symbol.iterator](); }
    [Symbol.iterator]() { return this.entries(); }
  }

  function encodeUtf8(text) {
    const out = [];
    for (let i = 0; i < text.length; ++i) {
      const code = text.charCodeAt(i);
      if (code < 0x80) out.push(code);
      else if (code < 0x800) out.push(0xc0 | (code >> 6), 0x80 | (code & 0x3f));
      else out.push(0xe0 | (code >> 12), 0x80 | ((code >> 6) & 0x3f), 0x80 | (code & 0x3f));
    }
    return new Uint8Array(out);
  }

  function decodeUtf8(bytes) {
    let out = '';
    for (let i = 0; i < bytes.length;) {
      const b1 = bytes[i++];
      if (b1 < 0x80) {
        out += String.fromCharCode(b1);
      } else if (b1 < 0xe0) {
        const b2 = bytes[i++] & 0x3f;
        out += String.fromCharCode(((b1 & 0x1f) << 6) | b2);
      } else {
        const b2 = bytes[i++] & 0x3f;
        const b3 = bytes[i++] & 0x3f;
        out += String.fromCharCode(((b1 & 0x0f) << 12) | (b2 << 6) | b3);
      }
    }
    return out;
  }

  function blobPartBytes(part) {
    if (part == null) return new Uint8Array(0);
    if (part instanceof Uint8Array) return part;
    if (part instanceof ArrayBuffer) return new Uint8Array(part);
    if (ArrayBuffer.isView(part)) return new Uint8Array(part.buffer, part.byteOffset, part.byteLength);
    if (part instanceof Blob) return part._bytes;
    return encodeUtf8(String(part));
  }

  class Blob {
    constructor(parts = [], options = {}) {
      const chunks = [];
      let size = 0;
      for (const part of parts) {
        const bytes = blobPartBytes(part);
        chunks.push(bytes);
        size += bytes.length;
      }
      this._bytes = new Uint8Array(size);
      let offset = 0;
      for (const chunk of chunks) {
        this._bytes.set(chunk, offset);
        offset += chunk.length;
      }
      this.size = size;
      this.type = options.type ? String(options.type).toLowerCase() : '';
    }

    slice(start = 0, end = this.size, type = '') {
      const from = start < 0 ? Math.max(this.size + start, 0) : Math.min(start, this.size);
      const to = end < 0 ? Math.max(this.size + end, 0) : Math.min(end, this.size);
      const blob = new Blob([], { type });
      blob._bytes = this._bytes.slice(Math.min(from, to), Math.max(from, to));
      blob.size = blob._bytes.length;
      return blob;
    }
    arrayBuffer() {
      const buffer = new ArrayBuffer(this._bytes.length);
      new Uint8Array(buffer).set(this._bytes);
      return Promise.resolve(buffer);
    }
    text() {
      return Promise.resolve(decodeUtf8(this._bytes));
    }
  }

  class File extends Blob {
    constructor(parts, name, options = {}) {
      super(parts, options);
      this.name = String(name);
      this.lastModified = options.lastModified === undefined ? Date.now() : Number(options.lastModified);
    }
  }

  class Request {
    constructor(input, init = {}) {
      const source = input instanceof Request ? input : null;
      this.url = source ? source.url : new URL(String(input), global.location.href || undefined).href;
      this.method = String(init.method || (source && source.method) || 'GET').toUpperCase();
      this.headers = new Headers(init.headers || (source && source.headers) || undefined);
      this.body = init.body !== undefined ? init.body : source ? source.body : null;
      this.mode = init.mode || (source && source.mode) || 'cors';
      this.credentials = init.credentials || (source && source.credentials) || 'same-origin';
      this.cache = init.cache || (source && source.cache) || 'default';
      this.redirect = init.redirect || (source && source.redirect) || 'follow';
      this.referrer = init.referrer || (source && source.referrer) || 'about:client';
      this.signal = init.signal || (source && source.signal) || null;
    }

    clone() { return new Request(this); }
  }

  class Response {
    constructor(body = null, init = {}) {
      this.body = body;
      this.status = init.status === undefined ? 200 : Number(init.status);
      this.statusText = init.statusText === undefined ? '' : String(init.statusText);
      this.headers = new Headers(init.headers);
      this.url = init.url || '';
      this.redirected = false;
      this.type = 'default';
      this.ok = this.status >= 200 && this.status < 300;
    }

    text() {
      if (this.body instanceof Blob) return this.body.text();
      if (this.body == null) return Promise.resolve('');
      return Promise.resolve(String(this.body));
    }

    arrayBuffer() {
      if (this.body instanceof Blob) return this.body.arrayBuffer();
      return new Blob([this.body == null ? '' : String(this.body)]).arrayBuffer();
    }

    json() { return this.text().then((text) => JSON.parse(text)); }
    clone() {
      return new Response(this.body, {
        status: this.status,
        statusText: this.statusText,
        headers: this.headers,
        url: this.url
      });
    }

    static json(data, init = {}) {
      const headers = new Headers(init.headers);
      if (!headers.has('content-type')) headers.set('content-type', 'application/json');
      return new Response(JSON.stringify(data), { ...init, headers });
    }

    static error() {
      const response = new Response(null, { status: 0, statusText: '' });
      response.type = 'error';
      response.ok = false;
      return response;
    }

    static redirect(url, status = 302) {
      return new Response(null, { status, headers: { location: String(url) } });
    }
  }

  function responseHeadersFromHost(response) {
    const headers = new Headers();
    if (response && response.mimeType) headers.set('content-type', response.mimeType);
    return headers;
  }

  function loadHostResource(url, kind = 'other') {
    if (!host || typeof host.loadResource !== 'function') {
      throw new Error('resource loading is not available');
    }
    return host.loadResource(absoluteURL(url), kind);
  }

  class XMLHttpRequest extends EventTarget {
    constructor() {
      super();
      this.readyState = XMLHttpRequest.UNSENT;
      this.response = '';
      this.responseText = '';
      this.responseType = '';
      this.responseURL = '';
      this.status = 0;
      this.statusText = '';
      this.timeout = 0;
      this.withCredentials = false;
      this.upload = new EventTarget();
      this.onreadystatechange = null;
      this.onload = null;
      this.onerror = null;
      this.onabort = null;
      this.onloadend = null;
      this.ontimeout = null;
      this._method = 'GET';
      this._url = '';
      this._async = true;
      this._requestHeaders = new Headers();
      this._responseHeaders = new Headers();
      this._aborted = false;
    }

    open(method, url, async = true) {
      this._method = String(method || 'GET').toUpperCase();
      this._url = absoluteURL(url);
      this._async = async !== false;
      this._aborted = false;
      this.readyState = XMLHttpRequest.OPENED;
      this._fire('readystatechange');
    }

    setRequestHeader(name, value) {
      if (this.readyState !== XMLHttpRequest.OPENED) throw new DOMException('XMLHttpRequest is not open.', 'InvalidStateError');
      this._requestHeaders.append(name, value);
    }

    getResponseHeader(name) {
      return this._responseHeaders.get(name);
    }

    getAllResponseHeaders() {
      const lines = [];
      this._responseHeaders.forEach((value, name) => lines.push(`${name}: ${value}`));
      return lines.length ? lines.join('\r\n') + '\r\n' : '';
    }

    overrideMimeType(mimeType) {
      this._overrideMimeType = String(mimeType);
    }

    abort() {
      this._aborted = true;
      this.status = 0;
      this.statusText = '';
      this.readyState = XMLHttpRequest.DONE;
      this._fire('readystatechange');
      this._fire('abort');
      this._fire('loadend');
    }

    send(body = null) {
      if (this.readyState !== XMLHttpRequest.OPENED) throw new DOMException('XMLHttpRequest is not open.', 'InvalidStateError');
      const run = () => {
        if (this._aborted) return;
        let loaded = null;
        try {
          loaded = loadHostResource(this._url, 'other');
          this.status = Number(loaded.status || 200);
          this.statusText = this.status >= 200 && this.status < 300 ? 'OK' : '';
          this.responseURL = loaded.url || this._url;
          this._responseHeaders = responseHeadersFromHost(loaded);
          if (this._overrideMimeType) this._responseHeaders.set('content-type', this._overrideMimeType);
          this.responseText = loaded.body || '';
          this.response = this.responseType === 'json'
            ? (this.responseText ? JSON.parse(this.responseText) : null)
            : this.responseText;
        } catch (error) {
          if (this._aborted) return;
          this.status = 0;
          this.statusText = '';
          this.response = '';
          this.responseText = '';
          this.readyState = XMLHttpRequest.DONE;
          this._fire('readystatechange');
          this._fire('error');
          this._fire('loadend');
          return;
        }

        if (this._aborted) return;
        this.readyState = XMLHttpRequest.HEADERS_RECEIVED;
        this._fire('readystatechange');

        this.readyState = XMLHttpRequest.LOADING;
        this._fire('readystatechange');

        this.readyState = XMLHttpRequest.DONE;
        this._fire('readystatechange');
        this._fire('load');
        this._fire('loadend');
      };

      void body;
      if (this._async) global.setTimeout(run, 0);
      else run();
    }

    _fire(type) {
      this.dispatchEvent(new Event(type));
    }
  }

  for (const [name, value] of Object.entries({
    UNSENT: 0,
    OPENED: 1,
    HEADERS_RECEIVED: 2,
    LOADING: 3,
    DONE: 4
  })) {
    defineValue(XMLHttpRequest, name, value, true);
    defineValue(XMLHttpRequest.prototype, name, value, true);
  }

  class Storage {
    constructor() {
      this._store = Object.create(null);
    }

    get length() { return Object.keys(this._store).length; }
    key(index) { return Object.keys(this._store)[Number(index)] || null; }
    getItem(key) {
      key = String(key);
      return Object.prototype.hasOwnProperty.call(this._store, key) ? this._store[key] : null;
    }
    setItem(key, value) { this._store[String(key)] = String(value); }
    removeItem(key) { delete this._store[String(key)]; }
    clear() { this._store = Object.create(null); }
  }

  class XMLSerializer {
    serializeToString(node) {
      if (!node) return '';
      if (node.nodeType === 9) return node.documentElement ? node.documentElement.outerHTML : '';
      if (node.nodeType === 11) return node.childNodes.map((child) => this.serializeToString(child)).join('');
      if (node.nodeType === 3) return node.data;
      return node.outerHTML || node.innerHTML || '';
    }
  }

  class DOMParser {
    parseFromString(html) {
      const parsed = document.implementation && document.implementation.createHTMLDocument
        ? document.implementation.createHTMLDocument('')
        : document;
      if (parsed.body) parsed.body.innerHTML = String(html ?? '');
      return parsed;
    }
  }

  const NodeFilter = {
    SHOW_ALL: 0xFFFFFFFF,
    SHOW_ELEMENT: 0x1,
    SHOW_ATTRIBUTE: 0x2,
    SHOW_TEXT: 0x4,
    SHOW_COMMENT: 0x80,
    SHOW_DOCUMENT: 0x100,
    SHOW_DOCUMENT_TYPE: 0x200,
    SHOW_DOCUMENT_FRAGMENT: 0x400,
    FILTER_ACCEPT: 1,
    FILTER_REJECT: 2,
    FILTER_SKIP: 3
  };

  const timers = new Map();
  let nextTimerId = 1;
  let timerNow = 0;

  function normalizeDelay(delay) {
    const value = Number(delay);
    if (!Number.isFinite(value) || value < 0) return 0;
    return value;
  }

  function callTimerCallback(timer) {
    if (typeof timer.callback === 'function') {
      timer.callback(...timer.args);
    }
  }

  function locationFromURL(href) {
    let url = null;
    try {
      url = href ? new URL(href) : null;
    } catch (_error) {
      url = null;
    }
    const location = {
      href: url ? url.href : String(href || ''),
      protocol: url ? url.protocol : '',
      host: url ? url.host : '',
      hostname: url ? url.hostname : '',
      port: url ? url.port : '',
      pathname: url ? url.pathname : '',
      search: url ? url.search : '',
      hash: url ? url.hash : '',
      origin: url ? url.origin : '',
      assign(value) {
        const next = new URL(value, this.href || undefined);
        Object.assign(this, locationFromURL(next.href));
      },
      replace(value) { this.assign(value); },
      reload() {},
      toString() { return this.href; }
    };
    return location;
  }

  function makeMediaQueryList(query) {
    const target = new EventTarget();
    return {
      matches: false,
      media: String(query),
      onchange: null,
      addEventListener: target.addEventListener.bind(target),
      removeEventListener: target.removeEventListener.bind(target),
      dispatchEvent: target.dispatchEvent.bind(target),
      addListener(callback) { this.addEventListener('change', callback); },
      removeListener(callback) { this.removeEventListener('change', callback); }
    };
  }

  function getRandomValues(array) {
    if (!ArrayBuffer.isView(array) || array instanceof DataView) {
      throw new TypeError('Expected an integer typed array');
    }
    if (array.byteLength > 65536) {
      throw new DOMException('Quota exceeded.', 'QuotaExceededError');
    }
    const bytes = new Uint8Array(array.buffer, array.byteOffset, array.byteLength);
    for (let index = 0; index < bytes.length; index++) {
      bytes[index] = Math.floor(Math.random() * 256);
    }
    return array;
  }

  function randomUUID() {
    const bytes = getRandomValues(new Uint8Array(16));
    bytes[6] = (bytes[6] & 0x0f) | 0x40;
    bytes[8] = (bytes[8] & 0x3f) | 0x80;
    const hex = Array.from(bytes, (byte) => byte.toString(16).padStart(2, '0'));
    return `${hex.slice(0, 4).join('')}-${hex.slice(4, 6).join('')}-${hex.slice(6, 8).join('')}-${hex.slice(8, 10).join('')}-${hex.slice(10, 16).join('')}`;
  }

  function observerStub() {
    this.observe = function() {};
    this.unobserve = function() {};
    this.disconnect = function() {};
    this.takeRecords = function() { return []; };
  }

  function installWptHook() {
    if (global.__pagecore_wpt_installed) return;
    global.__pagecore_wpt_installed = true;
    global.__pagecore_wpt_done = false;
    global.__pagecore_wpt_failures = 0;

    function squash(value) {
      return String(value).replace(/[\u0000-\u001f\u007f-\u009f]+/g, ' ').replace(/\s+/g, ' ').trim();
    }

    global.__pagecore_wpt_oncomplete = function(tests, status) {
      if (global.__pagecore_wpt_done) return;
      const harnessNames = ['OK', 'ERROR', 'TIMEOUT', 'PRECONDITION_FAILED'];
      const subtestNames = ['PASS', 'FAIL', 'TIMEOUT', 'NOTRUN', 'PRECONDITION_FAILED'];
      const harness = harnessNames[status.status] || `UNKNOWN(${status.status})`;
      const counts = { PASS: 0, FAIL: 0, TIMEOUT: 0, NOTRUN: 0, PRECONDITION_FAILED: 0 };
      const lines = [`WPT HARNESS ${harness}${status.message ? ' | ' + squash(status.message) : ''}`];
      const subtests = [];
      for (const test of tests) {
        const state = subtestNames[test.status] || `UNKNOWN(${test.status})`;
        if (counts[state] !== undefined) counts[state]++;
        lines.push(`WPT ${state} ${squash(test.name)}${test.message ? ' | ' + squash(test.message) : ''}`);
        subtests.push({ name: test.name, status: state, message: test.message === undefined ? null : test.message });
      }
      lines.push(`WPT SUMMARY total=${tests.length} pass=${counts.PASS} fail=${counts.FAIL} timeout=${counts.TIMEOUT} notrun=${counts.NOTRUN} precondition_failed=${counts.PRECONDITION_FAILED}`);
      global.__pagecore_wpt_report = lines.join('\n') + '\n';
      global.__pagecore_wpt_json = JSON.stringify({ harness, message: status.message || null, subtests });
      global.__pagecore_wpt_failures = counts.FAIL + counts.TIMEOUT + counts.NOTRUN + (harness === 'OK' || harness === 'PRECONDITION_FAILED' ? 0 : 1);
      global.__pagecore_wpt_done = true;
    };

    Object.defineProperty(global, 'add_completion_callback', {
      configurable: true,
      get() { return undefined; },
      set(callback) {
        delete global.add_completion_callback;
        global.add_completion_callback = callback;
        if (typeof callback === 'function') callback(global.__pagecore_wpt_oncomplete);
      }
    });

    const driver = {
      click(element) {
        return Promise.resolve().then(() => {
          if (element && typeof element.click === 'function') element.click();
        });
      },
      send_keys(element, keys) {
        return Promise.resolve().then(() => {
          if (!element) return;
          if (typeof element.focus === 'function') element.focus();
          for (const key of String(keys)) {
            element.dispatchEvent(new KeyboardEvent('keydown', { bubbles: true, cancelable: true, key }));
            if ('value' in element && key >= ' ') {
              element.value = String(element.value || '') + key;
              element.dispatchEvent(new Event('input', { bubbles: true }));
            }
            element.dispatchEvent(new KeyboardEvent('keyup', { bubbles: true, cancelable: true, key }));
          }
        });
      },
      action_sequence() { return Promise.resolve(); }
    };

    let testDriverInternal = null;
    Object.defineProperty(global, 'test_driver_internal', {
      configurable: true,
      get() { return testDriverInternal; },
      set(value) {
        testDriverInternal = value || {};
        Object.assign(testDriverInternal, driver);
      }
    });
  }

  installWindowIdentity(global);
  global.top = global;
  global.parent = global;
  global.document = document;
  global.DOMException = DOMException;
  global.Window = Window;
  global.EventTarget = EventTarget;
  global.Event = Event;
  global.CustomEvent = CustomEvent;
  global.MessageEvent = MessageEvent;
  global.MessagePort = MessagePort;
  global.MessageChannel = MessageChannel;
  global.UIEvent = UIEvent;
  global.MouseEvent = MouseEvent;
  global.KeyboardEvent = KeyboardEvent;
  global.PointerEvent = PointerEvent;
  global.AbortSignal = AbortSignal;
  global.AbortController = AbortController;
  global.MutationObserver = MutationObserver;
  global.Node = Node;
  global.Text = Text;
  global.Comment = Comment;
  global.Attr = Attr;
  global.Element = Element;
  global.HTMLElement = HTMLElement;
  global.HTMLAnchorElement = HTMLAnchorElement;
  global.HTMLAreaElement = HTMLAreaElement;
  global.HTMLAudioElement = HTMLAudioElement;
  global.HTMLBaseElement = HTMLBaseElement;
  global.HTMLBodyElement = HTMLBodyElement;
  global.HTMLBRElement = HTMLBRElement;
  global.HTMLButtonElement = HTMLButtonElement;
  global.HTMLCanvasElement = HTMLCanvasElement;
  global.HTMLDataElement = HTMLDataElement;
  global.HTMLDataListElement = HTMLDataListElement;
  global.HTMLDetailsElement = HTMLDetailsElement;
  global.HTMLDialogElement = HTMLDialogElement;
  global.HTMLDivElement = HTMLDivElement;
  global.HTMLDListElement = HTMLDListElement;
  global.HTMLEmbedElement = HTMLEmbedElement;
  global.HTMLFieldSetElement = HTMLFieldSetElement;
  global.HTMLFormElement = HTMLFormElement;
  global.HTMLHeadElement = HTMLHeadElement;
  global.HTMLHeadingElement = HTMLHeadingElement;
  global.HTMLHRElement = HTMLHRElement;
  global.HTMLHtmlElement = HTMLHtmlElement;
  global.HTMLIFrameElement = HTMLIFrameElement;
  global.HTMLImageElement = HTMLImageElement;
  global.HTMLInputElement = HTMLInputElement;
  global.HTMLLabelElement = HTMLLabelElement;
  global.HTMLLegendElement = HTMLLegendElement;
  global.HTMLLIElement = HTMLLIElement;
  global.HTMLLinkElement = HTMLLinkElement;
  global.HTMLMapElement = HTMLMapElement;
  global.HTMLMediaElement = HTMLMediaElement;
  global.HTMLMetaElement = HTMLMetaElement;
  global.HTMLMeterElement = HTMLMeterElement;
  global.HTMLModElement = HTMLModElement;
  global.HTMLObjectElement = HTMLObjectElement;
  global.HTMLOListElement = HTMLOListElement;
  global.HTMLOptGroupElement = HTMLOptGroupElement;
  global.HTMLOptionElement = HTMLOptionElement;
  global.HTMLOutputElement = HTMLOutputElement;
  global.HTMLParagraphElement = HTMLParagraphElement;
  global.HTMLParamElement = HTMLParamElement;
  global.HTMLPictureElement = HTMLPictureElement;
  global.HTMLPreElement = HTMLPreElement;
  global.HTMLProgressElement = HTMLProgressElement;
  global.HTMLQuoteElement = HTMLQuoteElement;
  global.HTMLScriptElement = HTMLScriptElement;
  global.HTMLSelectElement = HTMLSelectElement;
  global.HTMLSlotElement = HTMLSlotElement;
  global.HTMLSourceElement = HTMLSourceElement;
  global.HTMLSpanElement = HTMLSpanElement;
  global.HTMLStyleElement = HTMLStyleElement;
  global.HTMLTableCaptionElement = HTMLTableCaptionElement;
  global.HTMLTableCellElement = HTMLTableCellElement;
  global.HTMLTableColElement = HTMLTableColElement;
  global.HTMLTableElement = HTMLTableElement;
  global.HTMLTableRowElement = HTMLTableRowElement;
  global.HTMLTableSectionElement = HTMLTableSectionElement;
  global.HTMLTemplateElement = HTMLTemplateElement;
  global.HTMLTextAreaElement = HTMLTextAreaElement;
  global.HTMLTimeElement = HTMLTimeElement;
  global.HTMLTitleElement = HTMLTitleElement;
  global.HTMLTrackElement = HTMLTrackElement;
  global.HTMLUListElement = HTMLUListElement;
  global.HTMLUnknownElement = HTMLUnknownElement;
  global.HTMLVideoElement = HTMLVideoElement;
  global.SVGElement = SVGElement;
  global.SVGGraphicsElement = SVGGraphicsElement;
  global.SVGSVGElement = SVGSVGElement;
  global.SVGPathElement = SVGPathElement;
  global.SVGCircleElement = SVGCircleElement;
  global.SVGEllipseElement = SVGEllipseElement;
  global.SVGLineElement = SVGLineElement;
  global.SVGRectElement = SVGRectElement;
  global.SVGPolygonElement = SVGPolygonElement;
  global.SVGPolylineElement = SVGPolylineElement;
  global.SVGGElement = SVGGElement;
  global.SVGDefsElement = SVGDefsElement;
  global.SVGUseElement = SVGUseElement;
  global.SVGSymbolElement = SVGSymbolElement;
  global.SVGTitleElement = SVGTitleElement;
  global.SVGDescElement = SVGDescElement;
  global.Document = Document;
  global.DocumentFragment = DocumentFragment;
  global.ShadowRoot = ShadowRoot;
  global.DOMImplementation = DOMImplementation;
  global.ElementInternals = ElementInternals;
  global.DOMTokenList = DOMTokenList;
  global.Range = Range;
  global.Selection = Selection;
  global.DOMRectReadOnly = DOMRectReadOnly;
  global.DOMRect = DOMRect;
  global.URL = URL;
  global.URLSearchParams = URLSearchParams;
  global.TextEncoder = TextEncoder;
  global.TextDecoder = TextDecoder;
  global.Headers = Headers;
  global.Blob = Blob;
  global.File = File;
  global.Request = Request;
  global.Response = Response;
  global.XMLHttpRequest = XMLHttpRequest;
  global.Storage = Storage;
  global.CSSRule = CSSRule;
  global.CSSStyleRule = CSSStyleRule;
  global.CSSMediaRule = CSSMediaRule;
  global.CSSStyleSheet = CSSStyleSheet;
  global.CSSStyleDeclaration = CSSStyleDeclaration;
  global.XMLSerializer = XMLSerializer;
  global.DOMParser = DOMParser;
  global.NodeFilter = NodeFilter;
  customElementsRegistry = new CustomElementRegistry();
  global.CustomElementRegistry = CustomElementRegistry;
  global.customElements = customElementsRegistry;
  global.navigator = {
    userAgent: host.userAgent || 'PageCore/0.1',
    language: 'en-US',
    languages: ['en-US', 'en'],
    onLine: true,
    hardwareConcurrency: 1,
    javaEnabled: () => false,
    mediaDevices: {
      getSupportedConstraints: () => ({})
    }
  };
  global.location = locationFromURL(host.baseURL || '');
  global.history = {
    length: 1,
    state: null,
    pushState(state, _title, url = undefined) {
      this.state = state;
      if (url !== undefined) global.location.assign(url);
    },
    replaceState(state, _title, url = undefined) {
      this.state = state;
      if (url !== undefined) global.location.replace(url);
    },
    go() {},
    back() {},
    forward() {}
  };
  global.localStorage = new Storage();
  global.sessionStorage = new Storage();
  global.crypto = {
    getRandomValues,
    randomUUID
  };
  global.innerWidth = 1024;
  global.innerHeight = 768;
  global.outerWidth = 1024;
  global.outerHeight = 768;
  global.devicePixelRatio = 1;
  global.screen = {
    width: global.innerWidth,
    height: global.innerHeight,
    availWidth: global.innerWidth,
    availHeight: global.innerHeight,
    colorDepth: 24,
    pixelDepth: 24,
    orientation: {
      type: 'landscape-primary',
      angle: 0
    }
  };
  global.screenX = 0;
  global.screenY = 0;
  global.scrollX = 0;
  global.scrollY = 0;
  global.pageXOffset = 0;
  global.pageYOffset = 0;
  global.scrollTo = () => {};
  global.scrollBy = () => {};
  global.scroll = () => {};
  global.open = () => null;
  global.close = () => {};
  global.postMessage = (message) => {
    global.dispatchEvent(new MessageEvent('message', { data: message, origin: global.location.origin, source: global }));
  };

  function consoleArgument(value) {
    if (value && typeof value === 'object') {
      try {
        if (typeof value.stack === 'string' && value.stack) return value.stack;
        if (typeof value.name === 'string' && typeof value.message === 'string') return `${value.name}: ${value.message}`;
      } catch (_error) {
        return value;
      }
    }
    return value;
  }

  global.console = {
    log: (...args) => host.log('log', ...args.map(consoleArgument)),
    info: (...args) => host.log('info', ...args.map(consoleArgument)),
    warn: (...args) => host.log('warn', ...args.map(consoleArgument)),
    error: (...args) => host.log('error', ...args.map(consoleArgument)),
    debug: (...args) => host.log('debug', ...args.map(consoleArgument))
  };

  global.setTimeout = (callback, delay = 0, ...args) => {
    const id = nextTimerId++;
    const timeout = normalizeDelay(delay);
    timers.set(id, { callback, delay: timeout, due: timerNow + timeout, args, interval: false });
    return id;
  };
  global.clearTimeout = (id) => timers.delete(id);
  global.setInterval = (callback, delay = 0, ...args) => {
    const id = nextTimerId++;
    const timeout = normalizeDelay(delay);
    timers.set(id, { callback, delay: timeout, due: timerNow + timeout, args, interval: true });
    return id;
  };
  global.clearInterval = (id) => timers.delete(id);
  global.requestAnimationFrame = (callback) => global.setTimeout(() => callback(timerNow), 16);
  global.cancelAnimationFrame = global.clearTimeout;
  global.queueMicrotask = (callback) => Promise.resolve().then(callback);
  global.requestIdleCallback = (callback) => global.setTimeout(() => callback({ didTimeout: false, timeRemaining: () => 0 }), 0);
  global.cancelIdleCallback = global.clearTimeout;
  global.getComputedStyle = (element) => computedStyleFor(element);
  global.matchMedia = makeMediaQueryList;
  global.IntersectionObserver = observerStub;
  global.ResizeObserver = observerStub;
  global.PerformanceObserver = observerStub;
  global.performance = {
    now: () => timerNow,
    timeOrigin: Date.now(),
    mark() {},
    measure() {},
    getEntriesByName: () => [],
    getEntriesByType: () => []
  };
  global.addEventListener = (...args) => EventTarget.prototype.addEventListener.apply(global, args);
  global.removeEventListener = (...args) => EventTarget.prototype.removeEventListener.apply(global, args);
  global.dispatchEvent = (...args) => EventTarget.prototype.dispatchEvent.apply(global, args);
  global.Image = function(width = undefined, height = undefined) {
    const image = document.createElement('img');
    if (width !== undefined) image.width = Number(width);
    if (height !== undefined) image.height = Number(height);
    image.complete = false;
    image.decode = () => image.getAttribute('src') ? Promise.resolve() : Promise.reject(new DOMException('The source image cannot be decoded.', 'EncodingError'));
    return image;
  };
  global.fetch = (input, init = {}) => Promise.resolve().then(() => {
    const request = new Request(input, init);
    const loaded = loadHostResource(request.url, 'other');
    return new Response(loaded.body || '', {
      status: Number(loaded.status || 200),
      headers: responseHeadersFromHost(loaded),
      url: loaded.url || request.url
    });
  });
  global.__pagecore_install_wpt_hook = installWptHook;
  installWptHook();
  global.__pagecore_fireDOMContentLoaded = () => {
    if (documentReadyState === 'loading') setDocumentReadyState('interactive');
    document.dispatchEvent(new Event('DOMContentLoaded', { bubbles: true }));
    const handler = document.onreadystatechange;
    if (typeof handler === 'function') handler.call(document, new Event('readystatechange'));
  };
  global.__pagecore_fireLoad = () => {
    setDocumentReadyState('complete');
    const readystatechange = new Event('readystatechange');
    document.dispatchEvent(readystatechange);
    const readyHandler = document.onreadystatechange;
    if (typeof readyHandler === 'function') readyHandler.call(document, readystatechange);
    global.dispatchEvent(new Event('load'));
  };

  for (const [name, value] of Object.entries({
    ELEMENT_NODE: 1,
    ATTRIBUTE_NODE: 2,
    TEXT_NODE: 3,
    CDATA_SECTION_NODE: 4,
    PROCESSING_INSTRUCTION_NODE: 7,
    COMMENT_NODE: 8,
    DOCUMENT_NODE: 9,
    DOCUMENT_TYPE_NODE: 10,
    DOCUMENT_FRAGMENT_NODE: 11
  })) {
    defineValue(Node, name, value, true);
    defineValue(Node.prototype, name, value, true);
  }

  global.__pagecore_run_timers = (advanceMs = 0) => {
    const deadline = timerNow + normalizeDelay(advanceMs);
    let ran = 0;
    const maxCallbacks = 250;

    for (;;) {
      let nextDue = Infinity;
      for (const timer of timers.values()) {
        if (timer.due < nextDue) nextDue = timer.due;
      }

      if (nextDue === Infinity || nextDue > deadline) {
        timerNow = deadline;
        return ran;
      }

      timerNow = nextDue;
      const due = [...timers.entries()]
        .filter((entry) => entry[1].due <= timerNow)
        .sort((left, right) => left[1].due - right[1].due || left[0] - right[0]);

      for (const [id, timer] of due) {
        if (!timers.has(id)) continue;
        if (timer.interval) {
          timer.due = timerNow + Math.max(1, timer.delay);
        } else {
          timers.delete(id);
        }
        callTimerCallback(timer);
        ran++;
        if (ran >= maxCallbacks) return 0;
      }
    }
  };
})(globalThis);
