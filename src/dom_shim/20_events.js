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
    name: 'events',
    deps: ['core'],
    install(ctx, api) {
      const { global, host } = ctx;
      const {
        DOM_EXCEPTION_CODES,
        DOM_EXCEPTION_LEGACY_CONSTANTS,
        defineValue,
        isNodeWrapper
      } = api.core;


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
        for (const [name, code] of Object.entries(DOM_EXCEPTION_LEGACY_CONSTANTS)) {
          defineValue(DOMException, name, code, true);
          defineValue(DOMException.prototype, name, code, true);
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
          // Only escalate to window when the node is actually rooted in the
          // document; a detached node must not reach window.
          const rooted = start === global
            || start === global.document
            || (typeof start.isConnected === 'boolean' ? start.isConnected : true);
          if (rooted && path[path.length - 1] !== global) {
            path.push(global);
          }
          return path;
        }

        function connectCustomElementTree(node) {
          if (!ctx.customElementsRegistry) return;
          if (node instanceof ctx.Element) {
            ctx.customElementsRegistry._upgrade(node);
            invokeCustomElementConnected(ctx.wrapperCache.get(node.__id) || node);
          }
          if (node && node.querySelectorAll) {
            for (const child of node.querySelectorAll('*')) {
              ctx.customElementsRegistry._upgrade(child);
              invokeCustomElementConnected(ctx.wrapperCache.get(child.__id) || child);
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

        // Invoke listeners on one target for one phase. mode:
        //  'capture' -> only capture listeners (no on<type> handler),
        //  'bubble'  -> only non-capture listeners (+ on<type> handler),
        //  'target'  -> all listeners in registration order (+ on<type> handler).
        function invokeTargetListeners(target, event, mode) {
          const listeners = [...(ensureListeners(target).get(event.type) || [])];
          for (const entry of listeners) {
            if (mode === 'capture' && !entry.capture) continue;
            if (mode === 'bubble' && entry.capture) continue;
            callEventListener(target, entry, event);
            if (entry.once) target.removeEventListener(event.type, entry.callback, { capture: entry.capture });
            if (event._immediateStopped) return;
          }

          if (mode !== 'capture' && !event._immediateStopped) {
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
            const kept = [];
            for (const entry of list) {
              if (entry.callback === callback && entry.capture === opts.capture) {
                // Detach the abort listener so a long-lived signal does not
                // accumulate closures across add/remove cycles.
                if (entry.signal && entry.abortRemove
                    && typeof entry.signal.removeEventListener === 'function') {
                  try {
                    entry.signal.removeEventListener('abort', entry.abortRemove);
                  } catch (_abortCleanupError) {
                  }
                }
              } else {
                kept.push(entry);
              }
            }
            listeners.set(key, kept);
          }

          dispatchEvent(event) {
            if (!(event instanceof Event)) {
              throw new TypeError('dispatchEvent expects Event');
            }
            // Reset propagation state so an Event object can be re-dispatched.
            event.target = this;
            event.currentTarget = null;
            event.cancelBubble = false;
            event._immediateStopped = false;
            event._path = eventPath(this);

            const path = event._path; // [target, ...ancestors, window]

            // Capture phase: root -> just above the target.
            event.eventPhase = Event.CAPTURING_PHASE;
            for (let i = path.length - 1; i >= 1 && !event.cancelBubble; --i) {
              event.currentTarget = path[i];
              invokeTargetListeners(path[i], event, 'capture');
            }

            // Target phase: capture and bubble listeners in registration order.
            if (!event.cancelBubble) {
              event.eventPhase = Event.AT_TARGET;
              event.currentTarget = this;
              invokeTargetListeners(this, event, 'target');
            }

            // Bubble phase: just above the target -> root (only if bubbling).
            if (event.bubbles) {
              event.eventPhase = Event.BUBBLING_PHASE;
              for (let i = 1; i < path.length && !event.cancelBubble; ++i) {
                event.currentTarget = path[i];
                invokeTargetListeners(path[i], event, 'bubble');
              }
            }

            event.currentTarget = null;
            event.eventPhase = Event.NONE;
            event.cancelBubble = false;
            event._immediateStopped = false;
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
            const wantsOldValue = observer._matchWithOldValue(record);
            if (wantsOldValue === null) continue;
            // Each observer gets its own record; oldValue is exposed only when
            // that observer asked for it (attributeOldValue/characterDataOldValue).
            const delivered = Object.assign({}, record);
            delivered.oldValue = wantsOldValue && record.oldValue !== undefined ? record.oldValue : null;
            observer._records.push(delivered);
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
            if (!isNodeWrapper(target) && !ctx.isDocumentFragment(target)) {
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
            return this._matchWithOldValue(record) !== null;
          }

          // Returns null when the record matches no observation, otherwise a
          // boolean indicating whether any matching observation requested the
          // record's oldValue.
          _matchWithOldValue(record) {
            let matched = false;
            let wantsOldValue = false;
            for (const { target, options } of this._observations) {
              if (record.type === 'childList' && !options.childList) continue;
              if (record.type === 'attributes' && !options.attributes) continue;
              if (record.type === 'characterData' && !options.characterData) continue;
              if (record.type === 'attributes' && options.attributeFilter
                  && !options.attributeFilter.includes(record.attributeName)) continue;
              const isTarget = target === record.target
                || Boolean(options.subtree && target.contains && target.contains(record.target));
              if (!isTarget) continue;
              matched = true;
              if (record.type === 'attributes' && options.attributeOldValue) wantsOldValue = true;
              if (record.type === 'characterData' && options.characterDataOldValue) wantsOldValue = true;
            }
            return matched ? wantsOldValue : null;
          }
        }

      ctx.queueMutation = queueMutation;

      return {
        DOMException,
        Event,
        CustomEvent,
        MessageEvent,
        UIEvent,
        MouseEvent,
        KeyboardEvent,
        PointerEvent,
        EventTarget,
        Window,
        installWindowIdentity,
        MessagePort,
        MessageChannel,
        AbortSignal,
        AbortController,
        MutationObserver,
        queueMutation,
        connectCustomElementTree,
        notifyCustomElementAttributeChanged,
        invokeCustomElementConnected,
        reportEventListenerError
      };
    }
  };
});
