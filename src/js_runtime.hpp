#pragma once

#include "pagecore/dom.hpp"
#include "pagecore/page.hpp"
#include "pagecore/render.hpp"
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

class ResourceLoader;
struct ResourceResponse;
class CookieJar;

class JsRuntime {
public:
    using ComputedStyleResolver = std::function<std::optional<ComputedStyle>(NodeId)>;
    using ComputedStylePropertyResolver = std::function<std::optional<std::string>(NodeId, std::string_view)>;
    using ElementGeometryResolver = std::function<std::optional<ElementGeometry>(NodeId)>;
    using ViewportResolver = std::function<Viewport()>;

    JsRuntime(
        DomDocument& document,
        LoadOptions options,
        std::shared_ptr<ResourceLoader> loader,
        CookieJar* cookie_jar = nullptr);
    ~JsRuntime();

    JsRuntime(const JsRuntime&) = delete;
    JsRuntime& operator=(const JsRuntime&) = delete;

    void install();
    void execute(std::string_view script, std::string_view filename = "<eval>");
    void execute_module(std::string_view script, std::string_view filename);
    std::string evaluate(std::string_view script, std::string_view filename = "<eval>");
    void run_until_idle();
    bool run_until_ready(PageReadinessOptions options);

    DomDocument& document();
    PageActivityTracker& activity_tracker();
    ResourceResponse load_resource(
        std::string_view url,
        std::string_view kind,
        std::string method = "GET",
        std::string body = {},
        std::vector<std::pair<std::string, std::string>> headers = {},
        std::string credentials = "same-origin",
        std::string referrer = "about:client");
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

    struct EventLoopSnapshot {
        std::chrono::milliseconds now{0};
        std::size_t relevant_count = 0;
        std::optional<std::chrono::milliseconds> next_task_delay;
    };

    JSRuntime* runtime_ = nullptr;
    JSContext* context_ = nullptr;
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

    static char* normalize_module(JSContext* ctx, const char* module_base_name, const char* module_name, void* opaque);
    static JSModuleDef* load_module(JSContext* ctx, const char* module_name, void* opaque);

    void start_deadline();
    void clear_deadline();
    int drain_jobs();
    int deliver_mutation_observers();
    void run_microtask_checkpoint();
    int run_microtask_checkpoint_logged();
    int run_event_loop_step(std::chrono::milliseconds advance);
    EventLoopSnapshot event_loop_snapshot(std::chrono::milliseconds horizon);
    bool readiness_satisfied(WaitUntil wait_until, std::chrono::milliseconds stable_window) const;
    void check_exception(JSValue value, std::string_view source_name = {});
    void emit_script_perf(std::string_view name, std::chrono::steady_clock::time_point start, std::uint64_t count);
    std::optional<std::string> js_resource_budget_block_reason() const;
    void record_js_resource_load(long long elapsed_us, std::size_t bytes);
};

} // namespace pagecore
