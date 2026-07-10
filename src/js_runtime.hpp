#pragma once

#include "pagecore/dom.hpp"
#include "pagecore/page.hpp"
#include "pagecore/render.hpp"
#include "pagecore/resource_loader.hpp"
#include "async_loader.hpp"
#include "cookie_jar.hpp"
#include "event_loop.hpp"
#include "page_activity_tracker.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

extern "C" {
#include <quickjs.h>
}

namespace pagecore {

class CookieJar;

class JsRuntime {
public:
    using ComputedStyleResolver = std::function<std::optional<ComputedStyle>(NodeId)>;
    using ComputedStylePropertyResolver = std::function<std::optional<std::string>(NodeId, std::string_view)>;
    using ElementGeometryResolver = std::function<std::optional<ElementGeometry>(NodeId)>;
    using ViewportResolver = std::function<Viewport()>;

    // `loader` is the blocking loader used for sync XHR / module loads (a
    // CachingResourceLoader in the Page pipeline). `raw_loader` is the
    // unwrapped inner loader: when it is a CurlResourceLoader, JS-initiated
    // loads go through the truly asynchronous curl_multi engine; any other
    // loader is adapted by blocking one queued task per transfer.
    JsRuntime(
        DomDocument& document,
        LoadOptions options,
        std::shared_ptr<ResourceLoader> loader,
        CookieJar* cookie_jar = nullptr,
        std::shared_ptr<ResourceLoader> raw_loader = nullptr);
    ~JsRuntime();

    JsRuntime(const JsRuntime&) = delete;
    JsRuntime& operator=(const JsRuntime&) = delete;

    void install();
    void execute(std::string_view script, std::string_view filename = "<eval>");
    void execute_module(std::string_view script, std::string_view filename);
    // Sets an aggregate wall-clock deadline that every per-call execution deadline
    // is additionally clamped to (see start_deadline). std::nullopt clears it.
    // Used by the loader to bound the entire <script> sequence, not just one script.
    void set_load_deadline(std::optional<std::chrono::steady_clock::time_point> deadline);
    // True once a load deadline has been set and reached; the script loop uses this
    // to stop launching further scripts.
    bool load_deadline_passed() const;
    std::string evaluate(std::string_view script, std::string_view filename = "<eval>");
    void run_until_idle();
    bool run_until_ready(PageReadinessOptions options);

    DomDocument& document();
    PageActivityTracker& activity_tracker();
    EventLoop& event_loop();
    // Executes the JS callback registered for a queued task id (via the
    // __pagecore_run_task global); exceptions are logged, never propagated.
    void run_queued_task(std::uint64_t id);
    ResourceResponse load_resource(
        std::string_view url,
        std::string_view kind,
        std::string method = "GET",
        std::string body = {},
        std::vector<std::pair<std::string, std::string>> headers = {},
        std::string credentials = "same-origin",
        std::string referrer = "about:client");
    // Asynchronous counterpart of load_resource for fetch/XHR/dynamic scripts:
    // starts the transfer immediately; completion is delivered later through
    // the __pagecore_resource_load_complete(js_id, ...) global as a Networking
    // task. Throws synchronously on policy/budget violations.
    void start_resource_load(
        std::uint64_t js_id,
        std::string_view url,
        std::string_view kind,
        std::string method = "GET",
        std::string body = {},
        std::vector<std::pair<std::string, std::string>> headers = {},
        std::string credentials = "same-origin",
        std::string referrer = "about:client");
    void cancel_resource_load(std::uint64_t js_id);
    // Parser-discovered <script src> fetch: governed by the document policy
    // (max_document_script_loads), NOT the JS resource policy/budgets. Cookies
    // are attached at start and stored on completion; the completion callback
    // runs inside a queued Networking task.
    std::uint64_t start_document_script_load(ResourceRequest request, AsyncLoadCallback callback);
    // Pumps the event loop (one task per turn + microtask checkpoints) until
    // `condition` holds or `deadline` passes. Returns the final condition().
    bool run_event_loop_until(
        const std::function<bool()>& condition,
        std::chrono::steady_clock::time_point deadline);
    std::string document_cookie(std::string_view url) const;
    void set_document_cookie(std::string_view url, std::string_view cookie);
    void log_console(std::string_view severity, std::string_view message);
    bool is_timed_out() const;

    // Injected by Page::Impl so getComputedStyle() can read back litehtml's
    // cascade without JsRuntime depending on Page directly.
    void set_computed_style_resolver(ComputedStyleResolver resolver);
    std::optional<ComputedStyle> computed_style(NodeId node);
    void set_computed_style_property_resolver(ComputedStylePropertyResolver resolver);
    std::optional<std::string> computed_style_property(NodeId node, std::string_view property);

