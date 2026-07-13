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
    // Aggregate wall-clock ceiling on all JavaScript executed during a single
    // load (the whole <script> sequence plus lifecycle/readiness), as opposed to
    // js_timeout which bounds each script individually. Without it, a page with K
    // scripts could consume up to K * js_timeout of CPU. std::nullopt disables it.
    std::optional<std::chrono::milliseconds> max_load_time{std::chrono::milliseconds{60000}};
    std::size_t js_memory_limit_bytes = 256 * 1024 * 1024;
    // Cumulative cap on DOM nodes scripts may create (createElement, cloneNode,
    // innerHTML, ...). Bounds native Lexbor node memory that js_memory_limit_bytes
    // does not cover. 0 disables it. See DomDocument::set_max_created_nodes.
    std::size_t max_dom_nodes = 5 * 1000 * 1000;
    JsResourceLoadPolicy js_resource_load_policy = JsResourceLoadPolicy::Allow;
    // Ceiling on the number of parser-discovered external <script src> fetched
    // during a load. Unlike max_js_resource_* (which bound script-INITIATED loads
    // — fetch/XHR/dynamic script), this bounds the document's own <script src>
    // fan-out, which is otherwise limited only by the per-request size cap. 0
    // disables it. Scripts beyond the cap are not fetched or executed.
    std::size_t max_document_script_loads = 1000;
    std::optional<std::size_t> max_js_resource_loads;
    std::optional<std::size_t> max_js_resource_bytes;
    std::optional<std::chrono::milliseconds> max_js_resource_time;
    std::string user_agent = "PageCore/0.1";
    std::string base_url;
    // Canonical WHATWG label (e.g. "UTF-8", "Shift_JIS") for the document's
    // character encoding, as resolved by decode_html_bytes() before parsing.
    // A boot-time literal seeding document.characterSet/charset, like base_url
    // seeds location.href -- fixed for the document's lifetime.
    std::string document_character_set = "UTF-8";
    std::function<void(std::string_view severity, std::string_view message)> console_log;
    PerfTraceCallback perf_trace;
};

class ResourceLoader;

// A Page is NOT thread-safe and must be used from a single thread at a time. This
// includes the const read methods (display_list/render/computed_style/
// element_geometry and document() const): they maintain internal styled-document,
// geometry, and computed-style caches and adjust the DomDocument's
// layout-sensitive-attribute configuration, so concurrent "reads" race on shared
// mutable state. Use one Page per thread, or serialize access externally.
class Page {
public:
    explicit Page(LoadOptions options = {});
    ~Page();

    Page(const Page&) = delete;
    Page& operator=(const Page&) = delete;

    void set_resource_loader(std::shared_ptr<ResourceLoader> loader);
    void set_layout_engine_factory(std::shared_ptr<LayoutEngineFactory> factory);

    void load_html(std::string_view html, std::string base_url = {}, std::string character_set = "UTF-8");
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
    // Same contract as element_geometry(), but always forces a fresh layout on a
    // stale cache instead of falling back to geometry_bounded_mode's approximate
    // (or, for a never-before-measured node, null) last-known value. Not exposed
    // to page scripts: intended for WebDriver-simulation call sites (e.g. the WPT
    // testdriver vendor shim) that need real WebDriver's always-exact Get Element
    // Rect semantics regardless of unrelated geometry reads elsewhere on the page.
    std::optional<ElementGeometry> exact_element_geometry(NodeId node) const;
    // Hit-tests the last layout at (x, y), viewport-relative pixels. Returns
    // NodeIds in front-to-back paint order (topmost first); empty if nothing was
    // hit or if the engine doesn't support hit-testing. Always forces a fresh
    // layout on a stale cache, regardless of geometry_bounded_mode.
    // topmost_only == true stops after the first hit.
    std::vector<NodeId> elements_at_point(float x, float y, bool topmost_only) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pagecore
