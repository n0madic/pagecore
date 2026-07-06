#pragma once

#include "pagecore/dom.hpp"
#include "pagecore/render.hpp"

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace pagecore {

enum class JsResourceLoadPolicy {
    Allow,
    SameOriginOnly,
    BlockAll,
};

enum class WaitUntil {
    Load,
    NetworkIdle,
    DomStable,
    Ready,
};

struct PageReadinessOptions {
    WaitUntil wait_until = WaitUntil::Ready;
    std::chrono::milliseconds wait_time{15000};
    std::chrono::milliseconds stable_window{350};
};

// Which input the layout engine receives when (re)building the styled
// document: the serialized-HTML round trip (the engine re-parses the markup)
// or a tree built directly from the live DOM (LayoutEngine::load_dom, no
// serialize/re-parse). Engines without direct-DOM support always fall back to
// the serialized path.
enum class LayoutTreeInput {
    SerializedHtml,
    DirectDom,
};

struct LoadOptions {
    bool enable_js = true;
    LayoutTreeInput layout_tree_input = LayoutTreeInput::DirectDom;
    std::chrono::milliseconds wait_time{15000};
    WaitUntil wait_until = WaitUntil::Ready;
    std::chrono::milliseconds stable_window{350};
    std::chrono::milliseconds js_timeout{30000};
    std::size_t js_memory_limit_bytes = 256 * 1024 * 1024;
    JsResourceLoadPolicy js_resource_load_policy = JsResourceLoadPolicy::Allow;
    std::optional<std::size_t> max_js_resource_loads;
    std::optional<std::size_t> max_js_resource_bytes;
    std::optional<std::chrono::milliseconds> max_js_resource_time;
    std::string user_agent = "PageCore/0.1";
    std::string base_url;
    std::function<void(std::string_view severity, std::string_view message)> console_log;
    PerfTraceCallback perf_trace;
};

class ResourceLoader;

class Page {
public:
    explicit Page(LoadOptions options = {});
    ~Page();

    Page(const Page&) = delete;
    Page& operator=(const Page&) = delete;

    void set_resource_loader(std::shared_ptr<ResourceLoader> loader);
    void set_layout_engine_factory(std::shared_ptr<LayoutEngineFactory> factory);

    void load_html(std::string_view html, std::string base_url = {});
    void load_url(std::string_view url);
    std::string eval(std::string_view script);
    void run_until_idle();
    bool run_until_ready(PageReadinessOptions options = {});

    DomDocument& document();
    const DomDocument& document() const;

    std::string serialize_html() const;
    std::unique_ptr<LayoutEngine> layout(RenderOptions options = {}) const;
    DisplayList display_list(RenderOptions options = {}) const;
    RenderedImage render(RenderOptions options = {}) const;
    RenderedImage render(RasterBackend& backend, RenderOptions options = {}) const;
    std::optional<std::string> text_content(std::string_view selector);
    std::optional<std::string> outer_html(std::string_view selector);
    std::optional<ComputedStyle> computed_style(NodeId node) const;
    std::optional<std::string> computed_style_property(NodeId node, std::string_view property) const;
    std::optional<ElementGeometry> element_geometry(NodeId node) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pagecore
