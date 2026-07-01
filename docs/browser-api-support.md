# Browser API Support

PageCore is a headless/scraping-oriented engine. Its browser API surface is
therefore intentionally smaller than a general-purpose browser. The goal is to
load modern pages far enough to extract final DOM/HTML/text or render a
screenshot, without advertising APIs that would send libraries down code paths
PageCore cannot execute.

This document describes the page-visible API surface installed by
`src/dom_shim/90_install.js` and related behavior implemented in
`src/dom_shim/`, `src/page.cpp`, and the C++ resource/rendering pipeline.

## Status Policy

- `supported`: The API is page-visible, has a useful implementation for
  PageCore's headless use cases, and is covered by focused tests. This does not
  mean full browser or WPT compatibility.
- `partial`: The API is page-visible and intentionally usable, but has known
  limitations. Feature detection may treat it as available.
- `absent`: The API is intentionally not installed. Page scripts should take a
  fallback path when using feature detection.

When in doubt, prefer `absent` over a no-op or always-failing stub. A false
negative in feature detection is usually safer for scraping than a false
positive.

## Supported

| API surface | Current support | Primary coverage |
| --- | --- | --- |
| `window`, `self`, `top`, `parent`, `Window` identity | Single-window identity, iframe window identity for the lightweight iframe model, and stable `Window.prototype` event methods. | `test_browser_like_web_api_shims`, `test_global_event_listener_aliases_bind_to_window` |
| `DOMException` | DOM-style error object with legacy constants. | `pagecore_dom_shim_unit_tests` |
| `EventTarget`, `Event`, `CustomEvent`, `MessageEvent` | Listener registration/removal, `once`, `signal`, capture/bubble phases, object listeners, handler exceptions reported without aborting later listeners. | `test_event_options_bubbling_and_wpt_driver_shim`, `test_event_capture_bubble_phases`, `pagecore_dom_shim_unit_tests` |
| `AbortController`, `AbortSignal` | Listener cleanup through `{ signal }`; signal objects are exposed. | `pagecore_dom_shim_unit_tests` |
| DOM node model: `Node`, `Text`, `Comment`, `Attr`, `Element`, `Document`, `DocumentFragment` | Lexbor-backed tree, attributes, text, fragments, cloning, sibling/child traversal, stale wrapper invalidation. | `test_tree_operations_and_clone`, `test_inner_html_invalidates_stale_wrappers`, `test_deep_dom_traversal_is_iterative` |
| Selectors and traversal | `querySelector`, `querySelectorAll`, `matches`, `closest`, `getElementsBy*`, `TreeWalker`, `NodeIterator`, with compatibility fallbacks for escaped colon classes and `:target`. | `test_query_selector_cache_returns_all_and_first`, `test_escaped_colon_class_selector_fallback`, `test_target_pseudo_class_selector_fallback`, `pagecore_dom_shim_unit_tests` |
| DOM mutation helpers | `innerHTML`, `outerHTML`, `insertAdjacent*`, `append`, `prepend`, `before`, `after`, `replaceWith`, `remove`, fragment parsing, `document.write/open/close`. | `test_inner_html_fragment_parsing`, `test_document_write_fragment_insertion`, `test_document_write_external_script_and_open_close` |
| `MutationObserver` | Child-list, attribute, and character-data records, `attributeFilter`, and old-value handling. | `test_dom_fragment_range_serializer_and_mutation_observer`, `test_text_content_mutation_observer_records_nodes`, `test_mutation_observer_old_value` |
| `DOMTokenList`, `dataset` | Class-list and dataset facades backed by Lexbor attributes. | `test_dataset_attributes_and_cached_facades` |
| `URL`, `URLSearchParams` | URL parsing/resolution and query parameter operations used by modern JS bundles. | `test_browser_like_web_api_shims`, `test_web_shim_crypto_url_input`, `pagecore_dom_shim_unit_tests` |
| `Headers` | Header normalization, append/set/get/iteration. | `pagecore_dom_shim_unit_tests` |
| `TextEncoder`, `TextDecoder` | UTF-8 encode/decode helpers. | `test_text_encoder_decoder_utf8_shims`, `pagecore_dom_shim_unit_tests` |
| `btoa`, `atob` | Browser-compatible Latin-1 base64 behavior backed by the host C++ base64 bridge. | `test_browser_like_web_api_shims`, `pagecore_dom_shim_unit_tests` |
| `crypto.getRandomValues`, `crypto.randomUUID` | Host CSPRNG-backed random values and UUID v4 formatting. | `test_message_channel_and_crypto_shims`, `test_web_shim_crypto_url_input` |
| Basic timers | `setTimeout`, `clearTimeout`, `setInterval`, `clearInterval`, deterministic idle draining, and logged callback errors. | `test_timers_and_events`, `test_timer_wait_budget`, `pagecore_dom_shim_unit_tests` |
| `localStorage`, `sessionStorage`, `Storage` | In-memory storage for a page runtime. | `test_browser_like_web_api_shims` |
| `document.cookie`, `document.domain` | Basic cookie string state and domain property behavior. | `test_document_domain_and_cookie_jar` |
| `MessageChannel`, `MessagePort` | Basic in-process port messaging. | `test_message_channel_and_crypto_shims` |
| `XMLSerializer`, `DOMParser` | Basic HTML serialization/parsing helpers. | `test_dom_fragment_range_serializer_and_mutation_observer` |
| Form basics | `HTMLFormElement`, common form controls, `ValidityState`, `checkValidity`, `reportValidity`, `FormData` from forms. | `test_html_element_specific_constructors`, `pagecore_dom_shim_unit_tests` |
| `CSS.escape` | CSS identifier escaping. | `pagecore_dom_shim_unit_tests` |

