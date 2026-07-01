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

struct LoadOptions {
    bool enable_js = true;
    std::chrono::milliseconds wait_time{15000};
    std::chrono::milliseconds js_timeout{30000};
    std::size_t js_memory_limit_bytes = 256 * 1024 * 1024;
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
    std::optional<ElementGeometry> element_geometry(NodeId node) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pagecore
