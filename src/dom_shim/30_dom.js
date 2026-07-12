(function(root, factory) {
  const definition = factory();
  if (typeof module !== 'undefined' && module.exports) {
    module.exports = definition;
  } else if (root && typeof root.__pagecore_dom_shim_define === 'function') {
    root.__pagecore_dom_shim_define(definition);
  } else if (root) {
    root.PageCoreDomShimModules = root.PageCoreDomShimModules || {};
    root.PageCoreDomShimModules[definition.name] = definition;
  }
})(globalThis, function() {
  'use strict';

  return {
    name: 'dom',
    deps: ['core', 'events'],
    install(ctx, api) {
      const { global, bridge, host } = ctx;
      const {
        defineValue,
        defineMethod,
        syncMutationCache,
        afterMutation,
        setDocumentReadyState,
        isNodeWrapper,
        attachNodeId,
        assertNode,
        liveId,
        memo,
        toArray,
        activityBegin,
        activityEnd,
        formatErrorForLog,
        loadHostResource,
        loadHostResourceAsync
      } = api.core;
      const {
        DOMException,
        EventTarget,
        Event,
        CustomEvent,
        MessageEvent,
        UIEvent,
        MouseEvent,
        KeyboardEvent,
        PointerEvent,
        FocusEvent,
        HashChangeEvent,
        installWindowIdentity,
        connectCustomElementTree,
        notifyCustomElementAttributeChanged,
        invokeCustomElementConnected
      } = api.events;

      // document.createEvent() takes a legacy interface name, matched
      // ASCII-case-insensitively, and must throw NotSupportedError for anything
      // else. Only interfaces PageCore actually models are listed: exposing
      // DeviceMotionEvent/TouchEvent here would tell a page that sensors and
      // touch input exist, and docs/browser-api-support.md deliberately prefers a
      // clean absence over a stub that lies to feature detection.
      const legacyEventConstructors = {
        event: Event,
        events: Event,
        htmlevents: Event,
        svgevents: Event,
        customevent: CustomEvent,
        messageevent: MessageEvent,
        uievent: UIEvent,
        uievents: UIEvent,
        mouseevent: MouseEvent,
        mouseevents: MouseEvent,
        keyboardevent: KeyboardEvent,
        pointerevent: PointerEvent,
        focusevent: FocusEvent,
        hashchangeevent: HashChangeEvent
      };

      // ASCII lowercase, not String.prototype.toLowerCase(): the spec folds only
      // A-Z, and WPT checks that "UİEvent" (U+0130) does not fold into "uievent".
      function asciiLowercase(value) {
        return String(value).replace(/[A-Z]/g, (letter) => letter.toLowerCase());
      }
      const wrapperCache = ctx.wrapperCache;
      // Per-node cache of the materialized childNodes/children wrapper lists,
      // keyed (per entry) by the bridge mutation version. Repeated traversal
      // (firstChild/lastChild/siblings/loops) then avoids rebuilding the list and
      // re-crossing the bridge until the DOM actually changes. WeakMap keeps it
      // off the node objects and lets entries be collected with their nodes.
      const traversalCache = new WeakMap();
      let activeElement = null;
      let nextCssRootId = 1;
      const absoluteURLCache = new Map();
      const stylesheetTextCache = new Map();
      const stylesheetCache = new Map();
      let stylesheetListKey = '';
      let stylesheetListCache = null;
      let adoptedStyleSheets = [];
      let cssomVersion = 0;
      let stylesheetListMutationVersion = -1;
      let nextDynamicScriptId = 0;
      const orderedDynamicScripts = [];
      let orderedDynamicScriptRunning = false;

      function layoutMutationVersion() {
        return typeof bridge.layoutMutationVersion === 'function'
          ? bridge.layoutMutationVersion()
          : bridge.mutationVersion();
      }

      function locationFromURL(href) {
        let url = null;
        try {
          url = href ? new global.URL(href) : null;
        } catch (_error) {
          url = null;
        }
        return {
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
            const next = new global.URL(value, this.href || undefined);
            Object.assign(this, locationFromURL(next.href));
          },
          replace(value) { this.assign(value); },
          reload() {},
          toString() { return this.href; }
        };
      }

      function normalizedScriptType(value) {
        const text = String(value || '').trim().toLowerCase();
        const semicolon = text.indexOf(';');
        return semicolon >= 0 ? text.slice(0, semicolon).trim() : text;
      }

      function isClassicScriptType(script) {
        const type = normalizedScriptType(script.getAttribute('type') || '');
        return type === ''
          || type === 'text/javascript'
          || type === 'application/javascript'
          || type === 'application/ecmascript'
          || type === 'text/ecmascript'
          || type === 'application/x-javascript'
          || type === 'text/jscript';
      }

      function isOrderedDynamicScript(script) {
        return Object.prototype.hasOwnProperty.call(script, '__pagecoreScriptForceAsync')
          && script.__pagecoreScriptForceAsync === false
          && !script.hasAttribute('async');
      }

      function markDomCreatedScript(script) {
        if (script && script.localName === 'script'
            && !Object.prototype.hasOwnProperty.call(script, '__pagecoreScriptForceAsync')) {
          defineValue(script, '__pagecoreScriptForceAsync', true);
        }
      }

      function scriptErrorMessage(error) {
        if (error && typeof error === 'object') {
          const formatted = formatErrorForLog(error);
          if (typeof formatted === 'string' && formatted) return formatted;
        }
        try {
          return String(error);
        } catch (_stringifyError) {
          return 'JS exception in dynamic script';
        }
      }

      function reportDynamicScriptError(error) {
        try {
          if (host && typeof host.log === 'function') host.log('error', scriptErrorMessage(error));
        } catch (_reportError) {
        }
      }

      function sourceURLComment(filename) {
        const safe = String(filename || '<dynamic-script>').replace(/[\r\n]/g, '');
        return `\n//# sourceURL=${safe}`;
      }

      function executeDynamicScriptSource(script, code, filename) {
        const previous = document.currentScript;
        try {
          document.__setCurrentScript(script && script.__id ? script.__id : null);
          (0, global.eval)(String(code || '') + sourceURLComment(filename));
          script.dispatchEvent(new Event('load'));
        } catch (error) {
          reportDynamicScriptError(error);
          script.dispatchEvent(new Event('error'));
        } finally {
          document.__setCurrentScript(previous && previous.__id ? previous.__id : null);
        }
      }

      // Starts the fetch immediately (truly async through the host engine);
      // the script executes when the completion task lands. onDone always runs
      // exactly once, after execution or failure.
      function runDynamicExternalScript(script, url, onDone) {
        loadHostResourceAsync(url, 'script').then(
          (loaded) => {
            try {
              executeDynamicScriptSource(script, loaded && loaded.body ? loaded.body : '', (loaded && loaded.url) || url);
            } finally {
              if (onDone) onDone();
            }
          },
          (error) => {
            try {
              reportDynamicScriptError(error);
              script.dispatchEvent(new Event('error'));
            } finally {
              if (onDone) onDone();
            }
          });
      }

      function scheduleTask(callback, kind = 'other') {
        if (typeof global.__pagecore_queue_task === 'function') global.__pagecore_queue_task(callback, kind);
        else if (typeof global.setTimeout === 'function') global.setTimeout(callback, 0);
        else callback();
      }

      function drainOrderedDynamicScripts() {
        if (orderedDynamicScriptRunning) return;
        const next = orderedDynamicScripts.shift();
        if (!next) return;
        orderedDynamicScriptRunning = true;
        runDynamicExternalScript(next.script, next.url, () => {
          orderedDynamicScriptRunning = false;
          activityEnd('dynamic-script');
          drainOrderedDynamicScripts();
        });
      }

      function markScriptStarted(script) {
        if (script && script.localName === 'script') {
          defineValue(script, '__pagecoreScriptStarted', true);
        }
      }

      function markScriptsStartedInSubtree(node) {
        if (!node) return;
        markScriptStarted(node);
        if (node.querySelectorAll) {
          for (const script of node.querySelectorAll('script')) markScriptStarted(script);
        }
      }

      function executeDynamicScript(script) {
        if (!script || script.localName !== 'script' || script.__pagecoreScriptStarted) return;
        if (!script.isConnected || script.hasAttribute('nomodule') || !isClassicScriptType(script)) return;
        markScriptStarted(script);

        const src = script.getAttribute('src');
        if (src) {
          const url = absoluteURL(src);
          activityBegin('dynamic-script');
          if (isOrderedDynamicScript(script)) {
            orderedDynamicScripts.push({ script, url });
            drainOrderedDynamicScripts();
            return;
          }
          runDynamicExternalScript(script, url, () => activityEnd('dynamic-script'));
          return;
        }

        const base = global.location && global.location.href ? global.location.href : (host.baseURL || '');
        const filename = `${base || '<dynamic-script>'}#dynamic-script-${nextDynamicScriptId++}`;
        executeDynamicScriptSource(script, script.textContent || '', filename);
      }

      function executeDynamicScriptsInSubtree(node) {
        if (!node || !node.isConnected) return;
        if (node.localName === 'script') executeDynamicScript(node);
        if (node.querySelectorAll) {
          for (const script of node.querySelectorAll('script')) executeDynamicScript(script);
        }
      }

      function relContainsStylesheet(value) {
        return String(value || '').toLowerCase().split(/\s+/).includes('stylesheet');
      }

      function setInternalValue(target, property, value) {
        Object.defineProperty(target, property, {
          value,
          writable: true,
          configurable: true
        });
      }

      function scheduleElementResourceLoad(element) {
        if (ctx.suppressResourceLoadScheduling) return;
        if (!element || !element.localName) return;
        let kind = '';
        let url = '';

        if (element.localName === 'img') {
          const src = element.getAttribute('src') || '';
          if (!src) return;
          kind = 'image';
          url = absoluteURL(src);
        } else if (element.localName === 'link') {
          if (!element.isConnected || !relContainsStylesheet(element.getAttribute('rel')) || element.disabled) return;
          const href = element.getAttribute('href') || '';
          if (!href) return;
          kind = 'stylesheet';
          url = absoluteURL(href);
        } else {
          return;
        }

        if (!url) return;
        const key = `${kind}\n${url}`;
        if (element.__pagecoreResourceLoadKey === key) return;
        setInternalValue(element, '__pagecoreResourceLoadKey', key);
        if (element.localName === 'img') {
          element.complete = false;
        }

        activityBegin('dom-resource');
        const run = () => {
          try {
            const loaded = loadHostResource(url, kind);
            if (kind === 'stylesheet') {
              stylesheetTextCache.set(url, loaded && loaded.body ? loaded.body : '');
            }
            if (element.localName === 'img') element.complete = true;
            element.dispatchEvent(new Event('load'));
          } catch (_error) {
            if (element.localName === 'img') element.complete = true;
            element.dispatchEvent(new Event('error'));
          } finally {
            activityEnd('dom-resource');
          }
        };
        scheduleTask(run, 'dom-resource');
      }

      function scheduleResourceLoadsInSubtree(node) {
        if (!node) return;
        scheduleElementResourceLoad(node);
        if (node.querySelectorAll) {
          for (const image of node.querySelectorAll('img')) scheduleElementResourceLoad(image);
          for (const link of node.querySelectorAll('link')) scheduleElementResourceLoad(link);
        }
      }

        // NodeList and HTMLCollection used to be plain Arrays, which could not be
        // live and had no stable identity. Both are now facades over a read
        // function that recomputes on every access, so a list handed out before a
        // mutation reflects that mutation afterwards, and `el.childNodes ===
        // el.childNodes` holds because the facade is memoized per node.
        //
        // The read function lives in a WeakMap keyed by the *proxy* rather than in
        // a field or a private slot. Keying by the proxy is what gives the brand
        // check its teeth: `Object.create(list).length` runs the getter with a
        // receiver that is not the proxy, finds nothing in the map, and throws —
        // which is what the spec (and WPT) require. A `#private` field cannot be
        // used here at all, because reading one through a Proxy throws.
        const collectionReaders = new WeakMap();
        const collectionIndex = /^(?:0|[1-9]\d*)$/;

        function collectionItems(collection) {
          const read = collectionReaders.get(collection);
          if (!read) throw new TypeError('Illegal invocation');
          return read();
        }

        class NodeList {
          get length() { return collectionItems(this).length; }

          item(index) {
            const items = collectionItems(this);
            const offset = Number(index) >>> 0;
            return offset < items.length ? items[offset] : null;
          }

          forEach(callback, thisArg) {
            const items = collectionItems(this);
            for (let index = 0; index < items.length; index++) {
              callback.call(thisArg, items[index], index, this);
            }
          }

          keys() { return collectionItems(this).keys(); }
          values() { return collectionItems(this).values(); }
          entries() { return collectionItems(this).entries(); }
          [Symbol.iterator]() { return collectionItems(this)[Symbol.iterator](); }
          get [Symbol.toStringTag]() { return 'NodeList'; }
        }

        class HTMLCollection {
          get length() { return collectionItems(this).length; }

          item(index) {
            const items = collectionItems(this);
            const offset = Number(index) >>> 0;
            return offset < items.length ? items[offset] : null;
          }

          namedItem(name) {
            const key = String(name);
            // The empty string is never a supported property name, so it must not
            // match an element that happens to carry name="" or id="".
            if (key === '') return null;
            for (const element of collectionItems(this)) {
              if (element.getAttribute('id') === key) return element;
            }
            for (const element of collectionItems(this)) {
              if (NAMED_CONTENT_TAGS.has(element.localName) && element.getAttribute('name') === key) return element;
            }
            return null;
          }

          [Symbol.iterator]() { return collectionItems(this)[Symbol.iterator](); }
          get [Symbol.toStringTag]() { return 'HTMLCollection'; }
        }

        // Only these elements expose themselves by their name attribute; for
        // everything else `name` is just an ordinary attribute.
        const NAMED_CONTENT_TAGS = new Set(['a', 'applet', 'area', 'embed', 'form', 'frameset', 'iframe', 'img', 'object']);

        function liveCollection(Interface, read) {
          const proxy = new Proxy(new Interface(), {
            get(target, property, receiver) {
              if (typeof property === 'string') {
                if (collectionIndex.test(property)) {
                  const items = read();
                  const offset = Number(property);
                  return offset < items.length ? items[offset] : undefined;
                }
                // Named access must not shadow length/item/namedItem: HTMLCollection
                // is not [LegacyOverrideBuiltIns], so anything already reachable on
                // the interface wins over an element's id or name.
                if (Interface === HTMLCollection && !(property in target)) {
                  const named = receiver.namedItem(property);
                  if (named) return named;
                }
              }
              return Reflect.get(target, property, receiver);
            },
            has(target, property) {
              if (typeof property === 'string' && collectionIndex.test(property)) {
                return Number(property) < read().length;
              }
              return Reflect.has(target, property);
            },
            ownKeys(target) {
              const items = read();
              const keys = [];
              for (let index = 0; index < items.length; index++) keys.push(String(index));
              for (const key of Reflect.ownKeys(target)) {
                if (!keys.includes(key)) keys.push(key);
              }
              return keys;
            },
            getOwnPropertyDescriptor(target, property) {
              if (typeof property === 'string' && collectionIndex.test(property)) {
                const items = read();
                const offset = Number(property);
                if (offset >= items.length) return undefined;
                return { value: items[offset], writable: false, enumerable: true, configurable: true };
              }
              return Reflect.getOwnPropertyDescriptor(target, property);
            }
          });
          collectionReaders.set(proxy, read);
          return proxy;
        }

        // querySelectorAll returns a *static* NodeList: it is a snapshot, so the
        // read function closes over the array it was built from.
        function staticNodeList(items) {
          const snapshot = [...items];
          return liveCollection(NodeList, () => snapshot);
        }

        class Node extends EventTarget {
          constructor(id) {
            super();
            const nodeId = id === undefined ? ctx.pendingCustomElementNodeId : id;
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
          // Memoized so identity is stable (`el.childNodes === el.childNodes`);
          // liveChildNodeList() is re-read on every access, so the list is live.
          get childNodes() { return memo(this, '__childNodesList', () => liveCollection(NodeList, () => liveChildNodeList(this))); }
          get firstChild() { const nodes = liveChildNodeList(this); return nodes[0] || null; }
          get lastChild() {
            const nodes = liveChildNodeList(this);
            return nodes[nodes.length - 1] || null;
          }
          get previousSibling() {
            const parent = this.parentNode;
            if (!parent) return null;
            // A node parented in a DocumentFragment has no bridge-backed parent
            // (it isn't a Lexbor node), so walk the fragment's own childNodes
            // array instead of liveChildNodeList(), which would call parent._liveId().
            // Shadow-tree nodes are real Lexbor children of the container (see
            // ShadowRoot below), so they always take the liveChildNodeList() path.
            const nodes = this.__fragmentParent ? parent.childNodes : liveChildNodeList(parent);
            const index = nodes.findIndex((node) => node === this || node.__id === this.__id);
            return index > 0 ? nodes[index - 1] : null;
          }
          get nextSibling() {
            const parent = this.parentNode;
            if (!parent) return null;
            const nodes = this.__fragmentParent ? parent.childNodes : liveChildNodeList(parent);
            const index = nodes.findIndex((node) => node === this || node.__id === this.__id);
            return index >= 0 && index + 1 < nodes.length ? nodes[index + 1] : null;
          }
          get isConnected() {
            if (this.__fragmentParent) return Boolean(this.__fragmentParent.isConnected);
            return bridge.isConnected(this.__id);
          }
          get textContent() { return bridge.textContent(this._liveId()); }
          set textContent(value) {
            const text = String(value ?? '');
            const nodeType = this.nodeType;
            if (nodeType === 3 || nodeType === 8) {
              const oldValue = bridge.textContent(this._liveId());
              afterMutation(bridge.setTextContent(this._liveId(), text), {
                type: 'characterData',
                target: this,
                addedNodes: [],
                removedNodes: [],
                previousSibling: null,
                nextSibling: null,
                attributeName: null,
                oldValue
              });
              return;
            }

            // Snapshots, not the live childNodes: a MutationRecord must describe the
            // tree as it was at the mutation, and childNodes now keeps changing
            // underneath it.
            const removedNodes = [...this.childNodes];
            bridge.setTextContent(this._liveId(), text);
            syncMutationCache();
            if (ctx.suppressMutationRecords === 0) {
              ctx.queueMutation({
                type: 'childList',
                target: this,
                addedNodes: text === '' ? [] : [...this.childNodes],
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
              ctx.suppressMutationRecords++;
              try {
                for (const node of addedNodes) this.appendChild(node);
              } finally {
                ctx.suppressMutationRecords--;
              }
              child.childNodes.length = 0;
            ctx.queueMutation({
                type: 'childList',
                target: this,
                addedNodes,
                removedNodes: [],
                previousSibling: null,
                nextSibling: null,
                attributeName: null
              });
              for (const node of addedNodes) {
                scheduleResourceLoadsInSubtree(node);
                installWindowNamedPropertiesFromTree(node);
              }
              return child;
            }
            if (child && child.__fragmentParent) child.__fragmentParent.removeChild(child);
            const id = bridge.appendChild(this._liveId(), liveId(child));
            const appended = wrapNode(id) || child;
            connectCustomElementTree(appended);
            const result = afterMutation(appended, {
              type: 'childList',
              target: this,
              addedNodes: [child],
              removedNodes: [],
              previousSibling: null,
              nextSibling: null,
              attributeName: null
            });
            executeDynamicScriptsInSubtree(result);
            scheduleResourceLoadsInSubtree(result);
            installWindowNamedPropertiesFromTree(result);
            return result;
          }

          insertBefore(child, referenceChild) {
            if (isDocumentFragment(child)) {
              const addedNodes = [...child.childNodes];
              ctx.suppressMutationRecords++;
              try {
                for (const node of addedNodes) this.insertBefore(node, referenceChild);
              } finally {
                ctx.suppressMutationRecords--;
              }
              child.childNodes.length = 0;
            ctx.queueMutation({
                type: 'childList',
                target: this,
                addedNodes,
                removedNodes: [],
                previousSibling: null,
                nextSibling: referenceChild || null,
                attributeName: null
              });
              for (const node of addedNodes) {
                scheduleResourceLoadsInSubtree(node);
                installWindowNamedPropertiesFromTree(node);
              }
              return child;
            }
            if (child && child.__fragmentParent) child.__fragmentParent.removeChild(child);
            const referenceId = referenceChild == null ? 0 : liveId(referenceChild);
            const id = bridge.insertBefore(this._liveId(), liveId(child), referenceId);
            const inserted = wrapNode(id) || child;
            connectCustomElementTree(inserted);
            const result = afterMutation(inserted, {
              type: 'childList',
              target: this,
              addedNodes: [child],
              removedNodes: [],
              previousSibling: null,
              nextSibling: referenceChild || null,
              attributeName: null
            });
            executeDynamicScriptsInSubtree(result);
            scheduleResourceLoadsInSubtree(result);
            installWindowNamedPropertiesFromTree(result);
            return result;
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
            // Detach an incoming node from a DocumentFragment first, as
            // insertBefore/appendChild do; otherwise it stays linked in both the
            // fragment's childNodes and the live tree.
            if (child && child.__fragmentParent) child.__fragmentParent.removeChild(child);
            const id = bridge.replaceChild(this._liveId(), liveId(child), liveId(replacedChild));
            const result = afterMutation(wrapNode(id) || replacedChild, {
              type: 'childList',
              target: this,
              addedNodes: [child],
              removedNodes: [replacedChild],
              previousSibling: null,
              nextSibling: null,
              attributeName: null
            });
            executeDynamicScriptsInSubtree(child);
            scheduleResourceLoadsInSubtree(child);
            installWindowNamedPropertiesFromTree(child);
            return result;
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
            // Fast path: when neither node has a JS-only fragment/shadow
            // parent link, candidate's entire ancestor chain is exactly its
            // real bridge parent chain (Node.appendChild/insertBefore always
            // clear __fragmentParent on real attachment — see below), so
            // bridge.contains() — an O(depth) raw lexbor-pointer walk with no
            // wrapper allocation — gives the same answer as the JS loop
            // beneath it, just without re-wrapping every ancestor into a
            // full Element on each step. That wrapping is what makes
            // contains() pathological in a tight loop over a deep tree (e.g.
            // jQuery's .show()/.hide() over many real-world page elements).
            if (!this.__fragmentParent && !candidate.__fragmentParent
              && bridge.hasNode(this.__id) && bridge.hasNode(candidate.__id)) {
              return bridge.contains(this.__id, candidate.__id);
            }
            for (let node = candidate.parentNode; node; node = node.parentNode) {
              if (node === this) return true;
            }
            if (!bridge.hasNode(this.__id) || !bridge.hasNode(candidate.__id)) return false;
            return bridge.contains(this.__id, candidate.__id);
          }

          // Spec bit values (also mirrored onto Node/Node.prototype in the
          // install step so page scripts can read Node.DOCUMENT_POSITION_*).
          compareDocumentPosition(other) {
            const DISCONNECTED = 1;
            const PRECEDING = 2;
            const FOLLOWING = 4;
            const CONTAINS = 8;
            const CONTAINED_BY = 16;
            const IMPLEMENTATION_SPECIFIC = 32;
            if (other == null || !isNodeWrapper(other)) {
              return DISCONNECTED | IMPLEMENTATION_SPECIFIC | PRECEDING;
            }
            if (this === other || this.__id === other.__id) return 0;

            const chainToRoot = (node) => {
              const chain = [];
              for (let current = node; current; current = current.parentNode) chain.push(current);
              return chain;
            };
            const selfChain = chainToRoot(this);
            const otherChain = chainToRoot(other);
            const selfRoot = selfChain[selfChain.length - 1];
            const otherRoot = otherChain[otherChain.length - 1];
            if (!selfRoot || !otherRoot || selfRoot.__id !== otherRoot.__id) {
              // Disconnected trees: pick a consistent-but-arbitrary order.
              const following = this.__id < other.__id ? FOLLOWING : PRECEDING;
              return DISCONNECTED | IMPLEMENTATION_SPECIFIC | following;
            }
            if (this.contains(other)) return CONTAINED_BY | FOLLOWING;
            if (other.contains(this)) return CONTAINS | PRECEDING;

            // Walk both self-inclusive chains from the shared root down until
            // they diverge; the diverging children decide document order.
            const fromRootSelf = selfChain.slice().reverse();
            const fromRootOther = otherChain.slice().reverse();
            let depth = 0;
            while (depth < fromRootSelf.length && depth < fromRootOther.length
              && fromRootSelf[depth].__id === fromRootOther[depth].__id) depth++;
            const commonAncestor = fromRootSelf[depth - 1];
            const branchSelf = fromRootSelf[depth];
            const branchOther = fromRootOther[depth];
            if (!commonAncestor || !branchSelf || !branchOther) return FOLLOWING;
            const siblings = [...commonAncestor.childNodes];
            const indexSelf = siblings.findIndex((node) => node.__id === branchSelf.__id);
            const indexOther = siblings.findIndex((node) => node.__id === branchOther.__id);
            return indexSelf < indexOther ? FOLLOWING : PRECEDING;
          }

          getRootNode(options = {}) {
            let root = this;
            while (root.parentNode) root = root.parentNode;
            if (options && options.composed && root instanceof ShadowRoot) return root.host.getRootNode(options);
            // The root of a node attached to the main tree is the document; for a
            // detached subtree it is the topmost ancestor reached (e.g. a bare
            // createElement('div') is its own root), never the document.
            return root instanceof Document ? document : root;
          }

          hasChildNodes() { return this.firstChild !== null; }
          isSameNode(candidate) {
            return isNodeWrapper(candidate) && candidate.__id === this.__id;
          }
          isEqualNode(candidate) { return this.isSameNode(candidate); }
          normalize() {
            const normalizeNode = (node) => {
              let child = node.firstChild;
              while (child) {
                if (child.nodeType === 3) {
                  const next = child.nextSibling;
                  if (child.textContent === '') {
                    const empty = child;
                    child = child.nextSibling;
                    node.removeChild(empty);
                    continue;
                  }
                  if (next && next.nodeType === 3) {
                    child.textContent = child.textContent + next.textContent;
                    node.removeChild(next);
                    continue; // re-examine child against its new next sibling
                  }
                } else if (child.nodeType === 1) {
                  normalizeNode(child);
                }
                child = child.nextSibling;
              }
            };
            normalizeNode(this);
          }
        }

        // WebIDL `unsigned long`, i.e. ToUint32: -1 becomes 4294967295 (which then
        // trips the IndexSizeError bounds check) and -0x100000000 + 2 becomes 2.
        // CharacterData's offsets are specified in terms of this conversion, and
        // WPT tests the wraparound directly.
        function toUnsignedLong(value) {
          return Number(value) >>> 0;
        }

        function requireArguments(args, count, method) {
          if (args.length < count) {
            throw new TypeError(`Failed to execute '${method}' on 'CharacterData': ${count} argument${count === 1 ? '' : 's'} required, but only ${args.length} present.`);
          }
        }

        // Text and Comment both derive from CharacterData, which owns the data
        // mutation API. Comment used to derive from Text, which made
        // `comment instanceof Text` wrongly true.
        class CharacterData extends Node {
          get data() { return this.textContent; }

          // [LegacyNullToEmptyString]: `node.data = null` yields "", whereas the
          // plain DOMString arguments below turn null into "null".
          set data(value) { this.replaceData(0, this.length, value === null ? '' : String(value)); }

          get length() { return this.data.length; }

          substringData(offset, count) {
            requireArguments(arguments, 2, 'substringData');
            const data = this.data;
            offset = toUnsignedLong(offset);
            count = toUnsignedLong(count);
            if (offset > data.length) {
              throw new DOMException('The index is not in the allowed range.', 'IndexSizeError');
            }
            return offset + count > data.length ? data.slice(offset) : data.slice(offset, offset + count);
          }

          appendData(data) {
            requireArguments(arguments, 1, 'appendData');
            this.replaceData(this.length, 0, data);
          }

          insertData(offset, data) {
            requireArguments(arguments, 2, 'insertData');
            this.replaceData(offset, 0, data);
          }

          deleteData(offset, count) {
            requireArguments(arguments, 2, 'deleteData');
            this.replaceData(offset, count, '');
          }

          replaceData(offset, count, data) {
            requireArguments(arguments, 3, 'replaceData');
            const oldData = this.data;
            offset = toUnsignedLong(offset);
            count = toUnsignedLong(count);
            data = String(data);
            if (offset > oldData.length) {
              throw new DOMException('The index is not in the allowed range.', 'IndexSizeError');
            }
            if (offset + count > oldData.length) count = oldData.length - offset;
            this.textContent = oldData.slice(0, offset) + data + oldData.slice(offset + count);
          }
        }

        // materializeNode() constructs wrappers from an existing node id, while page
        // script constructs from data (`new Text("x")`). These cannot be told apart
        // by argument type: `new Text(42)` is a valid page call that must stringify
        // to "42", not adopt node id 42. So the internal path is marked explicitly.
        const INTERNAL_NODE_ID = Symbol('pagecore.internalNodeId');

        // Registering the fresh node in wrapperCache keeps wrapper identity stable,
        // so that once inserted, `parent.lastChild === theConstructedNode`.
        function createCharacterData(node, createNode, data) {
          // `constructor(optional DOMString data = "")`: undefined selects the
          // default "", while null goes through DOMString conversion to "null".
          const id = createNode(data === undefined ? '' : String(data));
          wrapperCache.set(id, node);
          return id;
        }

        class Text extends CharacterData {
          constructor(data, internalId) {
            if (data === INTERNAL_NODE_ID) {
              super(internalId);
              return;
            }
            super();
            attachNodeId(this, createCharacterData(this, bridge.createTextNode, data));
          }

          get wholeText() {
            let start = this;
            while (start.previousSibling && start.previousSibling.nodeType === 3) start = start.previousSibling;
            let text = '';
            for (let node = start; node && node.nodeType === 3; node = node.nextSibling) text += node.data;
            return text;
          }

          splitText(offset) {
            requireArguments(arguments, 1, 'splitText');
            const data = this.data;
            offset = toUnsignedLong(offset);
            if (offset > data.length) {
              throw new DOMException('The index is not in the allowed range.', 'IndexSizeError');
            }
            const tail = document.createTextNode(data.slice(offset));
            const parent = this.parentNode;
            if (parent) parent.insertBefore(tail, this.nextSibling);
            this.textContent = data.slice(0, offset);
            return tail;
          }
        }

        class Comment extends CharacterData {
          constructor(data, internalId) {
            if (data === INTERNAL_NODE_ID) {
              super(internalId);
              return;
            }
            super();
            attachNodeId(this, createCharacterData(this, bridge.createComment, data));
          }
        }

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

        const NodeFilter = Object.freeze({
          FILTER_ACCEPT: 1,
          FILTER_REJECT: 2,
          FILTER_SKIP: 3,
          SHOW_ALL: 0xFFFFFFFF,
          SHOW_ELEMENT: 0x1,
          SHOW_ATTRIBUTE: 0x2,
          SHOW_TEXT: 0x4,
          SHOW_CDATA_SECTION: 0x8,
          SHOW_ENTITY_REFERENCE: 0x10,
          SHOW_ENTITY: 0x20,
          SHOW_PROCESSING_INSTRUCTION: 0x40,
          SHOW_COMMENT: 0x80,
          SHOW_DOCUMENT: 0x100,
          SHOW_DOCUMENT_TYPE: 0x200,
          SHOW_DOCUMENT_FRAGMENT: 0x400,
          SHOW_NOTATION: 0x800
        });

        const nodeTypeShowMask = {
          1: NodeFilter.SHOW_ELEMENT,
          2: NodeFilter.SHOW_ATTRIBUTE,
          3: NodeFilter.SHOW_TEXT,
          4: NodeFilter.SHOW_CDATA_SECTION,
          5: NodeFilter.SHOW_ENTITY_REFERENCE,
          6: NodeFilter.SHOW_ENTITY,
          7: NodeFilter.SHOW_PROCESSING_INSTRUCTION,
          8: NodeFilter.SHOW_COMMENT,
          9: NodeFilter.SHOW_DOCUMENT,
          10: NodeFilter.SHOW_DOCUMENT_TYPE,
          11: NodeFilter.SHOW_DOCUMENT_FRAGMENT,
          12: NodeFilter.SHOW_NOTATION
        };

        function childNodesArray(node) {
          if (!node || !node.childNodes) return [];
          return Array.from(node.childNodes);
        }

        function firstChildNode(node) {
          return childNodesArray(node)[0] || null;
        }

        function lastChildNode(node) {
          const children = childNodesArray(node);
          return children[children.length - 1] || null;
        }

        function nextTreeNode(node, root, skipChildren = false) {
          if (!skipChildren) {
            const child = firstChildNode(node);
            if (child) return child;
          }

          for (let current = node; current && current !== root; current = current.parentNode || null) {
            if (current.nextSibling) return current.nextSibling;
          }
          return null;
        }

        function previousTreeNode(node, root) {
          if (!node || node === root) return null;

          let previous = node.previousSibling;
          if (previous) {
            for (;;) {
              const child = lastChildNode(previous);
              if (!child) return previous;
              previous = child;
            }
          }

          return node.parentNode || null;
        }

        function traversalFilterResult(filter, node) {
          if (!filter) return NodeFilter.FILTER_ACCEPT;
          const callback = typeof filter === 'function' ? filter : filter.acceptNode;
          if (typeof callback !== 'function') return NodeFilter.FILTER_ACCEPT;
          const result = Number(callback.call(filter, node));
          if (result === NodeFilter.FILTER_REJECT || result === NodeFilter.FILTER_SKIP) return result;
          return NodeFilter.FILTER_ACCEPT;
        }

        function traversalAccept(whatToShow, filter, node) {
          const mask = nodeTypeShowMask[node && node.nodeType] || 0;
          if ((whatToShow & mask) === 0) return NodeFilter.FILTER_SKIP;
          return traversalFilterResult(filter, node);
        }

        class TreeWalker {
          constructor(root, whatToShow = NodeFilter.SHOW_ALL, filter = null) {
            if (!root) throw new TypeError('TreeWalker root must be a Node');
            this.root = root;
            this.whatToShow = Number(whatToShow) >>> 0;
            this.filter = filter || null;
            this.currentNode = root;
          }

          _accept(node) {
            return traversalAccept(this.whatToShow, this.filter, node);
          }

          _firstAcceptedInSubtree(node) {
            for (let current = node; current;) {
              const result = this._accept(current);
              if (result === NodeFilter.FILTER_ACCEPT) return current;
              current = nextTreeNode(current, node, result === NodeFilter.FILTER_REJECT);
            }
            return null;
          }

          _lastAcceptedInSubtree(node) {
            let last = null;
            for (let current = node; current;) {
              const result = this._accept(current);
              if (result === NodeFilter.FILTER_ACCEPT) last = current;
              current = nextTreeNode(current, node, result === NodeFilter.FILTER_REJECT);
            }
            return last;
          }

          parentNode() {
            for (let node = this.currentNode && this.currentNode.parentNode; node && node !== this.root; node = node.parentNode) {
              if (this._accept(node) === NodeFilter.FILTER_ACCEPT) {
                this.currentNode = node;
                return node;
              }
            }
            return null;
          }

          firstChild() {
            for (let node = firstChildNode(this.currentNode); node;) {
              const found = this._firstAcceptedInSubtree(node);
              if (found) {
                this.currentNode = found;
                return found;
              }
              node = node.nextSibling;
            }
            return null;
          }

          lastChild() {
            for (let node = lastChildNode(this.currentNode); node;) {
              const found = this._lastAcceptedInSubtree(node);
              if (found) {
                this.currentNode = found;
                return found;
              }
              node = node.previousSibling;
            }
            return null;
          }

          nextSibling() {
            for (let node = this.currentNode && this.currentNode.nextSibling; node; node = node.nextSibling) {
              const found = this._firstAcceptedInSubtree(node);
              if (found) {
                this.currentNode = found;
                return found;
              }
            }
            return null;
          }

          previousSibling() {
            for (let node = this.currentNode && this.currentNode.previousSibling; node; node = node.previousSibling) {
              const found = this._lastAcceptedInSubtree(node);
              if (found) {
                this.currentNode = found;
                return found;
              }
            }
            return null;
          }

          nextNode() {
            for (let node = nextTreeNode(this.currentNode, this.root); node;) {
              const result = this._accept(node);
              if (result === NodeFilter.FILTER_ACCEPT) {
                this.currentNode = node;
                return node;
              }
              node = nextTreeNode(node, this.root, result === NodeFilter.FILTER_REJECT);
            }
            return null;
          }

          previousNode() {
            for (let node = previousTreeNode(this.currentNode, this.root); node && node !== this.root; node = previousTreeNode(node, this.root)) {
              if (this._accept(node) === NodeFilter.FILTER_ACCEPT) {
                this.currentNode = node;
                return node;
              }
            }
            return null;
          }
        }

        class NodeIterator {
          constructor(root, whatToShow = NodeFilter.SHOW_ALL, filter = null) {
            if (!root) throw new TypeError('NodeIterator root must be a Node');
            this.root = root;
            this.whatToShow = Number(whatToShow) >>> 0;
            this.filter = filter || null;
            this.referenceNode = root;
            this.pointerBeforeReferenceNode = true;
          }

          _accept(node) {
            return traversalAccept(this.whatToShow, this.filter, node) === NodeFilter.FILTER_ACCEPT;
          }

          nextNode() {
            let node = this.pointerBeforeReferenceNode
              ? this.referenceNode
              : nextTreeNode(this.referenceNode, this.root);
            for (; node; node = nextTreeNode(node, this.root)) {
              if (this._accept(node)) {
                this.referenceNode = node;
                this.pointerBeforeReferenceNode = false;
                return node;
              }
            }
            return null;
          }

          previousNode() {
            let node = this.pointerBeforeReferenceNode
              ? previousTreeNode(this.referenceNode, this.root)
              : this.referenceNode;
            for (; node; node = previousTreeNode(node, this.root)) {
              if (this._accept(node)) {
                this.referenceNode = node;
                this.pointerBeforeReferenceNode = true;
                return node;
              }
              if (node === this.root) break;
            }
            return null;
          }

          detach() {}
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

          // Fragments are JS-only (no bridge id), so innerHTML round-trips
          // through a throwaway bridge-backed <div> for lexbor parsing, then
          // moves the parsed nodes in. This is what lets web components build
          // their shadow tree via `shadowRoot.innerHTML = template`.
          get innerHTML() {
            let out = '';
            for (const child of this.childNodes) {
              if (child.nodeType === 1) out += child.outerHTML;
              else if (child.nodeType === 8) out += '<!--' + (child.textContent || '') + '-->';
              else out += String(child.textContent || '').replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
            }
            return out;
          }
          set innerHTML(value) {
            for (const child of [...this.childNodes]) this.removeChild(child);
            const markup = String(value ?? '');
            if (markup === '') return;
            const temp = document.createElement('div');
            temp.innerHTML = markup;
            for (const child of [...temp.childNodes]) this.appendChild(child);
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

        // ShadowRoot is declared after Element (below) since it extends it —
        // see the class for why. Forward references to the name here (in
        // isConnected/getRootNode/resolveOffsetParent) only run once the whole
        // module has finished evaluating, so the later declaration is fine.

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

        const validityFlagNames = [
          'valueMissing',
          'typeMismatch',
          'patternMismatch',
          'tooLong',
          'tooShort',
          'rangeUnderflow',
          'rangeOverflow',
          'stepMismatch',
          'badInput',
          'customError'
        ];

        class ValidityState {
          constructor(flags = {}) {
            for (const name of validityFlagNames) {
              defineValue(this, name, Boolean(flags[name]), true);
            }
          }

          get valid() {
            return !validityFlagNames.some((name) => this[name]);
          }
        }

        function formOwner(element) {
          const id = element.getAttribute && element.getAttribute('form');
          if (id) {
            const owned = document.getElementById(id);
            if (owned && owned.localName === 'form') return owned;
          }
          for (let node = element.parentElement; node; node = node.parentElement) {
            if (node.localName === 'form') return node;
          }
          return null;
        }

        function normalizedInputType(element) {
          return String(element.getAttribute('type') || 'text').toLowerCase();
        }

        function validationValue(element) {
          if (element.localName === 'select') return element.value || '';
          if (element.localName === 'textarea') return element.value || '';
          if (element.localName === 'input') {
            const type = normalizedInputType(element);
            if (type === 'checkbox' || type === 'radio') return element.checked ? (element.getAttribute('value') || 'on') : '';
            return element.value || '';
          }
          return element.getAttribute('value') || '';
        }

        function isValidationCandidate(element) {
          if (!element || typeof element.localName !== 'string') return false;
          if (element.hasAttribute && element.hasAttribute('disabled')) return false;
          if (element.localName === 'select' || element.localName === 'textarea') return true;
          if (element.localName !== 'input') return false;
          return !['hidden', 'button', 'submit', 'reset', 'image'].includes(normalizedInputType(element));
        }

        function radioGroupRequiredMissing(element) {
          const name = element.getAttribute('name') || '';
          const form = formOwner(element);
          const root = form || document;
          const selector = name
            ? `input[type="radio"][name="${String(name).replace(/"/g, '\\"')}"]`
            : 'input[type="radio"]';
          const radios = root.querySelectorAll ? root.querySelectorAll(selector) : [element];
          for (const radio of radios) {
            if (radio.checked) return false;
          }
          return true;
        }

        function numericAttribute(element, name) {
          const raw = element.getAttribute(name);
          if (raw == null || raw === '') return null;
          const value = Number(raw);
          return Number.isFinite(value) ? value : null;
        }

        function validationFlags(element) {
          const flags = Object.create(null);
          for (const name of validityFlagNames) flags[name] = false;
          if (!isValidationCandidate(element)) return flags;

          const value = validationValue(element);
          const type = element.localName === 'input' ? normalizedInputType(element) : element.localName;

          if (element.hasAttribute('required')) {
            if (type === 'checkbox') flags.valueMissing = !element.checked;
            else if (type === 'radio') flags.valueMissing = radioGroupRequiredMissing(element);
            else flags.valueMissing = value === '';
          }

          if (value !== '') {
            if (type === 'email') {
              flags.typeMismatch = !/^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(value);
            } else if (type === 'url') {
              try {
                const URLConstructor = global.URL || globalThis.URL;
                if (!URLConstructor) throw new TypeError('URL is not available');
                new URLConstructor(value, global.location && global.location.href ? global.location.href : undefined);
              } catch (_error) {
                flags.typeMismatch = true;
              }
            }

            const pattern = element.getAttribute('pattern');
            if (pattern != null) {
              try {
                flags.patternMismatch = !(new RegExp(`^(?:${pattern})$`)).test(value);
              } catch (_error) {
                flags.patternMismatch = false;
              }
            }

            const minLength = numericAttribute(element, 'minlength');
            const maxLength = numericAttribute(element, 'maxlength');
            if (minLength != null && value.length < minLength) flags.tooShort = true;
            if (maxLength != null && value.length > maxLength) flags.tooLong = true;

            if (type === 'number' || type === 'range') {
              const number = Number(value);
              if (!Number.isFinite(number)) {
                flags.badInput = true;
              } else {
                const min = numericAttribute(element, 'min');
                const max = numericAttribute(element, 'max');
                if (min != null && number < min) flags.rangeUnderflow = true;
                if (max != null && number > max) flags.rangeOverflow = true;

                const stepAttr = element.getAttribute('step');
                if (stepAttr !== 'any') {
                  const step = stepAttr == null || stepAttr === '' ? 1 : Number(stepAttr);
                  if (Number.isFinite(step) && step > 0) {
                    const base = min != null ? min : 0;
                    const remainder = Math.abs((number - base) % step);
                    if (remainder > 1e-9 && Math.abs(remainder - step) > 1e-9) flags.stepMismatch = true;
                  }
                }
              }
            }
          }

          flags.customError = Boolean(element.__customValidityMessage);
          return flags;
        }

        function validationMessageFor(element) {
          const validity = element.validity;
          if (validity.valid) return '';
          if (validity.customError) return element.__customValidityMessage || '';
          if (validity.valueMissing) return 'Please fill out this field.';
          if (validity.typeMismatch) return 'Please enter a valid value.';
          if (validity.patternMismatch) return 'Please match the requested format.';
          if (validity.tooShort) return 'Please lengthen this text.';
          if (validity.tooLong) return 'Please shorten this text.';
          if (validity.rangeUnderflow) return 'Value must be greater than or equal to the minimum.';
          if (validity.rangeOverflow) return 'Value must be less than or equal to the maximum.';
          if (validity.stepMismatch) return 'Please enter a valid value.';
          if (validity.badInput) return 'Please enter a number.';
          return 'Please enter a valid value.';
        }

        function listedFormControls(root) {
          if (!root || typeof root.querySelectorAll !== 'function') return [];
          // An Array, not the NodeList: form.elements decorates this with
          // item/namedItem and its callers use Array methods on it.
          return [...root.querySelectorAll('button,input,select,textarea')];
        }

        function isDocumentFragment(value) {
          return value instanceof DocumentFragment;
        }

        // Internal bookkeeping attributes attach_shadow_root() puts on real
        // Lexbor nodes (see pagecore/dom.hpp) so litehtml and query selectors
        // can hide shadow structure without a JS-visible attribute existing.
        // The JS attribute-read surface must hide them too.
        function isShadowMarkerAttributeName(name) {
          return name === 'data-pc-shadow-root' || name === 'data-pc-shadow-host';
        }

        function parseHTMLFragment(html) {
          const container = document.createElement('div');
          container.innerHTML = String(html ?? '');
          const nodes = [...container.childNodes];
          for (const node of nodes) markScriptsStartedInSubtree(node);
          return nodes;
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
            return bridge.attributes(this.element._liveId())
              .filter((attr) => !isShadowMarkerAttributeName(attr.name))
              .map((attr) => new Attr(this.element, attr.name));
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

        function indexDeclarations(declarations) {
          const values = Object.create(null);
          const priorities = Object.create(null);
          for (const declaration of declarations) {
            values[declaration.name] = declaration.value;
            priorities[declaration.name] = declaration.priority;
          }
          return { values, priorities };
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
              const { values, priorities } = indexDeclarations(declarations);
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
            const text = serializeDeclarations(declarations);
            this._writeCssText(text);
            // setProperty()/removeProperty() already know exactly what they
            // just wrote, so seed the snapshot cache directly instead of
            // invalidating it — this turns a sequence of N setProperty()
            // calls on the same element from N parseDeclarations() reparses
            // (each call's pre-write _declarations() previously had to
            // reparse, since the prior call's _invalidate() had cleared the
            // cache) into effectively one, which matters in a tight
            // read-modify-write loop like jQuery's .css(). Verify the write
            // landed as expected first — a synchronous
            // attributeChangedCallback/MutationObserver could have mutated
            // the same attribute again — and fall back to invalidating so
            // the next _snapshot() reparses from the real current value.
            if (String(this._readCssText() || '') === text) {
              const { values, priorities } = indexDeclarations(declarations);
              this._cachedCssText = text;
              this._cachedDeclarations = declarations;
              this._cachedValues = values;
              this._cachedPriorities = priorities;
            } else {
              this._invalidate();
            }
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

        // The dataset facade is a Proxy, so its prototype comes from the proxy
        // target: creating the target from DOMStringMap.prototype is what makes
        // `element.dataset instanceof DOMStringMap` true.
        class DOMStringMap {}

        function datasetFor(element) {
          return new Proxy(Object.create(DOMStringMap.prototype), {
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
            cssomVersion++;
            if (this.parentStyleSheet) this.parentStyleSheet._syncOwnerText();
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

        function computedDisplayForVersion(element, version) {
          return computedStylePropertyForVersion(element, 'display', version);
        }

        function computedDisplay(element) {
          return computedDisplayForVersion(element, layoutMutationVersion());
        }

        const windowNamedPropertyGetterMarker = Symbol('pagecoreWindowNamedPropertyGetter');

        function isNamedWindowElement(element) {
          return element instanceof Element
            && ['embed', 'form', 'img', 'object'].includes(element.localName);
        }

        function windowNamedPropertyNamesForElement(element) {
          if (!(element instanceof Element)) return [];
          const names = [];
          const id = element.getAttribute('id');
          if (id) names.push(id);
          if (isNamedWindowElement(element)) {
            const name = element.getAttribute('name');
            if (name) names.push(name);
          }
          return names;
        }

        function walkSubtree(root, callback) {
          if (!root) return;
          const pending = [root];
          while (pending.length) {
            const node = pending.pop();
            callback(node);
            const children = node && node.childNodes ? node.childNodes : [];
            for (let index = children.length - 1; index >= 0; index--) {
              pending.push(children[index]);
            }
          }
        }

        function namedWindowElementsFromTree(root) {
          const elements = [];
          const seen = new Set();
          const add = (element) => {
            if (!(element instanceof Element) || seen.has(element.__id)) return;
            seen.add(element.__id);
            elements.push(element);
          };

          if (root instanceof Element) add(root);
          if (root && typeof root.querySelectorAll === 'function') {
            try {
              for (const selector of ['[id]', 'embed[name]', 'form[name]', 'img[name]', 'object[name]']) {
                for (const element of root.querySelectorAll(selector)) add(element);
              }
              return elements;
            } catch (_selectorError) {
              elements.length = 0;
              seen.clear();
            }
          }

          walkSubtree(root, (node) => {
            if (windowNamedPropertyNamesForElement(node).length > 0) add(node);
          });
          return elements;
        }

        function firstNamedElementByName(name) {
          let found = null;
          walkSubtree(document.documentElement, (node) => {
            if (found || !isNamedWindowElement(node)) return;
            if (node.getAttribute('name') === name) found = node;
          });
          return found;
        }

        function resolveWindowNamedProperty(name) {
          const byId = document.getElementById(name);
          if (byId) return byId;
          return firstNamedElementByName(name) || undefined;
        }

        function installWindowNamedProperty(name) {
          const key = String(name || '');
          if (!key) return;
          const descriptor = Object.getOwnPropertyDescriptor(global, key);
          if (descriptor && !(descriptor.get && descriptor.get[windowNamedPropertyGetterMarker])) return;

          const getter = function() {
            return resolveWindowNamedProperty(key);
          };
          defineValue(getter, windowNamedPropertyGetterMarker, true);

          Object.defineProperty(global, key, {
            configurable: true,
            enumerable: true,
            get: getter,
            set(value) {
              defineValue(global, key, value, true);
            }
          });
        }

        function installWindowNamedPropertiesForElement(element) {
          for (const name of windowNamedPropertyNamesForElement(element)) {
            installWindowNamedProperty(name);
          }
        }

        function installWindowNamedPropertiesFromTree(root) {
          const previous = ctx.suppressResourceLoadScheduling;
          ctx.suppressResourceLoadScheduling = true;
          try {
            for (const element of namedWindowElementsFromTree(root)) {
              installWindowNamedPropertiesForElement(element);
            }
          } finally {
            ctx.suppressResourceLoadScheduling = previous;
          }
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

        function selectorMatchesElementFast(element, selectorOrTokens, scope) {
          const tokens = Array.isArray(selectorOrTokens) ? selectorOrTokens : selectorTokens(selectorOrTokens);
          if (tokens.length === 0) return false;

          // When a scope element is supplied, the `:scope` pseudo-class matches
          // only the query root (the element qSA was invoked on). Without a scope
          // it stays unmatched, preserving prior behavior for plain matching.
          const compoundMatches = (candidate, compound) =>
            (scope && compound === ':scope')
              ? !!candidate && candidate.__id === scope.__id
              : matchesCompoundSelector(candidate, compound);

          function matchFrom(index, candidate) {
            if (!(candidate instanceof Element)) return false;
            if (!compoundMatches(candidate, tokens[index].selector)) return false;
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

        // Per-element cache of litehtml's cascade result, keyed (per entry) by
        // the bridge layout mutation version — same invalidation pattern as
        // elementGeometryCache/traversalCache below. Without this, every
        // getComputedStyle(el) call started a fresh cache (scoped to that
        // single call's closure), so reading multiple properties off one
        // element — or calling getComputedStyle(el) repeatedly in a
        // read-modify-write loop, as jQuery's .css() does — re-crossed the
        // bridge and rebuilt the full ~30-property cascade every time, even
        // when nothing had changed since the last read.
        const computedStyleValuesCache = new WeakMap();
        function computedStyleEntryForVersion(element, version) {
          if (!(element instanceof Element)) return Object.create(null);
          let entry = computedStyleValuesCache.get(element);
          if (entry === undefined || entry.version !== version) {
            entry = {
              version,
              values: null,
              properties: Object.create(null)
            };
            computedStyleValuesCache.set(element, entry);
          }
          return entry;
        }

        function computedStyleValuesForVersion(element, version) {
          if (!(element instanceof Element)) return Object.create(null);
          const entry = computedStyleEntryForVersion(element, version);
          if (entry.values === null) entry.values = bridge.computedStyle(element.__id);
          return entry.values;
        }

        function computedStyleValuesFor(element) {
          return computedStyleValuesForVersion(element, layoutMutationVersion());
        }

        function computedStylePropertyForVersion(element, name, version) {
          const property = cssPropertyName(name);
          if (!property) return '';
          if (!(element instanceof Element)) return '';
          const entry = computedStyleEntryForVersion(element, version);
          if (entry.values !== null) return entry.values[property] || '';
          if (!Object.prototype.hasOwnProperty.call(entry.properties, property)) {
            entry.properties[property] = typeof bridge.computedStyleProperty === 'function'
              ? bridge.computedStyleProperty(element.__id, property)
              : (computedStyleValuesForVersion(element, version)[property] || '');
          }
          return entry.properties[property] || '';
        }

        function computedStyleFor(element) {
          const styleValues = () => computedStyleValuesFor(element);
          const propertyNames = () => Object.keys(styleValues());
          const propertyValue = (name) => {
            const property = cssPropertyName(name);
            if (!property) return '';
            return computedStylePropertyForVersion(element, property, layoutMutationVersion());
          };
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

        // Per-element cache of litehtml's box-model geometry, keyed (per entry)
        // by the bridge layout mutation version — same invalidation pattern as
        // traversalCache/liveChildNodeList. Returns null for elements that
        // don't participate in layout (display:none, or layout() hasn't run).
        const elementGeometryCache = new WeakMap();
        function elementGeometryForVersion(element, version) {
          if (!(element instanceof Element)) return null;
          let entry = elementGeometryCache.get(element);
          if (entry === undefined || entry.version !== version) {
            entry = { version, geometry: bridge.elementGeometry(element.__id) };
            elementGeometryCache.set(element, entry);
          }
          return entry.geometry;
        }

        function elementGeometry(element) {
          return elementGeometryForVersion(element, layoutMutationVersion());
        }

        function cssPixelValue(value) {
          const match = /^(-?(?:\d+|\d*\.\d+))px$/.exec(String(value || '').trim());
          if (!match) return 0;
          const number = Number(match[1]);
          return Number.isFinite(number) ? number : 0;
        }

        function horizontalPadding(element) {
          const style = computedStyleFor(element);
          return {
            left: cssPixelValue(style.getPropertyValue('padding-left')),
            right: cssPixelValue(style.getPropertyValue('padding-right'))
          };
        }

        function currentViewport() {
          try {
            return bridge.viewport();
          } catch (_viewportError) {
            return { width: 1280, height: 720, deviceScaleFactor: 1 };
          }
        }

        function isDocumentElement(element) {
          return element === document.documentElement;
        }

        const offsetParentCache = new WeakMap();
        function resolveOffsetParent(element, version) {
          if (!(element instanceof HTMLElement) || !isNodeWrapper(element)) return null;
          if (!element.isConnected || element.hidden || computedDisplayForVersion(element, version) === 'none') return null;
          for (let ancestor = element.parentElement; ancestor; ancestor = ancestor.parentElement) {
            const position = computedStylePropertyForVersion(ancestor, 'position', version);
            if (position && position !== 'static') return ancestor;
          }
          const root = element.getRootNode ? element.getRootNode() : null;
          return (root instanceof ShadowRoot ? root.host : null) || document.body || null;
        }

        function offsetParentFor(element, version = layoutMutationVersion()) {
          if (!(element instanceof HTMLElement)) return null;
          let entry = offsetParentCache.get(element);
          if (entry === undefined || entry.version !== version) {
            entry = { version, parent: resolveOffsetParent(element, version) };
            offsetParentCache.set(element, entry);
          }
          return entry.parent;
        }

        const offsetGeometryCache = new WeakMap();
        function offsetGeometry(element) {
          if (!(element instanceof Element)) {
            return { geometry: null, parentGeometry: null };
          }
          const version = layoutMutationVersion();
          let entry = offsetGeometryCache.get(element);
          if (entry === undefined || entry.version !== version) {
            const geometry = elementGeometryForVersion(element, version);
            const parent = geometry ? offsetParentFor(element, version) : null;
            entry = {
              version,
              geometry,
              parentGeometry: parent ? elementGeometryForVersion(parent, version) : null
            };
            offsetGeometryCache.set(element, entry);
          }
          return entry;
        }

        function currentDocumentURL() {
          return global.location && global.location.href ? global.location.href : (host.baseURL || '');
        }

        function getCookieString() {
          if (!host || typeof host.getCookieString !== 'function') return '';
          return String(host.getCookieString(currentDocumentURL()) || '');
        }

        function setCookieString(cookie) {
          if (!host || typeof host.setCookieString !== 'function') return;
          host.setCookieString(currentDocumentURL(), String(cookie ?? ''));
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

        // Lexbor rejects `:scope` at parse time, so we resolve scoped selectors in
        // JS: brute-force every descendant of the query root (plus the root itself)
        // and match each against the selector list with `:scope` bound to the root.
        // Covers descendant/child combinators (`:scope`, `:scope > x`, `:scope x`);
        // sibling combinators and stateful pseudo-classes inside a `:scope` selector
        // are not supported (rare for qSA) and simply won't match.
        function querySelectorAllScoped(root, selector) {
          const scope = root.nodeType === 1 ? root : (root.documentElement || root);
          if (!(scope instanceof Element)) return [];
          const parts = splitSelectorList(String(selector));
          const universe = [scope];
          try {
            for (const node of toArray(bridge.querySelectorAll(scope._liveId(), '*'))) universe.push(node);
          } catch (_error) {}
          const seen = new Set();
          const out = [];
          for (const candidate of universe) {
            if (!(candidate instanceof Element) || seen.has(candidate.__id)) continue;
            for (const part of parts) {
              if (selectorMatchesElementFast(candidate, selectorTokens(part), scope)) {
                seen.add(candidate.__id);
                out.push(candidate);
                break;
              }
            }
          }
          return out;
        }

        function querySelectorAllCompat(root, selector) {
          const text = String(selector);
          if (/:scope\b/i.test(text)) return querySelectorAllScoped(root, text);
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
          if (/:scope\b/i.test(text)) return querySelectorAllScoped(root, text)[0] || null;
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
          get hidden() { return this.hasAttribute('hidden'); }
          set hidden(value) { this.toggleAttribute('hidden', Boolean(value)); }
          get classList() { return memo(this, '__classList', () => new DOMTokenList(this, 'class')); }
          get attributes() { return memo(this, '__attributes', () => namedNodeMap(this)); }
          get dataset() { return memo(this, '__dataset', () => datasetFor(this)); }
          get children() { return memo(this, '__childrenList', () => liveCollection(HTMLCollection, () => liveChildElementList(this))); }
          get firstElementChild() { const children = liveChildElementList(this); return children[0] || null; }
          get lastElementChild() {
            const children = liveChildElementList(this);
            return children[children.length - 1] || null;
          }
          get childElementCount() { return liveChildElementList(this).length; }
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
            markScriptsStartedInSubtree(this);
            scheduleResourceLoadsInSubtree(this);
            installWindowNamedPropertiesFromTree(this);
          }
          get outerHTML() { return bridge.outerHTML(this._liveId()); }
          get style() { return memo(this, '__style', () => styleDeclaration(this)); }
          get shadowRoot() {
            const root = this.__shadowRoot || null;
            return root && root.mode === 'open' ? root : null;
          }
          attachShadow(init = {}) {
            if (this.__shadowRoot) throw new DOMException('Shadow root already attached.', 'NotSupportedError');
            // Creates a real Lexbor element (see attach_shadow_root()) and
            // registers its wrapper before anything else can observe the id, so
            // wrapNode(containerId) always resolves to this exact ShadowRoot
            // instance rather than a plain materialized Element.
            const containerId = bridge.attachShadowRoot(this._liveId());
            const root = new ShadowRoot(containerId, this, init);
            wrapperCache.set(containerId, root);
            defineValue(this, '__shadowRoot', root);
            return root;
          }
          attachInternals() {
            return memo(this, '__elementInternals', () => new ElementInternals(this));
          }

          getAttribute(name) {
            const attributeName = String(name);
            if (isShadowMarkerAttributeName(attributeName)) return null;
            const value = bridge.getAttribute(this._liveId(), attributeName);
            return value === null ? null : value;
          }
          hasAttribute(name) {
            const attributeName = String(name);
            if (isShadowMarkerAttributeName(attributeName)) return false;
            return bridge.hasAttribute(this._liveId(), attributeName);
          }
          getAttributeNames() {
            return bridge.attributes(this._liveId())
              .map((attr) => attr.name)
              .filter((name) => !isShadowMarkerAttributeName(name));
          }
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
              attributeName,
              oldValue
            });
            if (oldValue !== newValue) notifyCustomElementAttributeChanged(this, attributeName, oldValue, newValue);
            if (oldValue !== newValue && ['src', 'href', 'rel', 'disabled'].includes(attributeName.toLowerCase())) {
              scheduleElementResourceLoad(this);
            }
            if (oldValue !== newValue && ['id', 'name'].includes(attributeName.toLowerCase())) {
              installWindowNamedPropertiesForElement(this);
            }
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
              attributeName,
              oldValue
            });
            if (oldValue !== null) notifyCustomElementAttributeChanged(this, attributeName, oldValue, null);
            if (oldValue !== null && ['disabled'].includes(attributeName.toLowerCase())) {
              scheduleElementResourceLoad(this);
            }
            return result;
          }
          // The bridge stores attributes by qualified name, so namespaced
          // variants fold the prefix into the name and reuse the plain paths
          // (mutation records, custom-element/resource hooks). This matches
          // how browsers serialize e.g. xlink:href into HTML markup.
          setAttributeNS(_namespace, qualifiedName, value) {
            return this.setAttribute(qualifiedName, value);
          }
          getAttributeNS(_namespace, localName) {
            const local = String(localName);
            const direct = this.getAttribute(local);
            if (direct !== null) return direct;
            const suffix = ':' + local;
            for (const name of this.getAttributeNames()) {
              if (name.endsWith(suffix)) return this.getAttribute(name);
            }
            return null;
          }
          hasAttributeNS(namespace, localName) {
            return this.getAttributeNS(namespace, localName) !== null;
          }
          removeAttributeNS(_namespace, localName) {
            const local = String(localName);
            if (this.hasAttribute(local)) return this.removeAttribute(local);
            const suffix = ':' + local;
            for (const name of this.getAttributeNames()) {
              if (name.endsWith(suffix)) return this.removeAttribute(name);
            }
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
          // Static per spec: a snapshot, not a live view.
          querySelectorAll(selector) { return staticNodeList(querySelectorAllCompat(this, selector)); }

          // getElementsBy* are live, so they re-run the query on every access.
          getElementsByTagName(tagName) {
            const name = String(tagName);
            return liveCollection(HTMLCollection, () => querySelectorAllCompat(this, name));
          }

          getElementsByClassName(classNames) {
            const selector = String(classNames).trim().split(/\s+/).filter(Boolean).map((name) => `.${name}`).join('');
            return liveCollection(HTMLCollection, () => (selector ? querySelectorAllCompat(this, selector) : []));
          }
          matches(selector) {
            const text = String(selector);
            if (text.includes('.\\:')) return selectorMatchesEscapedClasses(this, text);
            try {
              return splitSelectorList(text).some((part) => {
                const tokens = selectorTokens(part);
                if (!selectorFilterMayMatch(this, selectorRightmostFilter(tokens))) return false;
                return selectorMatchesElementFast(this, tokens, this);
              });
            } catch (_error) {
              const parent = this.parentNode || document;
              return [...parent.querySelectorAll(text)].some((node) => node.__id === this.__id);
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
          getBoundingClientRect() {
            const geometry = elementGeometry(this);
            if (!geometry) return new DOMRect(0, 0, 0, 0);
            return new DOMRect(geometry.borderX, geometry.borderY, geometry.borderWidth, geometry.borderHeight);
          }
          getClientRects() {
            // Known simplification: returns one bounding rect instead of a
            // rect per line-box fragment for inline elements.
            const rect = this.getBoundingClientRect();
            return new DOMRectList(rect.width > 0 || rect.height > 0 ? [rect] : []);
          }
          get offsetWidth() {
            const geometry = elementGeometry(this);
            return geometry ? Math.round(geometry.borderWidth) : 0;
          }
          get offsetHeight() {
            const geometry = elementGeometry(this);
            return geometry ? Math.round(geometry.borderHeight) : 0;
          }
          get offsetTop() {
            const { geometry, parentGeometry } = offsetGeometry(this);
            if (!geometry) return 0;
            return Math.round(geometry.borderY - (parentGeometry ? parentGeometry.borderY : 0));
          }
          get offsetLeft() {
            const { geometry, parentGeometry } = offsetGeometry(this);
            if (!geometry) return 0;
            return Math.round(geometry.borderX - (parentGeometry ? parentGeometry.borderX : 0));
          }
          get clientWidth() {
            if (isDocumentElement(this)) return Math.round(currentViewport().width);
            const geometry = elementGeometry(this);
            if (!geometry) return 0;
            if (this.localName === 'input') {
              const padding = horizontalPadding(this);
              return Math.round(geometry.paddingWidth - padding.left - padding.right);
            }
            return Math.round(geometry.paddingWidth);
          }
          get clientHeight() {
            if (isDocumentElement(this)) return Math.round(currentViewport().height);
            const geometry = elementGeometry(this);
            return geometry ? Math.round(geometry.paddingHeight) : 0;
          }
          get clientTop() {
            if (isDocumentElement(this)) return 0;
            const geometry = elementGeometry(this);
            return geometry ? Math.round(geometry.paddingY - geometry.borderY) : 0;
          }
          get clientLeft() {
            if (isDocumentElement(this)) return 0;
            const geometry = elementGeometry(this);
            if (!geometry) return 0;
            const borderLeft = geometry.paddingX - geometry.borderX;
            if (this.localName === 'input') {
              return Math.round(borderLeft + horizontalPadding(this).left);
            }
            return Math.round(borderLeft);
          }
          // Approximation (no patch to litehtml's protected scroll-size
          // state): equates scroll size with the padding-box client size.
          // Known limitation for overflow:scroll|auto content.
          get scrollWidth() { return this.clientWidth; }
          get scrollHeight() { return this.clientHeight; }
          // No real scroll write-channel from JS back into litehtml layout.
          get scrollTop() { return 0; }
          set scrollTop(_value) {}
          get scrollLeft() { return 0; }
          set scrollLeft(_value) {}
        }

        // A thin wrapper over the real shadow-container element attach_shadow_root()
        // creates: __id is the container's NodeId, so every Element method
        // (childNodes, appendChild, querySelector, innerHTML, ...) operates on
        // real Lexbor content litehtml lays out and paints. Only identity-facing
        // bits (nodeType/nodeName/parentNode) are overridden to read like a
        // #shadow-root rather than the <pc-shadowroot> element it wraps.
        class ShadowRoot extends Element {
          constructor(containerId, host, init = {}) {
            super(containerId);
            this.host = host;
            this.mode = init && init.mode === 'closed' ? 'closed' : 'open';
            this.delegatesFocus = Boolean(init && init.delegatesFocus);
            this._adoptedStyleSheets = [];
          }

          get nodeType() { return 11; }
          get nodeName() { return '#shadow-root'; }
          // Stops parentNode/parentElement/getRootNode ancestor walks at the
          // shadow boundary — light-DOM code walking up from the host never
          // sees into the shadow tree, and shadow content's getRootNode()
          // resolves to this ShadowRoot rather than the outer document.
          get parentNode() { return null; }

          get adoptedStyleSheets() { return this._adoptedStyleSheets.slice(); }
          set adoptedStyleSheets(value) {
            if (!Array.isArray(value)) throw new TypeError('adoptedStyleSheets must be an array');
            for (const sheet of value) {
              if (!(sheet instanceof CSSStyleSheet)) throw new TypeError('adoptedStyleSheets entries must be CSSStyleSheet');
            }
            this._adoptedStyleSheets = value.slice();
            cssomVersion++;
          }
        }

        class HTMLElement extends Element {
          get innerText() { return isNodeWrapper(this) ? this.textContent || '' : ''; }
          set innerText(value) { if (isNodeWrapper(this)) this.textContent = String(value ?? ''); }
          get offsetParent() {
            return offsetParentFor(this);
          }
        }

        // ---- IDL attribute reflection ----------------------------------------
        // HTML exposes content attributes as IDL properties with per-type coercion
        // rules. Only a handful were hand-written (className, hidden), so most were
        // missing entirely. A table keeps them consistent and cheap to extend.

        const MAX_LONG = 2147483647;
        const MIN_LONG = -2147483648;
        const MAX_UNSIGNED_LONG = 4294967295;

        // The HTML "rules for parsing integers": leading ASCII whitespace, an
        // optional sign, then digits — and nothing else is inspected, so "5%" and
        // "+100" and " \t 7" all have well-defined outcomes. Returns null on failure,
        // which callers turn into the attribute's default value.
        function parseHtmlInteger(input) {
          const text = String(input);
          let position = 0;
          while (position < text.length && ' \t\n\f\r'.includes(text[position])) position++;
          let sign = 1;
          if (text[position] === '-') {
            sign = -1;
            position++;
          } else if (text[position] === '+') {
            position++;
          }
          const start = position;
          while (position < text.length && text[position] >= '0' && text[position] <= '9') position++;
          if (position === start) return null;
          const value = sign * Number(text.slice(start, position));
          // "-0" parses to -0, but the reflected value is the integer zero: without
          // this, tabIndex="-0" reads back as -0 and fails assert_equals against 0.
          return value === 0 ? 0 : value;
        }

        function defineReflected(prototype, idlName, descriptor) {
          Object.defineProperty(prototype, idlName, {
            configurable: true,
            enumerable: true,
            get: descriptor.get,
            set: descriptor.set
          });
        }

        function reflectString(prototype, idlName, attributeName) {
          defineReflected(prototype, idlName, {
            get() {
              const value = this.getAttribute(attributeName);
              return value === null ? '' : value;
            },
            set(value) { this.setAttribute(attributeName, String(value)); }
          });
        }

        // Nullable DOMString (the ARIA surface): absent reads back as null, and both
        // null and undefined remove the attribute rather than stringifying.
        function reflectNullableString(prototype, idlName, attributeName) {
          defineReflected(prototype, idlName, {
            get() { return this.getAttribute(attributeName); },
            set(value) {
              if (value === null || value === undefined) this.removeAttribute(attributeName);
              else this.setAttribute(attributeName, String(value));
            }
          });
        }

        function reflectBoolean(prototype, idlName, attributeName) {
          defineReflected(prototype, idlName, {
            get() { return this.hasAttribute(attributeName); },
            set(value) {
              if (value) this.setAttribute(attributeName, '');
              else this.removeAttribute(attributeName);
            }
          });
        }

        function reflectLong(prototype, idlName, attributeName, defaultValue) {
          defineReflected(prototype, idlName, {
            get() {
              const raw = this.getAttribute(attributeName);
              if (raw === null) return defaultValue;
              const parsed = parseHtmlInteger(raw);
              if (parsed === null || parsed > MAX_LONG || parsed < MIN_LONG) return defaultValue;
              return parsed;
            },
            // WebIDL `long` is ToInt32, so 1.5 becomes 1 and NaN becomes 0.
            set(value) { this.setAttribute(attributeName, String(Number(value) | 0)); }
          });
        }

        // Reflected unsigned longs are capped at MAX_LONG, not at the IDL type's
        // 4294967295: HTML requires the parsed value to be "in the range of the IDL
        // attribute's type", and anything above 2147483647 falls back to the default.
        function reflectUnsignedLong(prototype, idlName, attributeName, defaultValue = 0) {
          defineReflected(prototype, idlName, {
            get() {
              const raw = this.getAttribute(attributeName);
              if (raw === null) return defaultValue;
              const parsed = parseHtmlInteger(raw);
              if (parsed === null || parsed < 0 || parsed > MAX_LONG) return defaultValue;
              return parsed;
            },
            set(value) { this.setAttribute(attributeName, String(Number(value) >>> 0)); }
          });
        }

        // "Limited to only non-negative numbers": a negative assignment is an error
        // rather than something to clamp, and an absent/invalid attribute reads back
        // as the default (-1 when the spec gives none).
        function reflectLimitedLong(prototype, idlName, attributeName, defaultValue = -1) {
          defineReflected(prototype, idlName, {
            get() {
              const raw = this.getAttribute(attributeName);
              if (raw === null) return defaultValue;
              const parsed = parseHtmlInteger(raw);
              if (parsed === null || parsed < 0 || parsed > MAX_LONG) return defaultValue;
              return parsed;
            },
            set(value) {
              const number = Number(value) | 0;
              if (number < 0) throw new DOMException('The index is not in the allowed range.', 'IndexSizeError');
              this.setAttribute(attributeName, String(number));
            }
          });
        }

        function reflectLimitedUnsignedLong(prototype, idlName, attributeName, defaultValue) {
          defineReflected(prototype, idlName, {
            get() {
              const raw = this.getAttribute(attributeName);
              if (raw === null) return defaultValue;
              const parsed = parseHtmlInteger(raw);
              // Zero is not allowed either: the spec's "limited to only positive
              // numbers" falls back to the default for it.
              if (parsed === null || parsed <= 0 || parsed > MAX_UNSIGNED_LONG) return defaultValue;
              return parsed;
            },
            set(value) {
              const number = Number(value) >>> 0;
              if (number === 0) throw new DOMException('The index is not in the allowed range.', 'IndexSizeError');
              this.setAttribute(attributeName, String(number));
            }
          });
        }

        // A URL-valued attribute reads back *resolved* against the document base URL,
        // not as the literal attribute text.
        //
        // form.action and formAction carry an extra rule: a missing *or empty* value
        // reads back as the document's own URL, because submitting with no action
        // targets the current page.
        function reflectUrl(prototype, idlName, attributeName, emptyIsDocumentUrl = false) {
          defineReflected(prototype, idlName, {
            get() {
              const raw = this.getAttribute(attributeName);
              if (raw === null || (emptyIsDocumentUrl && raw === '')) {
                return emptyIsDocumentUrl ? document.URL : '';
              }
              const resolved = absoluteURL(raw);
              return resolved || raw;
            },
            set(value) { this.setAttribute(attributeName, String(value)); }
          });
        }

        // An enumerated attribute maps to its canonical keyword ASCII-case-
        // insensitively; anything unrecognized (or absent) falls back to a default.
        function reflectEnum(prototype, idlName, attributeName, keywords, defaultValue = '', invalidValue = defaultValue) {
          const canonical = new Map(keywords.map((keyword) => [asciiLowercase(keyword), keyword]));
          defineReflected(prototype, idlName, {
            get() {
              const raw = this.getAttribute(attributeName);
              if (raw === null) return defaultValue;
              const match = canonical.get(asciiLowercase(raw));
              return match === undefined ? invalidValue : match;
            },
            set(value) { this.setAttribute(attributeName, String(value)); }
          });
        }

        // A nullable enumerated attribute (crossOrigin): absent reads back as null,
        // and assigning null removes the attribute instead of stringifying it.
        function reflectNullableEnum(prototype, idlName, attributeName, keywords, invalidValue) {
          const canonical = new Map(keywords.map((keyword) => [asciiLowercase(keyword), keyword]));
          defineReflected(prototype, idlName, {
            get() {
              const raw = this.getAttribute(attributeName);
              if (raw === null) return null;
              const match = canonical.get(asciiLowercase(raw));
              return match === undefined ? invalidValue : match;
            },
            set(value) {
              if (value === null || value === undefined) this.removeAttribute(attributeName);
              else this.setAttribute(attributeName, String(value));
            }
          });
        }

        // Global attributes, reflected on every HTML element.
        reflectString(HTMLElement.prototype, 'title', 'title');
        reflectString(HTMLElement.prototype, 'lang', 'lang');
        reflectString(HTMLElement.prototype, 'accessKey', 'accesskey');
        reflectString(HTMLElement.prototype, 'nonce', 'nonce');
        reflectBoolean(HTMLElement.prototype, 'autofocus', 'autofocus');
        reflectEnum(HTMLElement.prototype, 'dir', 'dir', ['ltr', 'rtl', 'auto']);
        reflectEnum(HTMLElement.prototype, 'autocapitalize', 'autocapitalize', ['none', 'off', 'sentences', 'on', 'words', 'characters']);
        reflectEnum(HTMLElement.prototype, 'enterKeyHint', 'enterkeyhint', ['enter', 'done', 'go', 'next', 'previous', 'search', 'send']);
        reflectEnum(HTMLElement.prototype, 'inputMode', 'inputmode', ['none', 'text', 'tel', 'url', 'email', 'numeric', 'decimal', 'search']);
        // tabIndex's missing-value default depends on whether the element is
        // focusable, which is mostly a SHOULD; WPT deliberately does not test it.
        reflectLong(HTMLElement.prototype, 'tabIndex', 'tabindex', 0);

        // ARIAMixin lives on Element, not HTMLElement, so SVG and MathML elements
        // reflect it too.
        reflectNullableString(Element.prototype, 'role', 'role');
        // String-valued only. aria-activedescendant reflects an *Element* reference,
        // not a string, so it is deliberately not in this list.
        const ARIA_REFLECTED = [
          'Atomic', 'AutoComplete', 'BrailleLabel', 'BrailleRoleDescription',
          'Busy', 'Checked', 'ColCount', 'ColIndex', 'ColIndexText', 'ColSpan', 'Current', 'Description',
          'Disabled', 'Expanded', 'HasPopup', 'Hidden', 'Invalid', 'KeyShortcuts', 'Label', 'Level',
          'Live', 'Modal', 'MultiLine', 'MultiSelectable', 'Orientation', 'Placeholder', 'PosInSet',
          'Pressed', 'ReadOnly', 'Relevant', 'Required', 'RoleDescription', 'RowCount', 'RowIndex',
          'RowIndexText', 'RowSpan', 'Selected', 'SetSize', 'Sort', 'ValueMax', 'ValueMin', 'ValueNow',
          'ValueText'
        ];
        for (const suffix of ARIA_REFLECTED) {
          reflectNullableString(Element.prototype, `aria${suffix}`, `aria-${asciiLowercase(suffix)}`);
        }
        class HTMLFormControlElement extends HTMLElement {
          get form() { return formOwner(this); }
          get name() { return this.getAttribute('name') || ''; }
          set name(value) { this.setAttribute('name', String(value)); }
          get disabled() { return this.hasAttribute('disabled'); }
          set disabled(value) { this.toggleAttribute('disabled', Boolean(value)); }
          get required() { return this.hasAttribute('required'); }
          set required(value) { this.toggleAttribute('required', Boolean(value)); }
          get willValidate() { return isValidationCandidate(this); }
          get validity() { return new ValidityState(validationFlags(this)); }
          get validationMessage() { return validationMessageFor(this); }
          setCustomValidity(message) {
            const text = String(message ?? '');
            if (text) defineValue(this, '__customValidityMessage', text);
            else if (Object.prototype.hasOwnProperty.call(this, '__customValidityMessage')) {
              delete this.__customValidityMessage;
            }
          }
          checkValidity() {
            if (!this.willValidate || this.validity.valid) return true;
            this.dispatchEvent(new Event('invalid', { cancelable: true }));
            return false;
          }
          reportValidity() { return this.checkValidity(); }
        }
        // Implements the HTMLHyperlinkElementUtils URL decomposition members
        // (protocol/host/pathname/...) shared by <a> and <area>. Libraries such
        // as AngularJS parse URLs by assigning href to an anchor and reading
        // these back; without them the getters return undefined and callers
        // like urlResolve() throw on `pathname.charAt(0)`.
        function resolveHyperlinkURL(element) {
          const attr = element.getAttribute('href');
          if (attr == null) return null;
          const base = global.location && global.location.href
            ? global.location.href
            : (host.baseURL || undefined);
          try {
            return new global.URL(attr, base);
          } catch (_error) {
            return null;
          }
        }
        function mutateHyperlinkURL(element, apply) {
          const url = resolveHyperlinkURL(element);
          if (!url) return;
          apply(url);
          element.setAttribute('href', url.href);
        }
        class HTMLHyperlinkElement extends HTMLElement {
          get href() {
            const url = resolveHyperlinkURL(this);
            return url ? url.href : (this.getAttribute('href') || '');
          }
          set href(value) { this.setAttribute('href', String(value)); }
          get origin() { const url = resolveHyperlinkURL(this); return url ? url.origin : ''; }
          get protocol() { const url = resolveHyperlinkURL(this); return url ? url.protocol : ':'; }
          set protocol(value) { mutateHyperlinkURL(this, (url) => { url.protocol = value; }); }
          get username() { const url = resolveHyperlinkURL(this); return url ? url.username : ''; }
          set username(value) { mutateHyperlinkURL(this, (url) => { url.username = value; }); }
          get password() { const url = resolveHyperlinkURL(this); return url ? url.password : ''; }
          set password(value) { mutateHyperlinkURL(this, (url) => { url.password = value; }); }
          get host() { const url = resolveHyperlinkURL(this); return url ? url.host : ''; }
          set host(value) { mutateHyperlinkURL(this, (url) => { url.host = value; }); }
          get hostname() { const url = resolveHyperlinkURL(this); return url ? url.hostname : ''; }
          set hostname(value) { mutateHyperlinkURL(this, (url) => { url.hostname = value; }); }
          get port() { const url = resolveHyperlinkURL(this); return url ? url.port : ''; }
          set port(value) { mutateHyperlinkURL(this, (url) => { url.port = value; }); }
          get pathname() { const url = resolveHyperlinkURL(this); return url ? url.pathname : ''; }
          set pathname(value) { mutateHyperlinkURL(this, (url) => { url.pathname = value; }); }
          get search() { const url = resolveHyperlinkURL(this); return url ? url.search : ''; }
          set search(value) { mutateHyperlinkURL(this, (url) => { url.search = value; }); }
          get hash() { const url = resolveHyperlinkURL(this); return url ? url.hash : ''; }
          set hash(value) { mutateHyperlinkURL(this, (url) => { url.hash = value; }); }
          toString() { return this.href; }
        }
        class HTMLAnchorElement extends HTMLHyperlinkElement {
          get text() { return this.textContent || ''; }
          set text(value) { this.textContent = String(value ?? ''); }
        }
        class HTMLAreaElement extends HTMLHyperlinkElement {}
        class HTMLMediaElement extends HTMLElement {
          get muted() { return this.hasAttribute('muted'); }
          set muted(value) { this.toggleAttribute('muted', Boolean(value)); }
          get paused() { return !this.__playing; }
          get currentTime() { return Number(this.__currentTime || 0); }
          set currentTime(value) {
            const next = Number(value);
            defineValue(this, '__currentTime', Number.isFinite(next) && next >= 0 ? next : 0);
          }
          canPlayType() { return ''; }
          load() {}
          play() {
            defineValue(this, '__playing', true);
            return Promise.resolve();
          }
          pause() { defineValue(this, '__playing', false); }
          addTextTrack() { return {}; }
        }
        class HTMLAudioElement extends HTMLMediaElement {}
        class HTMLBaseElement extends HTMLElement {}
        class HTMLBodyElement extends HTMLElement {}
        class HTMLBRElement extends HTMLElement {}
        class HTMLButtonElement extends HTMLFormControlElement {
          get type() {
            const type = String(this.getAttribute('type') || 'submit').toLowerCase();
            return ['submit', 'reset', 'button'].includes(type) ? type : 'submit';
          }
          set type(value) { this.setAttribute('type', String(value)); }
          // The value IDL attribute reflects the content attribute, so a
          // <button name=x value=y> submitter contributes x=y to form submission.
          get value() { return this.getAttribute('value') || ''; }
          set value(value) { this.setAttribute('value', String(value)); }
          get willValidate() { return false; }
        }
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
        class HTMLFormElement extends HTMLElement {
          get elements() {
            const controls = listedFormControls(this);
            defineValue(controls, 'item', function(index) { return this[Number(index)] || null; });
            defineValue(controls, 'namedItem', function(name) {
              const key = String(name);
              return this.find((control) => control.name === key || control.id === key) || null;
            });
            return controls;
          }
          get length() { return this.elements.length; }
          checkValidity() {
            let valid = true;
            for (const control of this.elements) {
              if (control.willValidate && !control.checkValidity()) valid = false;
            }
            return valid;
          }
          reportValidity() { return this.checkValidity(); }
          reset() {
            // Clear each control's HTML "dirty" flags so its IDL
            // value/checkedness/selectedness falls back to the corresponding
            // default (defaultValue/defaultChecked/defaultSelected).
            for (const control of this.elements) {
              if (control instanceof HTMLInputElement) {
                control._dirtyValue = false;
                control._dirtyCheckedness = false;
              } else if (control instanceof HTMLTextAreaElement) {
                control._dirtyValue = false;
              } else if (control instanceof HTMLSelectElement) {
                for (const option of control.querySelectorAll('option')) option._dirtySelectedness = false;
              }
            }
            this.dispatchEvent(new Event('reset', { bubbles: true, cancelable: true }));
          }
          submit() {}
          requestSubmit(submitter = null) {
            if (submitter != null && submitter.form !== this) {
              throw new DOMException('Submitter does not belong to this form.', 'NotFoundError');
            }
            if (!this.reportValidity()) return;
            this.dispatchEvent(new Event('submit', { bubbles: true, cancelable: true }));
          }
        }
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
        class HTMLImageElement extends HTMLElement {
          get x() {
            if (computedDisplay(this) === 'none') return 0;
            return Math.round(this.getBoundingClientRect().left);
          }
          get y() {
            if (computedDisplay(this) === 'none') return 0;
            return Math.round(this.getBoundingClientRect().top);
          }
        }
        class HTMLInputElement extends HTMLFormControlElement {
          get type() { return normalizedInputType(this); }
          set type(value) { this.setAttribute('type', String(value)); }
          // The current value is tracked separately from the "value" content
          // attribute (the default value), per the HTML "dirty value flag", so
          // typing does not rewrite the attribute and form.reset() works.
          get value() {
            return this._dirtyValue ? this._value : (this.getAttribute('value') || '');
          }
          set value(value) {
            this._value = String(value);
            this._dirtyValue = true;
          }
          get defaultValue() { return this.getAttribute('value') || ''; }
          set defaultValue(value) { this.setAttribute('value', String(value)); }
          // Checkedness is tracked separately from the "checked" content
          // attribute (the default), per the HTML "dirty checkedness flag", so
          // toggling the control does not rewrite the attribute and reset() works.
          get checked() {
            return this._dirtyCheckedness ? this._checkedness : this.hasAttribute('checked');
          }
          set checked(value) {
            this._checkedness = Boolean(value);
            this._dirtyCheckedness = true;
          }
          get defaultChecked() { return this.hasAttribute('checked'); }
          set defaultChecked(value) { this.toggleAttribute('checked', Boolean(value)); }
          get minLength() { return Number(this.getAttribute('minlength') || -1); }
          set minLength(value) { this.setAttribute('minlength', String(value)); }
          get maxLength() { return Number(this.getAttribute('maxlength') || -1); }
          set maxLength(value) { this.setAttribute('maxlength', String(value)); }
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
        class HTMLMetaElement extends HTMLElement {}
        class HTMLMeterElement extends HTMLElement {}
        class HTMLModElement extends HTMLElement {}
        class HTMLObjectElement extends HTMLElement {}
        class HTMLOListElement extends HTMLElement {}
        class HTMLOptGroupElement extends HTMLElement {}
        class HTMLOptionElement extends HTMLElement {
          get value() { return this.getAttribute('value') || this.textContent || ''; }
          set value(value) { this.setAttribute('value', String(value)); }
          // Selectedness is tracked separately from the "selected" content
          // attribute (the default), per the HTML "dirtiness" flag, so scripting
          // the option does not rewrite the attribute and form.reset() works.
          get selected() {
            return this._dirtySelectedness ? this._selectedness : this.hasAttribute('selected');
          }
          set selected(value) {
            this._selectedness = Boolean(value);
            this._dirtySelectedness = true;
          }
          get defaultSelected() { return this.hasAttribute('selected'); }
          set defaultSelected(value) { this.toggleAttribute('selected', Boolean(value)); }
          get disabled() { return this.hasAttribute('disabled'); }
          set disabled(value) { this.toggleAttribute('disabled', Boolean(value)); }
          get text() { return this.textContent || ''; }
          set text(value) { this.textContent = String(value ?? ''); }
        }
        class HTMLOutputElement extends HTMLElement {}
        class HTMLParagraphElement extends HTMLElement {}
        class HTMLParamElement extends HTMLElement {}
        class HTMLPictureElement extends HTMLElement {}
        class HTMLPreElement extends HTMLElement {}
        class HTMLProgressElement extends HTMLElement {}
        class HTMLQuoteElement extends HTMLElement {}
        class HTMLScriptElement extends HTMLElement {
          get async() { return this.hasAttribute('async') || this.__pagecoreScriptForceAsync === true; }
          set async(value) {
            const enabled = Boolean(value);
            defineValue(this, '__pagecoreScriptForceAsync', enabled);
            this.toggleAttribute('async', enabled);
          }
          get defer() { return this.hasAttribute('defer'); }
          set defer(value) { this.toggleAttribute('defer', Boolean(value)); }
          get text() { return this.textContent || ''; }
          set text(value) { this.textContent = String(value ?? ''); }
        }
        class HTMLSelectElement extends HTMLFormControlElement {
          get options() { return liveCollection(HTMLCollection, () => querySelectorAllCompat(this, 'option')); }
          get value() {
            // Read live selectedness (the IDL flag), not the "selected" content
            // attribute, so a script-set option.selected is reflected here.
            const options = [...this.options];
            const selected = options.find((option) => option.selected) || options[0];
            return selected ? selected.value : '';
          }
          set value(value) {
            const text = String(value);
            for (const option of this.querySelectorAll('option')) {
              option.selected = option.value === text;
            }
          }
          get selectedIndex() {
            return [...this.options].findIndex((option) => option.selected);
          }
          set selectedIndex(value) {
            const index = Number(value);
            [...this.options].forEach((option, optionIndex) => { option.selected = optionIndex === index; });
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
        class HTMLTextAreaElement extends HTMLFormControlElement {
          // The raw value is tracked separately from the child text (the default
          // value), per the HTML "dirty value flag", so setting value does not
          // rewrite the text content and form.reset() restores the default.
          get value() {
            return this._dirtyValue ? this._value : (this.getAttribute('value') || this.textContent || '');
          }
          set value(value) {
            this._value = String(value);
            this._dirtyValue = true;
          }
          get defaultValue() { return this.textContent || ''; }
          set defaultValue(value) { this.textContent = String(value ?? ''); }
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

        // Obsolete elements still have real interfaces in every browser; without
        // them these tags fell back to HTMLUnknownElement and reflected nothing.
        class HTMLMarqueeElement extends HTMLElement {}
        class HTMLFontElement extends HTMLElement {}
        class HTMLDirectoryElement extends HTMLElement {}
        class HTMLFrameElement extends HTMLElement {}
        class HTMLFrameSetElement extends HTMLElement {}

        // Per-element reflected attributes. Applied to the prototypes after the
        // classes exist, which also lets a table entry replace an earlier hand-written
        // accessor (input.minLength/maxLength used plain Number() parsing, which does
        // not follow the HTML integer rules and never applied the -1 default).
        reflectString(HTMLInputElement.prototype, 'accept', 'accept');
        reflectString(HTMLInputElement.prototype, 'alt', 'alt');
        reflectString(HTMLInputElement.prototype, 'autocomplete', 'autocomplete');
        reflectString(HTMLInputElement.prototype, 'dirName', 'dirname');
        reflectString(HTMLInputElement.prototype, 'max', 'max');
        reflectString(HTMLInputElement.prototype, 'min', 'min');
        reflectString(HTMLInputElement.prototype, 'pattern', 'pattern');
        reflectString(HTMLInputElement.prototype, 'placeholder', 'placeholder');
        reflectString(HTMLInputElement.prototype, 'step', 'step');
        reflectString(HTMLInputElement.prototype, 'align', 'align');
        reflectString(HTMLInputElement.prototype, 'useMap', 'usemap');
        reflectString(HTMLInputElement.prototype, 'formTarget', 'formtarget');
        reflectBoolean(HTMLInputElement.prototype, 'multiple', 'multiple');
        reflectBoolean(HTMLInputElement.prototype, 'readOnly', 'readonly');
        reflectBoolean(HTMLInputElement.prototype, 'required', 'required');
        reflectBoolean(HTMLInputElement.prototype, 'formNoValidate', 'formnovalidate');
        reflectUrl(HTMLInputElement.prototype, 'formAction', 'formaction', true);
        reflectUrl(HTMLInputElement.prototype, 'src', 'src');
        reflectUnsignedLong(HTMLInputElement.prototype, 'height', 'height');
        reflectUnsignedLong(HTMLInputElement.prototype, 'width', 'width');
        reflectLimitedLong(HTMLInputElement.prototype, 'maxLength', 'maxlength');
        reflectLimitedLong(HTMLInputElement.prototype, 'minLength', 'minlength');
        reflectLimitedUnsignedLong(HTMLInputElement.prototype, 'size', 'size', 20);
        // formenctype/formmethod have no missing-value default (an absent attribute
        // means "defer to the form"), but they do have an invalid-value default.
        reflectEnum(HTMLInputElement.prototype, 'formEnctype', 'formenctype',
          ['application/x-www-form-urlencoded', 'multipart/form-data', 'text/plain'], '', 'application/x-www-form-urlencoded');
        reflectEnum(HTMLInputElement.prototype, 'formMethod', 'formmethod', ['get', 'post'], '', 'get');

        reflectUrl(HTMLButtonElement.prototype, 'formAction', 'formaction', true);
        reflectUrl(HTMLFormElement.prototype, 'action', 'action', true);
        reflectString(HTMLButtonElement.prototype, 'formTarget', 'formtarget');
        reflectBoolean(HTMLButtonElement.prototype, 'formNoValidate', 'formnovalidate');
        reflectEnum(HTMLButtonElement.prototype, 'formEnctype', 'formenctype',
          ['application/x-www-form-urlencoded', 'multipart/form-data', 'text/plain'], '', 'application/x-www-form-urlencoded');
        reflectEnum(HTMLButtonElement.prototype, 'formMethod', 'formmethod', ['get', 'post', 'dialog'], '', 'get');

        reflectString(HTMLMarqueeElement.prototype, 'bgColor', 'bgcolor');
        reflectString(HTMLMarqueeElement.prototype, 'height', 'height');
        reflectString(HTMLMarqueeElement.prototype, 'width', 'width');
        reflectBoolean(HTMLMarqueeElement.prototype, 'trueSpeed', 'truespeed');
        reflectUnsignedLong(HTMLMarqueeElement.prototype, 'hspace', 'hspace');
        reflectUnsignedLong(HTMLMarqueeElement.prototype, 'vspace', 'vspace');
        reflectUnsignedLong(HTMLMarqueeElement.prototype, 'scrollAmount', 'scrollamount', 6);
        reflectUnsignedLong(HTMLMarqueeElement.prototype, 'scrollDelay', 'scrolldelay', 85);

        reflectString(HTMLFontElement.prototype, 'color', 'color');
        reflectString(HTMLFontElement.prototype, 'face', 'face');
        reflectString(HTMLFontElement.prototype, 'size', 'size');

        reflectBoolean(HTMLDirectoryElement.prototype, 'compact', 'compact');

        reflectString(HTMLFrameElement.prototype, 'name', 'name');
        reflectString(HTMLFrameElement.prototype, 'scrolling', 'scrolling');
        reflectString(HTMLFrameElement.prototype, 'frameBorder', 'frameborder');
        reflectString(HTMLFrameElement.prototype, 'marginHeight', 'marginheight');
        reflectString(HTMLFrameElement.prototype, 'marginWidth', 'marginwidth');
        reflectBoolean(HTMLFrameElement.prototype, 'noResize', 'noresize');
        reflectUrl(HTMLFrameElement.prototype, 'longDesc', 'longdesc');

        reflectString(HTMLFrameSetElement.prototype, 'cols', 'cols');
        reflectString(HTMLFrameSetElement.prototype, 'rows', 'rows');

        // Metadata elements: <link>/<meta>/<base> carry most of what a scraper reads
        // out of <head>, so these matter well beyond the WPT score.
        const REFERRER_POLICIES = ['', 'no-referrer', 'no-referrer-when-downgrade', 'same-origin', 'origin',
          'strict-origin', 'origin-when-cross-origin', 'strict-origin-when-cross-origin', 'unsafe-url'];
        const LINK_DESTINATIONS = ['fetch', 'audio', 'document', 'embed', 'font', 'image', 'manifest', 'object',
          'report', 'script', 'sharedworker', 'style', 'track', 'video', 'worker', 'xslt'];

        for (const name of ['media', 'integrity', 'hreflang', 'charset', 'rev', 'target', 'nonce']) {
          reflectString(HTMLLinkElement.prototype, name, name);
        }
        reflectEnum(HTMLLinkElement.prototype, 'as', 'as', LINK_DESTINATIONS);
        reflectEnum(HTMLLinkElement.prototype, 'referrerPolicy', 'referrerpolicy', REFERRER_POLICIES);
        // "" is not a keyword, so it falls through to the invalid-value default.
        reflectNullableEnum(HTMLLinkElement.prototype, 'crossOrigin', 'crossorigin', ['anonymous', 'use-credentials'], 'anonymous');
        reflectBoolean(HTMLLinkElement.prototype, 'disabled', 'disabled');

        reflectString(HTMLBaseElement.prototype, 'target', 'target');

        reflectString(HTMLMetaElement.prototype, 'name', 'name');
        reflectString(HTMLMetaElement.prototype, 'content', 'content');
        reflectString(HTMLMetaElement.prototype, 'media', 'media');
        reflectString(HTMLMetaElement.prototype, 'scheme', 'scheme');
        reflectString(HTMLMetaElement.prototype, 'httpEquiv', 'http-equiv');

        reflectString(HTMLStyleElement.prototype, 'media', 'media');
        reflectString(HTMLStyleElement.prototype, 'nonce', 'nonce');

        const htmlElementConstructors = {
          a: HTMLAnchorElement,
          marquee: HTMLMarqueeElement,
          font: HTMLFontElement,
          dir: HTMLDirectoryElement,
          frame: HTMLFrameElement,
          frameset: HTMLFrameSetElement,
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
            if (ctx.documentReadyState !== 'loading') {
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
          get readyState() { return ctx.documentReadyState; }
          set readyState(value) { setDocumentReadyState(value); }
          get characterSet() { return 'UTF-8'; }
          get charset() { return 'UTF-8'; }
          get contentType() { return 'text/html'; }
          get compatMode() { return bridge.compatMode(); }
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
            if (ctx.customElementsRegistry) {
              const customElement = ctx.customElementsRegistry._construct(localName, id);
              if (customElement) return customElement;
            }
            const element = wrapNode(id);
            markDomCreatedScript(element);
            return element;
          }
          createElementNS(namespaceURI, qualifiedName) {
            const namespace = String(namespaceURI || '');
            const localName = String(qualifiedName).split(':').pop().toLowerCase();
            const id = bridge.createElement(localName);
            if (ctx.customElementsRegistry && namespace === 'http://www.w3.org/1999/xhtml') {
              const customElement = ctx.customElementsRegistry._construct(localName, id);
              if (customElement) return applyElementNamespace(customElement, namespace);
            }
            const element = wrapNode(id);
            applyElementNamespace(element, namespace);
            markDomCreatedScript(element);
            return element;
          }
          createTextNode(text) { return wrapNode(bridge.createTextNode(String(text ?? ''))); }
          createComment(text) { return wrapNode(bridge.createComment(String(text ?? ''))); }
          createDocumentFragment() { return new DocumentFragment(); }
          getElementById(id) { return wrapNode(bridge.getElementById(String(id))); }
          querySelector(selector) { return querySelectorCompat(this, selector); }
          // Static per spec: a snapshot, not a live view.
          querySelectorAll(selector) { return staticNodeList(querySelectorAllCompat(this, selector)); }

          // getElementsBy* are live, so they re-run the query on every access.
          getElementsByTagName(tagName) {
            const name = String(tagName);
            return liveCollection(HTMLCollection, () => querySelectorAllCompat(this, name));
          }

          getElementsByClassName(classNames) {
            const selector = String(classNames).trim().split(/\s+/).filter(Boolean).map((name) => `.${name}`).join('');
            return liveCollection(HTMLCollection, () => (selector ? querySelectorAllCompat(this, selector) : []));
          }
          getElementsByName(name) { return this.querySelectorAll(`[name="${String(name).replace(/"/g, '\\"')}"]`); }
          createEvent(type) {
            const Constructor = Object.prototype.hasOwnProperty.call(legacyEventConstructors, asciiLowercase(type))
              ? legacyEventConstructors[asciiLowercase(type)]
              : null;
            if (!Constructor) {
              throw new DOMException(`The provided event type ("${type}") is invalid.`, 'NotSupportedError');
            }
            return new Constructor('');
          }
          createTreeWalker(root, whatToShow = NodeFilter.SHOW_ALL, filter = null) {
            return new TreeWalker(root, whatToShow, filter);
          }
          createNodeIterator(root, whatToShow = NodeFilter.SHOW_ALL, filter = null) {
            return new NodeIterator(root, whatToShow, filter);
          }
          createRange() { return new Range(this); }
          getSelection() { return selection; }
          hasFocus() { return true; }
          // No coordinate-based hit-testing (PageCore has no visual hit region
          // model), but the API must exist: it is a baseline method many libraries
          // call without feature-detecting it. Returning null is a spec-legitimate
          // result ("no element at the point") and never a wrong element.
          elementFromPoint() { return null; }
          elementsFromPoint() { return []; }
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
          __markScriptStarted(id) {
            markScriptStarted(id == null ? null : wrapNode(id));
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
          createTreeWalker(root, whatToShow = NodeFilter.SHOW_ALL, filter = null) {
            return new TreeWalker(root, whatToShow, filter);
          }
          createNodeIterator(root, whatToShow = NodeFilter.SHOW_ALL, filter = null) {
            return new NodeIterator(root, whatToShow, filter);
          }
          createRange() { return document.createRange(); }
          getSelection() { return selection; }
          hasFocus() { return false; }
          elementFromPoint() { return null; }
          elementsFromPoint() { return []; }
          adoptNode(node) { return node; }
          importNode(node, deep = false) { return node && node.cloneNode ? node.cloneNode(Boolean(deep)) : node; }

          getElementById(id) {
            const escaped = String(id).replace(/\\/g, '\\\\').replace(/"/g, '\\"');
            return this.querySelector(`[id="${escaped}"]`);
          }

          querySelector(selector) { return this.querySelectorAll(selector)[0] || null; }

          // The document-level queries must also consider documentElement itself,
          // which an Element-rooted query never returns.
          _matchesInDocument(selector) {
            const root = this.documentElement;
            if (!root) return [];
            const results = [...root.querySelectorAll(selector)];
            return root.matches(selector) ? [root, ...results] : results;
          }

          querySelectorAll(selector) { return staticNodeList(this._matchesInDocument(selector)); }

          getElementsByTagName(tagName) {
            const name = String(tagName).toLowerCase();
            return liveCollection(HTMLCollection, () => this._matchesInDocument(name === '*' ? '*' : name));
          }

          getElementsByClassName(classNames) {
            const selector = String(classNames).trim().split(/\s+/).filter(Boolean).map((name) => `.${name}`).join('');
            return liveCollection(HTMLCollection, () => (selector ? this._matchesInDocument(selector) : []));
          }

          getElementsByName(name) {
            const selector = `[name="${String(name).replace(/"/g, '\\"')}"]`;
            return liveCollection(NodeList, () => this._matchesInDocument(selector));
          }
        }

        // PageCore has a single, HTML-parsed document, so Document and
        // HTMLDocument are the same class (see the Symbol.toStringTag override
        // below, which keeps the real document reporting "[object HTMLDocument]").
        // XMLDocument still needs to exist and be distinct: WPT checks that a
        // DOMParser-produced document is *not* an XMLDocument, which is only a
        // meaningful assertion if the interface is installed.
        class XMLDocument extends Document {}

        class DOMImplementation {
          createHTMLDocument(title = '') { return new DetachedHTMLDocument(title); }
          hasFeature() { return true; }
        }

        const domImplementation = new DOMImplementation();

        // Builds (and caches) a wrapper from an already-fetched descriptor, so no
        // bridge crossing is needed here. `tag` is only present for elements.
        function materializeNode(id, type, tag) {
          let node;
          if (type === 9) node = new Document(id);
          else if (type === 3) node = new Text(INTERNAL_NODE_ID, id);
          else if (type === 8) node = new Comment(INTERNAL_NODE_ID, id);
          else if (type === 1) {
            const tagName = (tag || '').toLowerCase();
            const Constructor = htmlElementConstructors[tagName] || (tagName.includes('-') ? HTMLElement : HTMLUnknownElement);
            node = new Constructor(id);
          } else node = new Node(id);

          wrapperCache.set(id, node);
          if (type === 1) scheduleElementResourceLoad(node);
          return node;
        }

        function wrapNode(id) {
          if (!id) return null;
          // syncMutationCache() prunes any wrapper whose node was removed, so a
          // surviving cache entry is guaranteed live and needs no hasNode check.
          syncMutationCache();
          const cached = wrapperCache.get(id);
          if (cached !== undefined) return cached;

          // One bridge crossing returns liveness + type (+ tag) instead of the
          // former hasNode + nodeType + tagName round-trips.
          const descriptor = bridge.describeNode(id);
          if (!descriptor) return null;
          return materializeNode(id, descriptor.type, descriptor.tag);
        }

        // Materializes a list of descriptors (from childNodesDescribed/
        // childrenDescribed) into wrappers with a single bridge crossing for the
        // whole list, reusing cached wrappers where present.
        function wrapDescribedList(descriptors) {
          syncMutationCache();
          const out = [];
          for (let i = 0; i < descriptors.length; i++) {
            const descriptor = descriptors[i];
            const cached = wrapperCache.get(descriptor.id);
            out.push(cached !== undefined ? cached : materializeNode(descriptor.id, descriptor.type, descriptor.tag));
          }
          return out;
        }

        // Returns the traversal cache entry for `node` valid at the current
        // mutation version, resetting it whenever the DOM has changed.
        function traversalEntry(node) {
          const version = bridge.mutationVersion();
          let entry = traversalCache.get(node);
          if (entry === undefined || entry.version !== version) {
            entry = { version, childNodes: null, children: null };
            traversalCache.set(node, entry);
          }
          return entry;
        }

        // A host's shadow container is a real Lexbor child (see
        // attach_shadow_root()) but must stay out of the host's own light-DOM
        // traversal — encapsulation, and it's already reachable as node.__shadowRoot.
        function excludingShadowContainer(node, descriptors) {
          const containerId = node.__shadowRoot ? node.__shadowRoot.__id : null;
          return containerId === null ? descriptors : descriptors.filter((descriptor) => descriptor.id !== containerId);
        }

        // Internal: returns the SHARED cached childNodes list (callers must not
        // mutate it). The public childNodes getter hands out a copy.
        function liveChildNodeList(node) {
          const entry = traversalEntry(node);
          if (entry.childNodes === null) {
            entry.childNodes = wrapDescribedList(excludingShadowContainer(node, bridge.childNodesDescribed(node._liveId())));
          }
          return entry.childNodes;
        }

        function liveChildElementList(node) {
          const entry = traversalEntry(node);
          if (entry.children === null) {
            entry.children = wrapDescribedList(excludingShadowContainer(node, bridge.childrenDescribed(node._liveId())));
          }
          return entry.children;
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
            ctx.pendingCustomElementNodeId = id;
            try {
              element = new constructor();
            } catch (error) {
              console.error(error);
              return null;
            } finally {
              ctx.pendingCustomElementNodeId = null;
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

      ctx.wrapNode = wrapNode;
      ctx.isDocumentFragment = isDocumentFragment;
      ctx.document = document;
      ctx.Element = Element;

      const exports = {
        document,
        Node,
        CharacterData,
        Text,
        Comment,
        NodeList,
        HTMLCollection,
        DOMStringMap,
        XMLDocument,
        Attr,
        Element,
        HTMLElement,
        HTMLAnchorElement,
        HTMLAreaElement,
        HTMLAudioElement,
        HTMLBaseElement,
        HTMLBodyElement,
        HTMLBRElement,
        HTMLButtonElement,
        HTMLCanvasElement,
        HTMLDataElement,
        HTMLDataListElement,
        HTMLDetailsElement,
        HTMLDialogElement,
        HTMLDivElement,
        HTMLDListElement,
        HTMLDirectoryElement,
        HTMLFontElement,
        HTMLFrameElement,
        HTMLFrameSetElement,
        HTMLMarqueeElement,
        HTMLEmbedElement,
        HTMLFieldSetElement,
        HTMLFormElement,
        HTMLHeadElement,
        HTMLHeadingElement,
        HTMLHRElement,
        HTMLHtmlElement,
        HTMLIFrameElement,
        HTMLImageElement,
        HTMLInputElement,
        HTMLLabelElement,
        HTMLLegendElement,
        HTMLLIElement,
        HTMLLinkElement,
        HTMLMapElement,
        HTMLMediaElement,
        HTMLMetaElement,
        HTMLMeterElement,
        HTMLModElement,
        HTMLObjectElement,
        HTMLOListElement,
        HTMLOptGroupElement,
        HTMLOptionElement,
        HTMLOutputElement,
        HTMLParagraphElement,
        HTMLParamElement,
        HTMLPictureElement,
        HTMLPreElement,
        HTMLProgressElement,
        HTMLQuoteElement,
        HTMLScriptElement,
        HTMLSelectElement,
        HTMLSlotElement,
        HTMLSourceElement,
        HTMLSpanElement,
        HTMLStyleElement,
        HTMLTableCaptionElement,
        HTMLTableCellElement,
        HTMLTableColElement,
        HTMLTableElement,
        HTMLTableRowElement,
        HTMLTableSectionElement,
        HTMLTemplateElement,
        HTMLTextAreaElement,
        HTMLTimeElement,
        HTMLTitleElement,
        HTMLTrackElement,
        HTMLUListElement,
        HTMLUnknownElement,
        HTMLVideoElement,
        SVGElement,
        SVGGraphicsElement,
        SVGSVGElement,
        SVGPathElement,
        SVGCircleElement,
        SVGEllipseElement,
        SVGLineElement,
        SVGRectElement,
        SVGPolygonElement,
        SVGPolylineElement,
        SVGGElement,
        SVGDefsElement,
        SVGUseElement,
        SVGSymbolElement,
        SVGTitleElement,
        SVGDescElement,
        Document,
        DocumentFragment,
        ShadowRoot,
	        DOMImplementation,
	        ElementInternals,
	        ValidityState,
	        DOMTokenList,
	        NodeFilter,
	        TreeWalker,
	        NodeIterator,
	        CustomElementRegistry,
        CSSRule,
        CSSStyleRule,
        CSSMediaRule,
        CSSStyleSheet,
        CSSStyleDeclaration,
        computedStyleFor,
        styleSheetsForDocument,
        parseHTMLFragment,
        fragmentFromHTML,
        wrapNode,
        isDocumentFragment,
        installWindowNamedPropertiesFromTree
      };

      // Give every Node-derived interface a Symbol.toStringTag so
      // Object.prototype.toString.call(node) reports the real interface name
      // (e.g. "[object HTMLDivElement]", "[object HTMLDocument]") instead of the
      // giveaway "[object Object]". The names come from the export keys, which —
      // unlike Constructor.name — survive the optional class-name minifier. The
      // shim collapses Document/HTMLDocument into one class, so the real
      // document object must still report "HTMLDocument".
      const interfaceTagOverrides = { Document: 'HTMLDocument' };
      for (const [name, value] of Object.entries(exports)) {
        if (typeof value !== 'function' || !value.prototype) continue;
        const isNodeInterface = value === Node || value === DocumentFragment
          || value.prototype instanceof Node || value.prototype instanceof DocumentFragment;
        if (!isNodeInterface) continue;
        const tag = interfaceTagOverrides[name] || name;
        Object.defineProperty(value.prototype, Symbol.toStringTag, {
          get() { return tag; },
          configurable: true
        });
      }

      ctx.dom = exports;
      return exports;
    }
  };
});