## Partial

| API surface | Current support and limitations | Primary coverage |
| --- | --- | --- |
| Static classic scripts | Standard JavaScript MIME types execute in document order. External scripts are fetched through `ResourceLoader::load_all` and still execute in document order. Non-standard script types stay inert. `async`/`defer` timing is not browser-complete. | `test_external_script_via_resource_loader`, `test_external_scripts_load_in_document_order`, `test_non_javascript_script_types_are_not_executed` |
| Dynamic classic scripts | DOM-inserted classic scripts load through `ResourceLoader`, set `document.currentScript`, dispatch `load`/`error`, and do not rerun when moved. Dynamic module scripts are not implemented. | `test_dynamic_script_insertion_executes_classic_scripts` |
| Module scripts | Static external and inline module scripts can import relative dependencies and expose `import.meta.url`; `document.currentScript` is `null`. Import maps, top-level await, and dynamic module script insertion are not complete browser implementations. | `test_module_script_imports_relative_dependencies`, `test_inline_module_uses_document_url_for_relative_imports` |
| Page lifecycle | `DOMContentLoaded`, `readystatechange`, and `load` are dispatched after script execution and before idle draining. Ordering against all browser task sources, deferred/module scripts, and pending stylesheets is simplified. | `test_browser_like_web_api_shims`, `test_document_lifecycle_ignores_ready_state_overrides` |
| `fetch`, `Request`, `Response` | Uses the host `ResourceLoader`, passes method/body/headers, returns status/body/basic headers. No real CORS model, streaming body, redirect modes, credential enforcement, or fetch `AbortSignal` cancellation. | `test_request_response_fetch_object_shims`, `test_xhr_and_fetch_load_through_resource_loader` |
| `XMLHttpRequest` | Uses `ResourceLoader`, supports method/body/headers, response text/json, ready-state transitions, abort/error/load events. Progress events, timeout behavior, upload progress, streaming, and full response-header fidelity are limited. | `test_xhr_and_fetch_load_through_resource_loader`, `test_xhr_event_handler_exceptions_are_reported` |
| `Blob`, `File` | Text/arrayBuffer/slice and file metadata are available. Object URLs and `FileReader` are absent. | `test_request_response_fetch_object_shims` |
| Streams | Default readable/writable/transform flows are implemented; BYOB mode is explicitly rejected even though BYOB-related constructors are installed. | `test_streams_writable_controller_and_tee`, `pagecore_dom_shim_unit_tests` |
| `getComputedStyle`, `CSSStyleDeclaration` | Backed by litehtml's cascade over the real Lexbor DOM. Single-property reads use a lazy property bridge cached by layout mutation version; full enumeration still materializes the style object. Returns computed values, not used values; percentages and `auto` may remain authored. `opacity` reads as `1`; CSS custom properties and `var()` are not resolved. CSSOM-only states that litehtml cannot see do not affect rendering. | `test_get_computed_style_reads_display_from_stylesheets`, `test_get_computed_style_matches_real_cascade_for_cases_js_engine_got_wrong` |
| CSSOM: `CSSRule`, `CSSStyleRule`, `CSSMediaRule`, `CSSStyleSheet`, `document.styleSheets` | Basic rule/declaration parsing, insertion, deletion, and stylesheet discovery. `disabled`, `adoptedStyleSheets`, and shadow-tree styles are JS-visible but do not affect litehtml rendering or `getComputedStyle`. | `test_cssom_stylesheets_rules_declarations_and_cascade`, `test_cssom_dynamic_sheets_media_disabled_and_adopted` |
| `CSS.supports` | Basic declaration-shaped checks only; complex condition parsing is not a full CSS parser. | `pagecore_dom_shim_unit_tests` |
| `queueMicrotask` | Promise-based microtask scheduling. Ordering is not documented as a full browser event-loop model. | Covered indirectly by JS runtime and async tests. |
| Geometry APIs | `getBoundingClientRect`, `getClientRects`, `offset*`, `client*`, and `offsetParent` read from litehtml layout where practical. Heavy pages can enter bounded geometry mode and return cached or zero geometry to avoid pathological scraping stalls. | `test_geometry_box_model_apis_reflect_real_layout`, `test_element_geometry_bounded_mode_returns_cached_geometry` |
| Scroll geometry | `scrollWidth`/`scrollHeight` approximate client size. `scrollTop`/`scrollLeft` read `0` and writes are ignored. `window.scroll*` APIs are no-ops. | Documented in `README.md`; covered indirectly by geometry tests. |
| `Range`, `Selection`, `DOMRect` | Useful range boundaries, contextual fragments, selection state, and DOMRect values. `Range.getBoundingClientRect()` returns an empty rect and `Range.getClientRects()` returns `[]`. | `test_dom_fragment_range_serializer_and_mutation_observer` |
| `customElements`, `CustomElementRegistry` | Define/get/whenDefined, construction, upgrade, connected callback, and observed attributes for common custom-element bundles. Browser lifecycle ordering is simplified. | `test_custom_elements_registry_shim`, `test_custom_elements_with_private_fields_construct_instances` |
| Shadow DOM: `attachShadow`, `ShadowRoot`, `HTMLSlotElement` | JS-only shadow tree model. Nodes are not attached to Lexbor's rendered DOM, slot assignment is empty, and shadow styling does not affect `getComputedStyle` or rendering. | `test_shadow_root_and_element_internals_shims` |
| `ElementInternals` | Minimal object for custom-element compatibility. | `test_shadow_root_and_element_internals_shims` |
| HTML element constructors | Many `HTML*Element` constructors are installed so `createElement()` and `instanceof` checks work. Only selected element-specific behavior is implemented. | `test_html_element_specific_constructors` |
| `HTMLCanvasElement` | Constructor is installed, but `getContext()` returns `null` and `toDataURL()` returns `data:,`. This is intentional so canvas feature detection can fail cleanly at context acquisition. | Source-level behavior in `src/dom_shim/30_dom.js` |
| `Image` / `HTMLImageElement` | `new Image()` creates an `img`; width/height attributes and `decode()` are minimal. Image network loading, `naturalWidth`, and `onload/onerror` image lifecycle are not browser-complete. Rendering image decode is handled by the C++ image pipeline, not this DOM object. | Rendering image tests cover the C++ pipeline; DOM `Image` is source-level partial support. |
| Media elements: `HTMLAudioElement`, `HTMLVideoElement`, `Audio` | Element constructors, `play()`, `pause()`, `load()`, `currentTime`, `muted`, and `canPlayType()` stubs exist for compatibility. No real media playback, buffering, metadata, or events. | `pagecore_dom_shim_unit_tests` |
| `HTMLIFrameElement` | Provides a lightweight detached `contentWindow`/`contentDocument` and local `postMessage`. It does not load frame documents from the network or create an isolated browsing context. | `test_browser_like_web_api_shims` |
| `window.postMessage` | Dispatches a same-window `message` event. Cross-origin/window targeting semantics are not implemented. | `test_browser_like_web_api_shims` |
| `navigator` | Exposes `userAgent`, language fields, `onLine`, `hardwareConcurrency`, `javaEnabled()`, and `mediaDevices.getSupportedConstraints()`. No service worker or real media devices. | `test_browser_like_web_api_shims` |
| `location`, `document.location`, `history` | URL object helpers and minimal `pushState`/`replaceState` mutation of `location`. No navigation, reload, session history stack, `popstate`, or `hashchange` model. | `test_document_location_aliases_window_location`, `test_browser_like_web_api_shims` |
| Viewport/screen globals | `innerWidth`, `innerHeight`, `outerWidth`, `outerHeight`, `devicePixelRatio`, and `screen.*` reflect the most recent render viewport. No visual viewport, actual scrolling, or multi-screen model. | `test_window_viewport_reflects_last_render_options`, `test_browser_like_web_api_shims` |
| `matchMedia` | Returns a media-query-list-like object and stores listeners, but does not evaluate full CSS media query truth or fire viewport-change events. | `test_browser_like_web_api_shims` |
| `requestAnimationFrame` | Implemented as a deterministic 16 ms timer callback. There is no real rendering frame pipeline. | Source-level behavior in `src/dom_shim/40_web.js`; timer tests cover scheduler behavior. |
| `requestIdleCallback` | Implemented as a timer callback with `didTimeout:false` and `timeRemaining() === 0`. | Source-level behavior in `src/dom_shim/40_web.js` |
| `performance` | `performance.now()` is tied to the shim timer clock; `timeOrigin` is set. Marks, measures, and entries are no-op/empty. | Source-level behavior in `src/dom_shim/90_install.js` |
| `Intl` fallback | Missing Intl constructors are filled with simple fallbacks for common library compatibility. Locale formatting is not browser/ICU-complete. | `pagecore_dom_shim_unit_tests` |
| SVG DOM constructors | Common `SVG*Element` constructors exist for `createElementNS()`/`instanceof` compatibility. SVG rasterization support is handled separately by the C++ image decoder and can be disabled at build time. | `test_create_element_ns_and_template_content_clone`, image decoder/render tests |

