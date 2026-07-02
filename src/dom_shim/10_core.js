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
    name: 'core',
    deps: [],
    install(ctx, api) {
      if (!ctx.bridge) throw new Error('PageCore DOM bridge is not installed');
      if (!ctx.host) throw new Error('PageCore host bridge is not installed');

      ctx.wrapperCache = new Map();
      ctx.observedForgetVersion = ctx.bridge.forgetVersion();
      ctx.suppressMutationRecords = 0;
      ctx.customElementsRegistry = null;
      ctx.pendingCustomElementNodeId = null;
      ctx.documentReadyState = 'loading';
      ctx.queueMutation = null;
      ctx.wrapNode = null;
      ctx.isDocumentFragment = null;
      ctx.Element = null;
      ctx.document = null;
      const absoluteURLCache = new Map();

      const DOM_EXCEPTION_CODES = {
        IndexSizeError: 1,
        DOMStringSizeError: 2,
        HierarchyRequestError: 3,
        WrongDocumentError: 4,
        InvalidCharacterError: 5,
        NoDataAllowedError: 6,
        NoModificationAllowedError: 7,
        NotFoundError: 8,
        NotSupportedError: 9,
        InUseAttributeError: 10,
        InvalidStateError: 11,
        SyntaxError: 12,
        InvalidModificationError: 13,
        NamespaceError: 14,
        InvalidAccessError: 15,
        ValidationError: 16,
        TypeMismatchError: 17,
        SecurityError: 18,
        NetworkError: 19,
        AbortError: 20,
        URLMismatchError: 21,
        QuotaExceededError: 22,
        TimeoutError: 23,
        InvalidNodeTypeError: 24,
        DataCloneError: 25
      };

      const DOM_EXCEPTION_LEGACY_CONSTANTS = {
        INDEX_SIZE_ERR: 1,
        DOMSTRING_SIZE_ERR: 2,
        HIERARCHY_REQUEST_ERR: 3,
        WRONG_DOCUMENT_ERR: 4,
        INVALID_CHARACTER_ERR: 5,
        NO_DATA_ALLOWED_ERR: 6,
        NO_MODIFICATION_ALLOWED_ERR: 7,
        NOT_FOUND_ERR: 8,
        NOT_SUPPORTED_ERR: 9,
        INUSE_ATTRIBUTE_ERR: 10,
        INVALID_STATE_ERR: 11,
        SYNTAX_ERR: 12,
        INVALID_MODIFICATION_ERR: 13,
        NAMESPACE_ERR: 14,
        INVALID_ACCESS_ERR: 15,
        VALIDATION_ERR: 16,
        TYPE_MISMATCH_ERR: 17,
        SECURITY_ERR: 18,
        NETWORK_ERR: 19,
        ABORT_ERR: 20,
        URL_MISMATCH_ERR: 21,
        QUOTA_EXCEEDED_ERR: 22,
        TIMEOUT_ERR: 23,
        INVALID_NODE_TYPE_ERR: 24,
        DATA_CLONE_ERR: 25
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
        // Only a "forget" (innerHTML replacement or document reparse) can turn a
        // cached id stale; ordinary append/remove/setAttribute mutations leave
        // every tracked id valid. Gating on the forget version keeps DOM-building
        // and removal loops O(1) per step instead of rescanning every wrapper.
        const current = ctx.bridge.forgetVersion();
        if (current === ctx.observedForgetVersion) return;

        for (const [id] of ctx.wrapperCache) {
          if (!ctx.bridge.hasNode(id)) ctx.wrapperCache.delete(id);
        }

        ctx.observedForgetVersion = current;
      }

      function afterMutation(value, record = null) {
        syncMutationCache();
        activityMarkMutation();
        if (record && ctx.suppressMutationRecords === 0 && typeof ctx.queueMutation === 'function') ctx.queueMutation(record);
        return value;
      }

      function setDocumentReadyState(value) {
        ctx.documentReadyState = String(value);
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
        if (!ctx.bridge.hasNode(value.__id)) {
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
        if (typeof ctx.wrapNode !== 'function') throw new Error('PageCore DOM node wrapper is not installed');
        return ids.map((id) => ctx.wrapNode(id)).filter(Boolean);
      }

      function activityBegin(kind) {
        try {
          if (ctx.host && typeof ctx.host.activityBegin === 'function') ctx.host.activityBegin(kind);
        } catch (_activityError) {
        }
      }

      function activityEnd(kind) {
        try {
          if (ctx.host && typeof ctx.host.activityEnd === 'function') ctx.host.activityEnd(kind);
        } catch (_activityError) {
        }
      }

      function activityMarkMutation() {
        try {
          if (ctx.host && typeof ctx.host.activityMarkMutation === 'function') {
            const clock = typeof ctx.pagecoreNow === 'function' ? ctx.pagecoreNow() : undefined;
            ctx.host.activityMarkMutation(ctx.bridge.mutationVersion(), clock);
          }
        } catch (_activityError) {
        }
      }

      function absoluteURL(value) {
        const text = String(value ?? '');
        if (!text) return '';
        const base = ctx.global.location && ctx.global.location.href ? ctx.global.location.href : (ctx.host.baseURL || undefined);
        const cacheKey = `${base || ''}\n${text}`;
        if (absoluteURLCache.has(cacheKey)) return absoluteURLCache.get(cacheKey);
        if (absoluteURLCache.size > 2048) absoluteURLCache.clear();
        try {
          const URLConstructor = ctx.global.URL || globalThis.URL;
          const resolved = URLConstructor ? new URLConstructor(text, base).href : text;
          absoluteURLCache.set(cacheKey, resolved);
          return resolved;
        } catch (_error) {
          absoluteURLCache.set(cacheKey, text);
          return text;
        }
      }

      function normalizeReferrer(value) {
        if (value === undefined || value === null || value === 'about:client') return 'about:client';
        const referrer = String(value);
        if (referrer === '' || referrer === 'no-referrer') return '';
        return absoluteURL(referrer);
      }

      function formatErrorForLog(value) {
        if (!value || typeof value !== 'object') return value;
        try {
          const stack = typeof value.stack === 'string' && value.stack ? value.stack : '';
          const header = (typeof value.name === 'string' && typeof value.message === 'string')
            ? `${value.name}: ${value.message}` : '';
          if (stack) return header && stack.indexOf(header) !== 0 ? `${header}\n${stack}` : stack;
          if (header) return header;
        } catch (_error) {}
        return value;
      }

      function loadHostResource(url, kind = 'other', init = {}) {
        if (!ctx.host || typeof ctx.host.loadResource !== 'function') {
          throw new Error('resource loading is not available');
        }
        return ctx.host.loadResource(
          absoluteURL(url),
          kind,
          init.method || 'GET',
          init.body == null ? '' : String(init.body),
          Array.isArray(init.headers) ? init.headers : [],
          init.credentials || 'same-origin',
          normalizeReferrer(Object.prototype.hasOwnProperty.call(init, 'referrer') ? init.referrer : 'about:client')
        );
      }

      return {
        DOM_EXCEPTION_CODES,
        DOM_EXCEPTION_LEGACY_CONSTANTS,
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
        activityMarkMutation,
        absoluteURL,
        formatErrorForLog,
        loadHostResource
      };
    }
  };
});
