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
    name: 'install',
    deps: ['core', 'events', 'dom', 'web', 'forms', 'streams', 'compat'],
    install(ctx, api) {
      const { global, host, bridge } = ctx;
      const {
        defineValue,
        setDocumentReadyState,
        activityBegin,
        activityEnd,
        formatErrorForLog,
        cancelResourceLoad,
        completeResourceLoad
      } = api.core;
      const {
        DOMException,
        Window,
        EventTarget,
        Event,
        CustomEvent,
        MessageEvent,
        MessagePort,
        MessageChannel,
        UIEvent,
        MouseEvent,
        KeyboardEvent,
        PointerEvent,
        FocusEvent,
        HashChangeEvent,
        AbortSignal,
        AbortController,
        MutationObserver,
        MutationRecord,
        deliverMutationObservers,
        installWindowIdentity
      } = api.events;
      const {
        document,
        Node,
        CharacterData,
        Text,
        Comment,
        DocumentType,
        ProcessingInstruction,
        CDATASection,
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
        installWindowNamedPropertiesFromTree
      } = api.dom;
      const {
        DOMRectReadOnly,
        DOMRect,
        DOMRectList,
        CaretPosition,
        Range,
        StaticRange,
        Selection,
        URL,
        URLSearchParams,
        TextEncoder,
        TextDecoder,
        Headers,
        Blob,
        File,
        Request,
        Response,
        responseHeadersFromHost,
        bodyText,
        loadHostResourceAsync,
        XMLHttpRequest,
        Storage,
        XMLSerializer,
        DOMParser,
        locationFromURL,
        MediaQueryList,
        MediaQueryListEvent,
        makeMediaQueryList,
        ResizeObserver,
        IntersectionObserver,
        getRandomValues,
        randomUUID,
        installWptHook,
        setTimeoutShim,
        clearTask,
        setIntervalShim,
        requestAnimationFrameShim,
        cancelAnimationFrameShim,
        runAnimationFrames,
        requestIdleCallbackShim,
        performanceNow,
        queuePageTask,
        runTask
      } = api.web;
      const {
        FormData
      } = api.forms;
      const {
        ReadableStream,
        ReadableStreamDefaultController,
        ReadableStreamDefaultReader,
        ReadableByteStreamController,
        ReadableStreamBYOBReader,
        ReadableStreamBYOBRequest,
        WritableStream,
        WritableStreamDefaultWriter,
        WritableStreamDefaultController,
        TransformStream,
        TransformStreamDefaultController,
        ByteLengthQueuingStrategy,
        CountQueuingStrategy,
        TextDecoderStream,
        TextEncoderStream
      } = api.streams;
      const {
        CSS,
        btoa,
        atob,
        Audio,
        Option,
        createIntlFallback
      } = api.compat;

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
        global.FocusEvent = FocusEvent;
        global.HashChangeEvent = HashChangeEvent;
        global.AbortSignal = AbortSignal;
        global.AbortController = AbortController;
        global.MutationObserver = MutationObserver;
        global.MutationRecord = MutationRecord;
        global.Node = Node;
        global.CharacterData = CharacterData;
        global.Text = Text;
        global.Comment = Comment;
        global.DocumentType = DocumentType;
        global.ProcessingInstruction = ProcessingInstruction;
        global.CDATASection = CDATASection;
        global.NodeList = NodeList;
        global.HTMLCollection = HTMLCollection;
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
        global.HTMLDirectoryElement = HTMLDirectoryElement;
        global.HTMLFontElement = HTMLFontElement;
        global.HTMLFrameElement = HTMLFrameElement;
        global.HTMLFrameSetElement = HTMLFrameSetElement;
        global.HTMLMarqueeElement = HTMLMarqueeElement;
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
        // Legacy alias: PageCore parses a single HTML document, so Document and
        // HTMLDocument are one class (Symbol.toStringTag still reports
        // "[object HTMLDocument]" for the real document).
        global.HTMLDocument = Document;
        global.XMLDocument = XMLDocument;
        global.DocumentFragment = DocumentFragment;
        global.ShadowRoot = ShadowRoot;
        global.DOMImplementation = DOMImplementation;
        global.DOMStringMap = DOMStringMap;
        global.ElementInternals = ElementInternals;
        global.ValidityState = ValidityState;
        global.DOMTokenList = DOMTokenList;
        global.NodeFilter = NodeFilter;
        global.TreeWalker = TreeWalker;
        global.NodeIterator = NodeIterator;
        global.Range = Range;
        global.StaticRange = StaticRange;
        global.Selection = Selection;
        global.DOMRectReadOnly = DOMRectReadOnly;
        global.DOMRect = DOMRect;
        global.DOMRectList = DOMRectList;
        global.CaretPosition = CaretPosition;
        global.URL = URL;
        global.URLSearchParams = URLSearchParams;
        global.TextEncoder = TextEncoder;
        global.TextDecoder = TextDecoder;
        global.Headers = Headers;
        global.Blob = Blob;
        global.File = File;
        global.FormData = FormData;
        global.Request = Request;
        global.Response = Response;
        global.ReadableStream = ReadableStream;
        global.ReadableStreamDefaultController = ReadableStreamDefaultController;
        global.ReadableStreamDefaultReader = ReadableStreamDefaultReader;
        global.ReadableByteStreamController = ReadableByteStreamController;
        global.ReadableStreamBYOBReader = ReadableStreamBYOBReader;
        global.ReadableStreamBYOBRequest = ReadableStreamBYOBRequest;
        global.WritableStream = WritableStream;
        global.WritableStreamDefaultWriter = WritableStreamDefaultWriter;
        global.WritableStreamDefaultController = WritableStreamDefaultController;
        global.TransformStream = TransformStream;
        global.TransformStreamDefaultController = TransformStreamDefaultController;
        global.ByteLengthQueuingStrategy = ByteLengthQueuingStrategy;
        global.CountQueuingStrategy = CountQueuingStrategy;
        global.TextDecoderStream = TextDecoderStream;
        global.TextEncoderStream = TextEncoderStream;
        global.XMLHttpRequest = XMLHttpRequest;
        global.Storage = Storage;
        global.CSSRule = CSSRule;
        global.CSSStyleRule = CSSStyleRule;
        global.CSSMediaRule = CSSMediaRule;
        global.CSSStyleSheet = CSSStyleSheet;
        global.CSSStyleDeclaration = CSSStyleDeclaration;
        global.XMLSerializer = XMLSerializer;
        global.DOMParser = DOMParser;
        global.MediaQueryList = MediaQueryList;
        global.MediaQueryListEvent = MediaQueryListEvent;
        global.ResizeObserver = ResizeObserver;
        global.IntersectionObserver = IntersectionObserver;
        global.CSS = Object.assign(global.CSS || {}, CSS);
        global.btoa = btoa;
        global.atob = atob;
        ctx.customElementsRegistry = new CustomElementRegistry();
        global.CustomElementRegistry = CustomElementRegistry;
        global.customElements = ctx.customElementsRegistry;
        // User-Agent Client Hints (navigator.userAgentData). Derived from the
        // configured userAgent so the low-entropy hints stay consistent with
        // navigator.userAgent. Sites such as Google unconditionally call
        // getHighEntropyValues() and reject when the API is absent, so exposing a
        // well-formed NavigatorUAData avoids an unhandled rejection and a degraded
        // fallback path rather than falling back cleanly.
        function createUserAgentData(userAgent) {
          const ua = String(userAgent || '');
          const mobile = /Mobi|Android|iPhone|iPad|iPod/i.test(ua);
          let platform = 'Unknown';
          if (/Windows/i.test(ua)) platform = 'Windows';
          else if (/Android/i.test(ua)) platform = 'Android';
          else if (/iPhone|iPad|iPod|iOS/i.test(ua)) platform = 'iOS';
          else if (/Mac OS X|Macintosh/i.test(ua)) platform = 'macOS';
          else if (/CrOS/i.test(ua)) platform = 'Chrome OS';
          else if (/Linux/i.test(ua)) platform = 'Linux';

          const grease = { brand: 'Not_A Brand', version: '99' };
          const chrome = /(?:Chrome|Chromium|CriOS)\/(\d+)(?:[.\d]*)/i.exec(ua);
          let brands;
          let uaFullVersion;
          if (chrome) {
            const major = chrome[1];
            uaFullVersion = chrome[0].split('/')[1];
            brands = [
              { brand: 'Chromium', version: major },
              { brand: 'Google Chrome', version: major },
              grease
            ];
          } else {
            const product = /^([^/\s]+)\/(\S+)/.exec(ua);
            const name = product ? product[1] : 'PageCore';
            uaFullVersion = product ? product[2] : '0';
            brands = [{ brand: name, version: String(parseInt(uaFullVersion, 10) || 0) }, grease];
          }
          const fullVersionList = brands.map((b) =>
            b === grease
              ? { brand: grease.brand, version: grease.version + '.0.0.0' }
              : { brand: b.brand, version: uaFullVersion });

          const highEntropy = {
            architecture: /arm|aarch64/i.test(ua) ? 'arm' : 'x86',
            bitness: '64',
            formFactors: [mobile ? 'Mobile' : 'Desktop'],
            model: '',
            platformVersion: '',
            uaFullVersion,
            fullVersionList,
            wow64: false
          };

          return {
            brands: brands.slice(),
            mobile,
            platform,
            toJSON() {
              return { brands: this.brands, mobile: this.mobile, platform: this.platform };
            },
            getHighEntropyValues(hints) {
              const result = { brands: brands.slice(), mobile, platform };
              const requested = Array.isArray(hints) ? hints : [];
              for (const hint of requested) {
                if (Object.prototype.hasOwnProperty.call(highEntropy, hint)) {
                  result[hint] = highEntropy[hint];
                }
              }
              return Promise.resolve(result);
            }
          };
        }
        global.navigator = {
          userAgent: host.userAgent || 'PageCore/0.1',
          userAgentData: createUserAgentData(host.userAgent || 'PageCore/0.1'),
          language: 'en-US',
          languages: ['en-US', 'en'],
          onLine: true,
          hardwareConcurrency: 1,
          javaEnabled: () => false,
          mediaDevices: {
            getSupportedConstraints: () => ({})
          }
        };
        global.Intl = createIntlFallback(global.Intl);
        global.location = locationFromURL(host.baseURL || '');
        Object.defineProperty(global, 'isSecureContext', {
          configurable: true,
          get() {
            const protocol = (global.location && global.location.protocol || '').toLowerCase();
            if (protocol === 'https:' || protocol === 'wss:' || protocol === 'file:') return true;
            const hostname = (global.location && global.location.hostname || '').toLowerCase();
            return hostname === 'localhost' || hostname === '127.0.0.1'
              || hostname === '::1' || hostname.endsWith('.localhost');
          }
        });
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
        const defaultViewport = { width: 1280, height: 720, deviceScaleFactor: 1 };
        function currentViewport() {
          try {
            return bridge.viewport();
          } catch (_viewportError) {
            return defaultViewport;
          }
        }
        Object.defineProperty(global, 'innerWidth', { configurable: true, get: () => currentViewport().width });
        Object.defineProperty(global, 'innerHeight', { configurable: true, get: () => currentViewport().height });
        Object.defineProperty(global, 'outerWidth', { configurable: true, get: () => currentViewport().width });
        Object.defineProperty(global, 'outerHeight', { configurable: true, get: () => currentViewport().height });
        Object.defineProperty(global, 'devicePixelRatio', {
          configurable: true,
          get: () => currentViewport().deviceScaleFactor
        });
        global.screen = {
          get width() { return currentViewport().width; },
          get height() { return currentViewport().height; },
          get availWidth() { return currentViewport().width; },
          get availHeight() { return currentViewport().height; },
          colorDepth: 24,
          pixelDepth: 24,
          orientation: {
            type: 'landscape-primary',
            angle: 0
          }
        };
        global.screenX = 0;
        global.screenY = 0;
        // Delegates to document.documentElement's virtual scroll model (see
        // Element.scrollTop in 30_dom.js): matches real browsers, where
        // window.scrollY === document.documentElement.scrollTop in standards mode.
        Object.defineProperty(global, 'scrollX', { configurable: true, get: () => document.documentElement.scrollLeft });
        Object.defineProperty(global, 'scrollY', { configurable: true, get: () => document.documentElement.scrollTop });
        Object.defineProperty(global, 'pageXOffset', { configurable: true, get: () => document.documentElement.scrollLeft });
        Object.defineProperty(global, 'pageYOffset', { configurable: true, get: () => document.documentElement.scrollTop });
        global.scrollTo = (...args) => document.documentElement.scrollTo(...args);
        global.scrollBy = (...args) => document.documentElement.scrollBy(...args);
        global.scroll = (...args) => document.documentElement.scroll(...args);
        global.open = () => null;
        global.close = () => {};
        global.postMessage = (message) => {
          global.dispatchEvent(new MessageEvent('message', { data: message, origin: global.location.origin, source: global }));
        };

        function consoleArgument(value) {
          return formatErrorForLog(value);
        }

        global.console = {
          log: (...args) => host.log('log', ...args.map(consoleArgument)),
          info: (...args) => host.log('info', ...args.map(consoleArgument)),
          warn: (...args) => host.log('warn', ...args.map(consoleArgument)),
          error: (...args) => host.log('error', ...args.map(consoleArgument)),
          debug: (...args) => host.log('debug', ...args.map(consoleArgument))
        };

        global.setTimeout = setTimeoutShim;
        global.clearTimeout = clearTask;
        global.setInterval = setIntervalShim;
        global.clearInterval = clearTask;
        global.requestAnimationFrame = requestAnimationFrameShim;
        global.cancelAnimationFrame = cancelAnimationFrameShim;
        global.queueMicrotask = (callback) => Promise.resolve().then(callback);
        global.requestIdleCallback = requestIdleCallbackShim;
        global.cancelIdleCallback = clearTask;
        global.getComputedStyle = (element) => computedStyleFor(element);

        // Many libraries (jQuery/Sizzle, Modernizr) feature-detect a "real" browser
        // by stringifying selector/matching methods and testing for "[native code]"
        // (Sizzle: support.qsa = /^[^{]+\{\s*\[native \w/.test(qsa)). Our page-facing
        // DOM methods are plain ES class methods whose toString() returns JS source,
        // so that probe fails and Sizzle disables its native qSA path, forcing every
        // selector through its legacy engine (which throws on modern selectors).
        // Give the probed methods a native-looking toString so feature detection
        // routes selectors back through our querySelectorAll. Anti-spoof checks via
        // Function.prototype.toString.call(fn) still see the truth — acceptable.
        function markNative(fn, name) {
          if (typeof fn !== 'function') return;
          const rendered = `function ${name || fn.name || ''}() {\n    [native code]\n}`;
          try { defineValue(fn, 'toString', function toString() { return rendered; }); } catch (_error) {}
        }
        function markNativeMethods(proto, names) {
          if (!proto) return;
          for (const name of names) if (typeof proto[name] === 'function') markNative(proto[name], name);
        }

        markNativeMethods(Element.prototype, ['querySelector', 'querySelectorAll', 'matches', 'closest', 'getElementsByTagName', 'getElementsByClassName']);
        markNativeMethods(Document.prototype, ['querySelector', 'querySelectorAll', 'getElementById', 'getElementsByTagName', 'getElementsByClassName', 'getElementsByName']);
        markNativeMethods(Node.prototype, ['contains']);
        markNativeMethods(EventTarget.prototype, ['addEventListener', 'removeEventListener']);
        markNative(global.getComputedStyle, 'getComputedStyle');

        // Vendor matches aliases many libs feature-detect (same fn object → already native).
        if (typeof Element.prototype.matches === 'function') {
          for (const alias of ['webkitMatchesSelector', 'msMatchesSelector', 'mozMatchesSelector']) {
            if (!Element.prototype[alias]) defineValue(Element.prototype, alias, Element.prototype.matches);
          }
        }

        global.matchMedia = makeMediaQueryList;
        global.performance = {
          now: performanceNow,
          timeOrigin: Date.now(),
          mark() {},
          measure() {},
          getEntriesByName: () => [],
          getEntriesByType: () => []
        };
        global.Image = function(width = undefined, height = undefined) {
          const image = document.createElement('img');
          if (width !== undefined) image.width = Number(width);
          if (height !== undefined) image.height = Number(height);
          image.complete = false;
          image.decode = () => image.getAttribute('src') ? Promise.resolve() : Promise.reject(new DOMException('The source image cannot be decoded.', 'EncodingError'));
          return image;
        };
        global.Audio = Audio;
        global.Option = Option;
        global.fetch = (input, init = {}) => {
          const request = new Request(input, init);
          const signal = request.signal;
          if (signal && signal.aborted) {
            return Promise.reject(signal.reason || new DOMException('The operation was aborted.', 'AbortError'));
          }
          activityBegin('xhr-fetch');
          return new Promise((resolve, reject) => {
            let loadId = null;
            let settled = false;
            let onAbort = null;
            const settle = (settleFn, value) => {
              if (settled) return;
              settled = true;
              // Detach the abort listener so a long-lived reused signal does
              // not accumulate one retained closure per completed fetch.
              if (onAbort && typeof signal.removeEventListener === 'function') {
                signal.removeEventListener('abort', onAbort);
              }
              activityEnd('xhr-fetch');
              settleFn(value);
            };
            if (signal && typeof signal.addEventListener === 'function') {
              onAbort = () => {
                // Abort mid-transfer: cancel the host load (its completion is
                // then dropped) and reject immediately.
                if (loadId != null) cancelResourceLoad(loadId);
                settle(reject, signal.reason || new DOMException('The operation was aborted.', 'AbortError'));
              };
              signal.addEventListener('abort', onAbort);
            }
            loadHostResourceAsync(request.url, 'other', {
              method: request.method,
              body: bodyText(request.body, request.headers),
              headers: request.headers,
              credentials: request.credentials,
              referrer: request.referrer,
              onStarted: (id) => { loadId = id; }
            }).then(
              (loaded) => settle(resolve, new Response(loaded.body || '', {
                status: loaded.status === undefined ? 200 : Number(loaded.status),
                statusText: loaded.statusText === undefined ? '' : String(loaded.statusText),
                headers: responseHeadersFromHost(loaded),
                url: loaded.url || request.url,
                redirected: Number(loaded.redirectCount || 0) > 0
              })),
              (error) => settle(reject, error));
          });
        };
        installWindowNamedPropertiesFromTree(document);
        global.__pagecore_install_wpt_hook = installWptHook;
        installWptHook();
        global.__pagecore_fireDOMContentLoaded = () => {
          if (ctx.documentReadyState === 'loading') setDocumentReadyState('interactive');
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
          DOCUMENT_FRAGMENT_NODE: 11,
          DOCUMENT_POSITION_DISCONNECTED: 1,
          DOCUMENT_POSITION_PRECEDING: 2,
          DOCUMENT_POSITION_FOLLOWING: 4,
          DOCUMENT_POSITION_CONTAINS: 8,
          DOCUMENT_POSITION_CONTAINED_BY: 16,
          DOCUMENT_POSITION_IMPLEMENTATION_SPECIFIC: 32
        })) {
          defineValue(Node, name, value, true);
          defineValue(Node.prototype, name, value, true);
        }

        global.__pagecore_queue_task = queuePageTask;
        global.__pagecore_run_task = runTask;
        global.__pagecore_run_raf_callbacks = runAnimationFrames;
        global.__pagecore_resource_load_complete = completeResourceLoad;
        global.__pagecore_deliver_mutation_observers = deliverMutationObservers;

      return {};
    }
  };
});