## Absent

These APIs are intentionally not installed in the page-visible global object.
Feature detection should fail and libraries should use fallback code paths.

| API surface | Reason |
| --- | --- |
| `Worker`, `SharedWorker`, `navigator.serviceWorker`, Service Worker APIs | No worker runtime, worker event loop, script isolation, cache integration, or service-worker-controlled fetch pipeline. |
| `IntersectionObserver`, `ResizeObserver`, `PerformanceObserver` | No browser observation loop. Geometry can be read synchronously through partial geometry APIs instead. |
| `WebSocket`, `EventSource` | No persistent browser network channel model. Add only if scraping targets require live push data. |
| IndexedDB | No transactional persistent browser database. |
| Cache API | No Service Worker/CacheStorage model. |
| `BroadcastChannel`, `MessageChannel` alternatives across browsing contexts | Only local `MessageChannel` is implemented. |
| `FileReader`, `URL.createObjectURL`, `URL.revokeObjectURL` | Blob URLs and asynchronous file readers are not implemented. |
| Canvas rendering contexts: `CanvasRenderingContext2D`, `OffscreenCanvas`, WebGL contexts | `HTMLCanvasElement.getContext()` returns `null`; no canvas drawing backend is exposed to JS. |
| `document.elementFromPoint` | No hit-testing API is installed. |
| Real media capture/playback APIs: `getUserMedia`, `MediaStream`, WebRTC, WebAudio | Not useful for the default scraping pipeline and would require large runtime subsystems. |
| Permissions, Notifications, Clipboard, Geolocation, Payment Request | Browser integration APIs are outside the scraping-oriented runtime. |
| Touch-specific event constructors such as `TouchEvent` | Pointer/mouse/keyboard constructor shims exist; touch input is not modeled. |
| CSP and Trusted Types enforcement | PageCore does not implement a browser security policy engine. |
| Visual viewport and real scroll state | Viewport size is exposed, but scroll is not modeled as mutable layout state. |

## Maintenance Rules

- Any change to `src/dom_shim/90_install.js` that adds or removes a
  page-visible global must update this document.
- Any change that turns a `partial` API into a more complete implementation must
  add or update tests and move the entry only when the documented behavior is
  covered.
- Any newly added compatibility stub must be classified deliberately. If the API
  cannot provide useful behavior for headless scraping, leave it absent.
- Keep `README.md` high-level. Use this document for detailed API status and
  limitations.
