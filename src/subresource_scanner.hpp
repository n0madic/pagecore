#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "cookie_jar.hpp"
#include "css_scan.hpp"
#include "pagecore/dom.hpp"
#include "pagecore/perf.hpp"
#include "pagecore/render.hpp"
#include "pagecore/resource_loader.hpp"

#if defined(PAGECORE_ENABLE_RENDERING)
#include "web_fonts.hpp"
#endif

namespace pagecore {

// Whether a styled-document build should load every layout-affecting resource
// (Full) or only stylesheets (StylesheetsOnly, used by synchronous geometry and
// computed-style reads that need the cascade but must not fetch images yet).
enum class LayoutResourceMode {
    Full,
    StylesheetsOnly,
};

#if defined(PAGECORE_ENABLE_RENDERING)
// A discovered @font-face source paired with the request used to fetch it.
struct PendingFontRequest {
    WebFontSource source;
    ResourceRequest request;
};
#endif

// Discovers a document's render sub-resources (images, stylesheets, CSS
// backgrounds, and web fonts) and prefetches them concurrently into a cache so
// the layout engine's synchronous, on-demand requests hit warm entries. Owns the
// per-document render resource cache, which persists across styled-document
// rebuilds and is dropped via reset() when the document or loader identity
// changes. Prefetch is a pure optimisation: any failure degrades to on-demand
// loading and never changes the rendered result.
class SubresourceScanner {
public:
    explicit SubresourceScanner(DomDocument& document);

    // Builds the render loader stack (Caching -> CookieAware -> [StylesheetOnly]
    // -> [Budgeted]) over `base_loader`, runs the wave1/wave2 prefetch, and
    // returns the loader the layout engine should use. `render_base_url` and
    // `effective_base_url` are computed by the caller (which owns the load
    // options and current URL) and passed in. Returns nullptr when external
    // loading is disabled or no base loader is available.
    std::shared_ptr<ResourceLoader> prepare_render_loader(
        const std::shared_ptr<ResourceLoader>& base_loader,
        CookieJar& cookie_jar,
        const RenderOptions& render_options,
        LayoutResourceMode resource_mode,
        const std::string& render_base_url,
        const std::string& effective_base_url,
        const PerfTraceCallback& perf_trace);

    // Fetches and builds the web-font environment for a Full render. Returns
    // nullptr when rendering is disabled, the mode is not Full, external loading
    // is off, or no fonts are discovered.
    std::shared_ptr<const FontEnvironment> prepare_font_environment(
        const RenderOptions& render_options,
        const std::shared_ptr<ResourceLoader>& render_loader,
        LayoutResourceMode resource_mode,
        const std::string& effective_base_url);

    // Drops the render resource cache (called when the document/loader identity
    // changes).
    void reset();

private:
    void add_subresource(
        std::vector<ResourceRequest>& requests,
        std::unordered_set<std::string>& seen,
        const std::string& resolve_base,
        const std::string& referrer,
        std::string_view raw,
        ResourceKind kind);

    void collect_dom_subresources(
        const std::string& effective_base,
        std::vector<ResourceRequest>& requests,
        std::unordered_set<std::string>& seen);

    void collect_inline_style_attribute_selectors(CssAttributeSelectorUsage& usage);
    void apply_layout_sensitive_attributes(const CssAttributeSelectorUsage& usage);

#if defined(PAGECORE_ENABLE_RENDERING)
    void add_font_request(
        std::vector<PendingFontRequest>& requests,
        std::unordered_set<std::string>& seen,
        const std::string& resolve_base,
        const std::string& referrer,
        const CssFontFace& face,
        std::string_view raw);

    void collect_font_requests_from_css(
        std::string_view css,
        const std::string& resolve_base,
        const std::string& referrer,
        std::vector<PendingFontRequest>& requests,
        std::unordered_set<std::string>& seen);
#endif

    std::uint64_t render_prefetch_loaded_bytes(const std::vector<ResourceResponse>& responses) const;

    void emit_render_prefetch_responses(
        const PerfTraceCallback& perf_trace,
        std::string_view wave,
        const std::vector<ResourceRequest>& requests,
        const std::vector<ResourceResponse>& responses) const;

    DomDocument& document_;
    // Sub-resource byte cache shared by every styled-document rebuild of the
    // current document, so a script-heavy page that rebuilds many times fetches
    // each image/stylesheet once instead of on every rebuild.
    std::shared_ptr<CachingResourceLoader> render_resource_cache_;
};

} // namespace pagecore
