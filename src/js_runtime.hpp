#pragma once

#include "pagecore/dom.hpp"
#include "pagecore/page.hpp"
#include "pagecore/render.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

extern "C" {
#include <quickjs.h>
}

namespace pagecore {

class ResourceLoader;
struct ResourceResponse;

class JsRuntime {
public:
    using ComputedStyleResolver = std::function<std::optional<ComputedStyle>(NodeId)>;
    using ComputedStylePropertyResolver = std::function<std::optional<std::string>(NodeId, std::string_view)>;
    using ElementGeometryResolver = std::function<std::optional<ElementGeometry>(NodeId)>;
    using ViewportResolver = std::function<Viewport()>;

    JsRuntime(DomDocument& document, LoadOptions options, std::shared_ptr<ResourceLoader> loader);
    ~JsRuntime();

    JsRuntime(const JsRuntime&) = delete;
    JsRuntime& operator=(const JsRuntime&) = delete;

    void install();
    void execute(std::string_view script, std::string_view filename = "<eval>");
    void execute_module(std::string_view script, std::string_view filename);
    std::string evaluate(std::string_view script, std::string_view filename = "<eval>");
    void run_until_idle();

    DomDocument& document();
    ResourceResponse load_resource(
        std::string_view url,
        std::string_view kind,
        std::string method = "GET",
        std::string body = {},
        std::vector<std::pair<std::string, std::string>> headers = {});
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

private:
    JSRuntime* runtime_ = nullptr;
    JSContext* context_ = nullptr;
    DomDocument* document_ = nullptr;
    LoadOptions options_;
    std::shared_ptr<ResourceLoader> loader_;
    ComputedStyleResolver computed_style_resolver_;
    ComputedStylePropertyResolver computed_style_property_resolver_;
    ElementGeometryResolver element_geometry_resolver_;
    ViewportResolver viewport_resolver_;
    std::chrono::steady_clock::time_point deadline_{};
    bool deadline_active_ = false;

    static char* normalize_module(JSContext* ctx, const char* module_base_name, const char* module_name, void* opaque);
    static JSModuleDef* load_module(JSContext* ctx, const char* module_name, void* opaque);

    void start_deadline();
    void clear_deadline();
    void drain_jobs();
    void check_exception(JSValue value, std::string_view source_name = {});
};

} // namespace pagecore
