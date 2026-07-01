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
    name: 'web',
    deps: ['core', 'events', 'dom'],
    install(ctx, api) {
      const { global, host } = ctx;
      const { defineValue, assertNode, absoluteURL, activityBegin, activityEnd } = api.core;
      const {
        DOMException,
        EventTarget,
        Event,
        MessageEvent,
        KeyboardEvent
      } = api.events;
      const { document, fragmentFromHTML } = api.dom;


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
            // Optional callback invoked after mutations so an owning URL can keep
            // its search/href in sync with a live searchParams object.
            this._onChange = null;
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

          _notify() { if (typeof this._onChange === 'function') this._onChange(this.toString()); }
          append(name, value) { this._entries.push([String(name), String(value)]); this._notify(); }
          delete(name) {
            name = String(name);
            this._entries = this._entries.filter((entry) => entry[0] !== name);
            this._notify();
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
            this._notify();
          }
          sort() { this._entries.sort((left, right) => left[0].localeCompare(right[0])); this._notify(); }
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
            if (!this._searchParams) {
              this._searchParams = new URLSearchParams(this.search);
              // Keep the URL's search/href in sync with live mutations.
              this._searchParams._onChange = (serialized) => {
                this._parts.search = serialized ? `?${serialized}` : '';
              };
            }
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
            this.credentials = normalizeCredentials(init.credentials || (source && source.credentials) || 'same-origin');
            this.cache = init.cache || (source && source.cache) || 'default';
            this.redirect = init.redirect || (source && source.redirect) || 'follow';
            this.referrer = normalizeReferrer(
              Object.prototype.hasOwnProperty.call(init, 'referrer')
                ? init.referrer
                : (source ? source.referrer : 'about:client'));
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
            this.redirected = Boolean(init.redirected);
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
              url: this.url,
              redirected: this.redirected
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
          if (response && response.headers && typeof response.headers[Symbol.iterator] === 'function') {
            for (const pair of response.headers) {
              const name = String(pair[0] || '');
              if (name.toLowerCase() === 'set-cookie' || name.toLowerCase() === 'set-cookie2') continue;
              headers.append(name, pair[1]);
            }
          }
          if (response && response.mimeType && !headers.has('content-type')) headers.set('content-type', response.mimeType);
          return headers;
        }

        function normalizeCredentials(value) {
          const credentials = String(value || 'same-origin');
          if (credentials === 'omit' || credentials === 'same-origin' || credentials === 'include') return credentials;
          throw new TypeError('Invalid credentials mode');
        }

        function normalizeReferrer(value) {
          if (value === undefined || value === null || value === 'about:client') return 'about:client';
          const referrer = String(value);
          if (referrer === '' || referrer === 'no-referrer') return '';
          return new URL(referrer, global.location.href || undefined).href;
        }

        function bodyText(body) {
          if (body == null) return '';
          if (body instanceof Blob) return decodeUtf8(body._bytes);
          if (body instanceof Uint8Array) return decodeUtf8(body);
          if (body instanceof ArrayBuffer) return decodeUtf8(new Uint8Array(body));
          if (ArrayBuffer.isView(body)) return decodeUtf8(new Uint8Array(body.buffer, body.byteOffset, body.byteLength));
          return String(body);
        }

        function headerPairs(headers) {
          return headers ? Array.from(headers.entries()) : [];
        }

        function loadHostResource(url, kind = 'other', init = {}) {
          if (!host || typeof host.loadResource !== 'function') {
            throw new Error('resource loading is not available');
          }
          return host.loadResource(
            absoluteURL(url),
            kind,
            init.method || 'GET',
            init.body == null ? '' : String(init.body),
            headerPairs(init.headers),
            normalizeCredentials(init.credentials || 'same-origin'),
            normalizeReferrer(Object.prototype.hasOwnProperty.call(init, 'referrer') ? init.referrer : 'about:client')
          );
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
            this._activityPending = false;
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
            this._endActivity();
          }

          send(body = null) {
            if (this.readyState !== XMLHttpRequest.OPENED) throw new DOMException('XMLHttpRequest is not open.', 'InvalidStateError');
            this._beginActivity();
            const run = () => {
              if (this._aborted) {
                this._endActivity();
                return;
              }
              let loaded = null;
              try {
                loaded = loadHostResource(this._url, 'other', {
                  method: this._method,
                  body: bodyText(body),
                  headers: this._requestHeaders,
                  credentials: this.withCredentials ? 'include' : 'same-origin',
                  referrer: 'about:client'
                });
                this.status = loaded.status === undefined ? 200 : Number(loaded.status);
                this.statusText = loaded.statusText === undefined ? '' : String(loaded.statusText);
                this.responseURL = loaded.url || this._url;
                this._responseHeaders = responseHeadersFromHost(loaded);
                if (this._overrideMimeType) this._responseHeaders.set('content-type', this._overrideMimeType);
                this.responseText = loaded.body || '';
                this.response = this.responseType === 'json'
                  ? (this.responseText ? JSON.parse(this.responseText) : null)
                  : this.responseText;
              } catch (error) {
                if (this._aborted) {
                  this._endActivity();
                  return;
                }
                this.status = 0;
                this.statusText = '';
                this.response = '';
                this.responseText = '';
                this.readyState = XMLHttpRequest.DONE;
                this._fire('readystatechange');
                this._fire('error');
                this._fire('loadend');
                this._endActivity();
                return;
              }

              if (this._aborted) {
                this._endActivity();
                return;
              }
              this.readyState = XMLHttpRequest.HEADERS_RECEIVED;
              this._fire('readystatechange');

              this.readyState = XMLHttpRequest.LOADING;
              this._fire('readystatechange');

              this.readyState = XMLHttpRequest.DONE;
              this._fire('readystatechange');
              this._fire('load');
              this._fire('loadend');
              this._endActivity();
            };

            void body;
            if (this._async) global.setTimeout(run, 0);
            else run();
          }

          _beginActivity() {
            if (this._activityPending) return;
            this._activityPending = true;
            activityBegin('xhr-fetch');
          }

          _endActivity() {
            if (!this._activityPending) return;
            this._activityPending = false;
            activityEnd('xhr-fetch');
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

        const timers = new Map();
        let nextTimerId = 1;
        let timerNow = 0;

        function normalizeDelay(delay) {
          const value = Number(delay);
          if (!Number.isFinite(value) || value < 0) return 0;
          return value;
        }

        function reportTimerError(error) {
          try {
            if (global.console && typeof global.console.error === 'function') {
              global.console.error(error);
            } else if (host && typeof host.log === 'function') {
              host.log('error', error && error.stack ? error.stack : String(error));
            }
          } catch (_reportError) {
          }
        }

        function callTimerCallback(timer) {
          if (typeof timer.callback === 'function') {
            // Timer callbacks run with the global object as `this`, per spec,
            // so non-strict callbacks relying on `this === window` work.
            try {
              timer.callback.call(global, ...timer.args);
            } catch (error) {
              reportTimerError(error);
            }
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
          if (!host || typeof host.randomBytes !== 'function') {
            throw new DOMException('A secure random source is not available', 'NotSupportedError');
          }
          const random = host.randomBytes(array.byteLength);
          const bytes = new Uint8Array(array.buffer, array.byteOffset, array.byteLength);
          for (let index = 0; index < bytes.length; index++) {
            bytes[index] = random[index] & 0xff;
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

      function setTimeoutShim(callback, delay = 0, ...args) {
        const id = nextTimerId++;
        const timeout = normalizeDelay(delay);
        timers.set(id, { callback, delay: timeout, due: timerNow + timeout, args, interval: false });
        return id;
      }

      function clearTimer(id) {
        return timers.delete(id);
      }

      function setIntervalShim(callback, delay = 0, ...args) {
        const id = nextTimerId++;
        const timeout = normalizeDelay(delay);
        timers.set(id, { callback, delay: timeout, due: timerNow + timeout, args, interval: true });
        return id;
      }

      function requestAnimationFrameShim(callback) {
        return setTimeoutShim(() => callback(timerNow), 16);
      }

      function requestIdleCallbackShim(callback) {
        return setTimeoutShim(() => callback({ didTimeout: false, timeRemaining: () => 0 }), 0);
      }

      function performanceNow() {
        return timerNow;
      }
      ctx.pagecoreNow = performanceNow;

      function runTimers(advanceMs = 0) {
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
      }

      function timerSnapshot(horizonMs = 0) {
        const horizon = timerNow + normalizeDelay(horizonMs);
        let relevant = 0;
        let nextDue = Infinity;
        for (const timer of timers.values()) {
          if (timer.interval || timer.due > horizon) continue;
          relevant++;
          if (timer.due < nextDue) nextDue = timer.due;
        }
        return {
          now: timerNow,
          relevant,
          nextDelay: nextDue === Infinity ? -1 : Math.max(0, nextDue - timerNow)
        };
      }

      return {
        DOMRectReadOnly,
        DOMRect,
        Range,
        Selection,
        selection,
        URLSearchParams,
        URL,
        TextEncoder,
        TextDecoder,
        Headers,
        Blob,
        File,
        Request,
        Response,
        responseHeadersFromHost,
        bodyText,
        loadHostResource,
        XMLHttpRequest,
        Storage,
        XMLSerializer,
        DOMParser,
        locationFromURL,
        makeMediaQueryList,
        getRandomValues,
        randomUUID,
        installWptHook,
        setTimeoutShim,
        clearTimer,
        setIntervalShim,
        requestAnimationFrameShim,
        requestIdleCallbackShim,
        performanceNow,
        runTimers,
        timerSnapshot
      };
    }
  };
});