    // Injected by Page::Impl so getBoundingClientRect()/offsetWidth/etc. can
    // read back litehtml's box-model geometry without JsRuntime depending on
    // Page directly.
    void set_element_geometry_resolver(ElementGeometryResolver resolver);
    std::optional<ElementGeometry> element_geometry(NodeId node);

    // Injected by Page::Impl so window.innerWidth/innerHeight/etc. reflect
    // the most recently used render viewport.
    void set_viewport_resolver(ViewportResolver resolver);
    Viewport viewport();

    void record_dom_bridge_perf(std::string_view name, long long elapsed_us);
    void flush_dom_bridge_perf();

private:
    struct DomBridgePerfAggregate {
        long long elapsed_us = 0;
        std::uint64_t calls = 0;
    };

    // Everything a JS-initiated load needs across its async lifetime: the
    // policy-checked, cookie-annotated request plus what the completion side
    // needs to store cookies and account the load.
    struct JsResourceRequestContext {
        ResourceRequest request;
        CookieCredentials credentials = CookieCredentials::SameOrigin;
        // Request URL before redirects, used as the Set-Cookie source.
        std::string request_url;
    };

    JSRuntime* runtime_ = nullptr;
    JSContext* context_ = nullptr;
    // The page's real event loop (libuv). Owned by the runtime so its lifetime
    // matches the QuickJS context; shut down explicitly before the context is
    // freed (see ~JsRuntime).
    std::unique_ptr<EventLoop> event_loop_;
    // Async transfer engine for JS-initiated loads. Shut down (handles closed
    // and drained) before the event loop in ~JsRuntime.
    std::unique_ptr<AsyncResourceLoader> async_loader_;
    // Non-null only on the curl_multi path, where the JS resource cache is
    // probed on start and written on completion explicitly (the task-queue
    // adapter goes through the caching loader's blocking load() instead).
    std::shared_ptr<CachingResourceLoader> js_resource_cache_;
    // js-side load id -> async transfer id, for cancellation.
    std::unordered_map<std::uint64_t, std::uint64_t> pending_resource_loads_;
    DomDocument* document_ = nullptr;
    LoadOptions options_;
    std::shared_ptr<ResourceLoader> loader_;
    CookieJar* cookie_jar_ = nullptr;
    ComputedStyleResolver computed_style_resolver_;
    ComputedStylePropertyResolver computed_style_property_resolver_;
    ElementGeometryResolver element_geometry_resolver_;
    ViewportResolver viewport_resolver_;
    std::unordered_map<std::string, DomBridgePerfAggregate> dom_bridge_perf_;
    std::size_t js_resource_load_count_ = 0;
    std::size_t js_resource_load_bytes_ = 0;
    long long js_resource_load_elapsed_us_ = 0;
    PageActivityTracker activity_tracker_;
    std::chrono::steady_clock::time_point deadline_{};
    bool deadline_active_ = false;
    // Aggregate load deadline; each per-call deadline is clamped down to it.
    std::optional<std::chrono::steady_clock::time_point> load_deadline_;

    static char* normalize_module(JSContext* ctx, const char* module_base_name, const char* module_name, void* opaque);
    static JSModuleDef* load_module(JSContext* ctx, const char* module_name, void* opaque);

    void start_deadline();
    void clear_deadline();
    int drain_jobs();
    int deliver_mutation_observers();
    void run_microtask_checkpoint();
    int run_microtask_checkpoint_logged();
    // One event-loop turn: pump libuv, run at most one queued task, then a full
    // microtask checkpoint, then (if a frame is due) one rendering phase.
    // Returns 1 when anything ran, 0 when nothing was ready, -1 when a
    // checkpoint failed.
    int run_event_loop_turn();
    // Runs the due animation-frame callbacks (rendering phase). Returns how
    // many ids were dispatched.
    int run_animation_frames();
    bool readiness_satisfied(WaitUntil wait_until, std::chrono::milliseconds stable_window) const;
    void check_exception(JSValue value, std::string_view source_name = {});
    void emit_script_perf(std::string_view name, std::chrono::steady_clock::time_point start, std::uint64_t count);
    std::optional<std::string> js_resource_budget_block_reason() const;
    void record_js_resource_load(long long elapsed_us, std::size_t bytes);
    // Shared preamble of load_resource/start_resource_load: URL resolution,
    // policy + budget enforcement (throws), cookie header attachment.
    JsResourceRequestContext prepare_js_resource_request(
        std::string_view url,
        std::string_view kind,
        std::string method,
        std::string body,
        std::vector<std::pair<std::string, std::string>> headers,
        std::string credentials,
        std::string referrer);
    // Completion side of start_resource_load; runs inside a Networking task.
    void finish_resource_load(
        std::uint64_t js_id,
        const JsResourceRequestContext& context,
        AsyncLoadResult result,
        std::chrono::steady_clock::time_point started,
        bool store_in_cache);
};

} // namespace pagecore
