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
        loadHostResource
      } = api.core;
      const {
        DOMException,
        EventTarget,
        Event,
        MouseEvent,
        KeyboardEvent,
        installWindowIdentity,
        connectCustomElementTree,
        notifyCustomElementAttributeChanged,
        invokeCustomElementConnected
      } = api.events;
      const wrapperCache = ctx.wrapperCache;
      // Per-node cache of the materialized childNodes/children wrapper lists,
      // keyed (per entry) by the bridge mutation version. Repeated traversal
      // (firstChild/lastChild/siblings/loops) then avoids rebuilding the list and
      // re-crossing the bridge until the DOM actually changes. WeakMap keeps it
      // off the node objects and lets entries be collected with their nodes.
      const traversalCache = new WeakMap();
      let activeElement = null;
      let nextCssRootId = 1;
      const cookieJar = new Map();
      const absoluteURLCache = new Map();
      const stylesheetTextCache = new Map();
      const stylesheetCache = new Map();
      let stylesheetListKey = '';
      let stylesheetListCache = null;
      let adoptedStyleSheets = [];
      let cssomVersion = 0;
      let stylesheetListMutationVersion = -1;

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
          get childNodes() { return liveChildNodeList(this).slice(); }
          get firstChild() { const nodes = liveChildNodeList(this); return nodes[0] || null; }
          get lastChild() {
            const nodes = liveChildNodeList(this);
            return nodes[nodes.length - 1] || null;
          }
          get previousSibling() {
            const parent = this.parentNode;
            if (!parent) return null;
            const nodes = liveChildNodeList(parent);
            const index = nodes.findIndex((node) => node.__id === this.__id);
            return index > 0 ? nodes[index - 1] : null;
          }
          get nextSibling() {
            const parent = this.parentNode;
            if (!parent) return null;
            const nodes = liveChildNodeList(parent);
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

            const removedNodes = this.childNodes;
            bridge.setTextContent(this._liveId(), text);
            syncMutationCache();
            if (ctx.suppressMutationRecords === 0) {
              ctx.queueMutation({
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
          return root.querySelectorAll('button,input,select,textarea');
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

        function computedDisplay(element) {
          return computedStyleFor(element).display;
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

        function computedStyleFor(element) {
          // litehtml's cascade is keyed on the bridge's mutation version (see
          // Page::Impl::ensure_styled_document), so a value computed for the
          // current version stays valid until the DOM changes again.
          let cachedValues = null;
          let cachedVersion = -1;
          const styleValues = () => {
            if (!(element instanceof Element)) return Object.create(null);
            const version = bridge.mutationVersion();
            if (cachedValues === null || cachedVersion !== version) {
              cachedValues = bridge.computedStyle(element.__id);
              cachedVersion = version;
            }
            return cachedValues;
          };
          const propertyNames = () => Object.keys(styleValues());
          const propertyValue = (name) => {
            const property = cssPropertyName(name);
            if (!property) return '';
            return styleValues()[property] || '';
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
          get children() { return liveChildElementList(this).slice(); }
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
              attributeName,
              oldValue
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
              attributeName,
              oldValue
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
        class HTMLAnchorElement extends HTMLElement {}
        class HTMLAreaElement extends HTMLElement {}
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
            for (const control of this.elements) {
              if ('defaultValue' in control) control.value = control.defaultValue;
              if ('defaultChecked' in control) control.checked = control.defaultChecked;
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
        class HTMLImageElement extends HTMLElement {}
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
          get checked() { return this.hasAttribute('checked'); }
          set checked(value) { this.toggleAttribute('checked', Boolean(value)); }
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
          get selected() { return this.hasAttribute('selected'); }
          set selected(value) { this.toggleAttribute('selected', Boolean(value)); }
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
        class HTMLScriptElement extends HTMLElement {}
        class HTMLSelectElement extends HTMLFormControlElement {
          get options() { return this.querySelectorAll('option'); }
          get value() {
            const selected = this.querySelector('option[selected]');
            const option = selected || this.querySelector('option');
            return option ? option.value : '';
          }
          set value(value) {
            const text = String(value);
            for (const option of this.querySelectorAll('option')) {
              option.selected = option.value === text;
            }
          }
          get selectedIndex() {
            return this.options.findIndex((option) => option.selected);
          }
          set selectedIndex(value) {
            const index = Number(value);
            this.options.forEach((option, optionIndex) => { option.selected = optionIndex === index; });
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
          get value() { return this.getAttribute('value') || this.textContent || ''; }
          set value(value) { this.textContent = String(value); }
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
            if (ctx.customElementsRegistry) {
              const customElement = ctx.customElementsRegistry._construct(localName, id);
              if (customElement) return customElement;
            }
            return wrapNode(id);
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
          createTreeWalker(root, whatToShow = NodeFilter.SHOW_ALL, filter = null) {
            return new TreeWalker(root, whatToShow, filter);
          }
          createNodeIterator(root, whatToShow = NodeFilter.SHOW_ALL, filter = null) {
            return new NodeIterator(root, whatToShow, filter);
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
          createTreeWalker(root, whatToShow = NodeFilter.SHOW_ALL, filter = null) {
            return new TreeWalker(root, whatToShow, filter);
          }
          createNodeIterator(root, whatToShow = NodeFilter.SHOW_ALL, filter = null) {
            return new NodeIterator(root, whatToShow, filter);
          }
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

        // Builds (and caches) a wrapper from an already-fetched descriptor, so no
        // bridge crossing is needed here. `tag` is only present for elements.
        function materializeNode(id, type, tag) {
          let node;
          if (type === 9) node = new Document(id);
          else if (type === 3) node = new Text(id);
          else if (type === 8) node = new Comment(id);
          else if (type === 1) {
            const tagName = (tag || '').toLowerCase();
            const Constructor = htmlElementConstructors[tagName] || (tagName.includes('-') ? HTMLElement : HTMLUnknownElement);
            node = new Constructor(id);
          } else node = new Node(id);

          wrapperCache.set(id, node);
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

        // Internal: returns the SHARED cached childNodes list (callers must not
        // mutate it). The public childNodes getter hands out a copy.
        function liveChildNodeList(node) {
          const entry = traversalEntry(node);
          if (entry.childNodes === null) {
            entry.childNodes = wrapDescribedList(bridge.childNodesDescribed(node._liveId()));
          }
          return entry.childNodes;
        }

        function liveChildElementList(node) {
          const entry = traversalEntry(node);
          if (entry.children === null) {
            entry.children = wrapDescribedList(bridge.childrenDescribed(node._liveId()));
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
        Text,
        Comment,
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
        isDocumentFragment
      };
      ctx.dom = exports;
      return exports;
    }
  };
});
