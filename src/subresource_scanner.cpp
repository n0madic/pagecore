#include "subresource_scanner.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>

#include "perf_scope.hpp"
#include "render_resource_loaders.hpp"

namespace pagecore {

SubresourceScanner::SubresourceScanner(DomDocument& document)
    : document_(document)
{
}

void SubresourceScanner::reset()
{
    render_resource_cache_.reset();
}

// Resolves a raw sub-resource reference against `resolve_base`, drops targets
// that do not benefit from prefetching (data:/blob:/fragment), de-duplicates by
// kind+URL, and appends a request whose referrer matches the litehtml
// container's.
void SubresourceScanner::add_subresource(
    std::vector<ResourceRequest>& requests,
    std::unordered_set<std::string>& seen,
    const std::string& resolve_base,
    const std::string& referrer,
    std::string_view raw,
    ResourceKind kind)
{
    if (raw.empty()) {
        return;
    }
    const std::string url = resolve_url(resolve_base, raw);
    if (url.empty() || url.front() == '#'
        || url.rfind("data:", 0) == 0 || url.rfind("blob:", 0) == 0) {
        return;
    }
    const std::string key = resource_kind_name(kind) + "\n" + url;
    if (!seen.insert(key).second) {
        return;
    }
    requests.push_back(ResourceRequest{url, kind, referrer, resolve_base});
}

// Sub-resources discoverable directly from the DOM: <img src>, <link
// rel=stylesheet href>, and url(...) targets in inline style="" attributes and
// <style> blocks. All resolve against the document's effective base.
void SubresourceScanner::collect_dom_subresources(
    const std::string& effective_base,
    std::vector<ResourceRequest>& requests,
    std::unordered_set<std::string>& seen)
{
    const NodeId root = document_.document_node();

    for (NodeId img : document_.query_selector_all(root, "img[src]")) {
        if (const auto src = document_.get_attribute(img, "src")) {
            add_subresource(requests, seen, effective_base, effective_base, *src, ResourceKind::Image);
        }
    }
    for (NodeId link : document_.query_selector_all(root, "link[rel='stylesheet'][href]")) {
        if (const auto href = document_.get_attribute(link, "href")) {
            add_subresource(requests, seen, effective_base, effective_base, *href, ResourceKind::Stylesheet);
        }
    }
    for (NodeId styled : document_.query_selector_all(root, "[style]")) {
        if (const auto style = document_.get_attribute(styled, "style")) {
            for (const auto& ref : extract_css_urls(*style)) {
                add_subresource(requests, seen, effective_base, effective_base, ref.url, ref.kind);
            }
        }
    }
    for (NodeId style : document_.query_selector_all(root, "style")) {
        for (const auto& ref : extract_css_urls(document_.text_content(style))) {
            add_subresource(requests, seen, effective_base, effective_base, ref.url, ref.kind);
        }
    }
}

void SubresourceScanner::collect_inline_style_attribute_selectors(CssAttributeSelectorUsage& usage)
{
    const NodeId root = document_.document_node();
    for (NodeId style : document_.query_selector_all(root, "style")) {
        collect_css_attribute_selectors(document_.text_content(style), usage);
    }
}

void SubresourceScanner::apply_layout_sensitive_attributes(const CssAttributeSelectorUsage& usage)
{
    document_.set_layout_sensitive_attributes(css_attribute_selector_names(usage), usage.wildcard);
}

#if defined(PAGECORE_ENABLE_RENDERING)
void SubresourceScanner::add_font_request(
    std::vector<PendingFontRequest>& requests,
    std::unordered_set<std::string>& seen,
    const std::string& resolve_base,
    const std::string& referrer,
    const CssFontFace& face,
    std::string_view raw)
{
    if (raw.empty()) {
        return;
    }
    const std::string url = resolve_url(resolve_base, raw);
    if (url.empty() || url.front() == '#' || url.rfind("blob:", 0) == 0) {
        return;
    }

    std::string key = face.family;
    key += '\n';
    key += std::to_string(face.weight);
    key += face.italic ? "\ni\n" : "\nn\n";
    key += url;
    if (!seen.insert(key).second) {
        return;
    }

    requests.push_back(PendingFontRequest{
        WebFontSource{face.family, url, {}, face.weight, face.italic},
        ResourceRequest{url, ResourceKind::Font, referrer, resolve_base},
    });
}

void SubresourceScanner::collect_font_requests_from_css(
    std::string_view css,
    const std::string& resolve_base,
    const std::string& referrer,
    std::vector<PendingFontRequest>& requests,
    std::unordered_set<std::string>& seen)
{
    for (const auto& face : extract_font_faces(css)) {
        for (const auto& source : face.sources) {
            add_font_request(requests, seen, resolve_base, referrer, face, source);
        }
    }
}

std::shared_ptr<const FontEnvironment> SubresourceScanner::prepare_font_environment(
    const RenderOptions& render_options,
    const std::shared_ptr<ResourceLoader>& render_loader,
    LayoutResourceMode resource_mode,
    const std::string& effective_base_url)
{
    if (resource_mode != LayoutResourceMode::Full) {
        return nullptr;
    }
    if (!render_options.load_external_resources || !render_loader) {
        return nullptr;
    }

    const std::string& effective_base = effective_base_url;
    std::vector<PendingFontRequest> pending_fonts;
    std::unordered_set<std::string> seen_fonts;
    const NodeId root = document_.document_node();

    for (NodeId style : document_.query_selector_all(root, "style")) {
        collect_font_requests_from_css(
            document_.text_content(style),
            effective_base,
            effective_base,
            pending_fonts,
            seen_fonts);
    }

    std::vector<ResourceRequest> stylesheet_requests;
    std::unordered_set<std::string> seen_stylesheets;
    for (NodeId link : document_.query_selector_all(root, "link[rel='stylesheet'][href]")) {
        if (const auto href = document_.get_attribute(link, "href")) {
            add_subresource(
                stylesheet_requests,
                seen_stylesheets,
                effective_base,
                effective_base,
                *href,
                ResourceKind::Stylesheet);
        }
    }

    if (!stylesheet_requests.empty()) {
        try {
            const auto sheets = render_loader->load_all(stylesheet_requests, BatchErrorMode::Lenient);
            for (std::size_t i = 0; i < sheets.size() && i < stylesheet_requests.size(); ++i) {
                const auto& sheet = sheets[i];
                if (sheet.status < 200 || sheet.status >= 400 || sheet.body.empty()) {
                    continue;
                }
                const std::string sheet_base = sheet.url.empty() ? stylesheet_requests[i].url : sheet.url;
                collect_font_requests_from_css(
                    sheet.body,
                    sheet_base,
                    effective_base,
                    pending_fonts,
                    seen_fonts);
            }
        } catch (...) {
        }
    }

    if (pending_fonts.empty()) {
        return nullptr;
    }

    std::vector<ResourceRequest> font_requests;
    font_requests.reserve(pending_fonts.size());
    for (const auto& pending : pending_fonts) {
        font_requests.push_back(pending.request);
    }

    std::vector<WebFontSource> loaded_fonts;
    try {
        const auto responses = render_loader->load_all(font_requests, BatchErrorMode::Lenient);
        for (std::size_t i = 0; i < responses.size() && i < pending_fonts.size(); ++i) {
            if (responses[i].status < 200 || responses[i].status >= 400 || responses[i].body.empty()) {
                continue;
            }
            WebFontSource source = std::move(pending_fonts[i].source);
            source.body = responses[i].body;
            loaded_fonts.push_back(std::move(source));
        }
    } catch (...) {
        return nullptr;
    }

    return create_font_environment(loaded_fonts);
}
#else
std::shared_ptr<const FontEnvironment> SubresourceScanner::prepare_font_environment(
    const RenderOptions&,
    const std::shared_ptr<ResourceLoader>&,
    LayoutResourceMode,
    const std::string&)
{
    return nullptr;
}
#endif

std::uint64_t SubresourceScanner::render_prefetch_loaded_bytes(const std::vector<ResourceResponse>& responses) const
{
    std::uint64_t bytes = 0;
    for (const auto& response : responses) {
        bytes += response.body.size();
    }
    return bytes;
}

void SubresourceScanner::emit_render_prefetch_responses(
    const PerfTraceCallback& perf_trace,
    std::string_view wave,
    const std::vector<ResourceRequest>& requests,
    const std::vector<ResourceResponse>& responses) const
{
    const std::size_t count = std::min(requests.size(), responses.size());
    for (std::size_t i = 0; i < count; ++i) {
        PerfEvent event{
            PerfPhase::ResourceLoad,
            "render_prefetch_response",
            0,
            responses[i].body.size(),
        };
        event.property = resource_kind_name(requests[i].kind);
        event.url = responses[i].url.empty() ? requests[i].url : responses[i].url;
        event.reason = responses[i].from_cache
            ? std::string(wave) + ":cache"
            : std::string(wave);
        emit_perf_trace(perf_trace, std::move(event));
    }
}

// Returns the loader litehtml should use for a render. When external loading is
// enabled, the page's sub-resources are prefetched concurrently into a cache so
// the layout's synchronous requests hit warm entries.
//
// The cache persists across styled-document rebuilds for the lifetime of the
// current document: a script-heavy page invalidates the styled-document cache on
// every DOM mutation, so it rebuilds (and re-lays-out) its litehtml document
// many times during a single load, and each layout re-requests the page's images
// and stylesheets. A per-rebuild cache would re-fetch all of them from the
// network every time; a persistent one fetches each resource once. It is reset
// whenever the loader or document identity changes (via reset()).
std::shared_ptr<ResourceLoader> SubresourceScanner::prepare_render_loader(
    const std::shared_ptr<ResourceLoader>& base_loader,
    CookieJar& cookie_jar,
    const RenderOptions& render_options,
    LayoutResourceMode resource_mode,
    const std::string& render_base_url,
    const std::string& effective_base_url,
    const PerfTraceCallback& perf_trace)
{
    CssAttributeSelectorUsage attribute_usage;
    collect_inline_style_attribute_selectors(attribute_usage);
    apply_layout_sensitive_attributes(attribute_usage);

    if (!render_options.load_external_resources || !base_loader) {
        return nullptr;
    }

    if (!render_resource_cache_) {
        render_resource_cache_ = std::make_shared<CachingResourceLoader>(base_loader, 4096);
    }
    const std::shared_ptr<CachingResourceLoader>& cache = render_resource_cache_;
    std::shared_ptr<ResourceLoader> render_loader = std::make_shared<CookieAwareResourceLoader>(
        cache,
        &cookie_jar,
        render_base_url,
        CookieCredentials::SameOrigin);
    if (resource_mode == LayoutResourceMode::StylesheetsOnly) {
        render_loader = std::make_shared<StylesheetOnlyResourceLoader>(
            render_loader,
            perf_trace);
    }
    if (render_options.max_external_resource_loads
        || render_options.max_external_resource_bytes
        || render_options.max_external_resource_time) {
        render_loader = std::make_shared<BudgetedRenderResourceLoader>(
            render_loader,
            render_options,
            perf_trace);
    }

    const std::string& effective_base = effective_base_url;

    std::vector<ResourceRequest> wave1;
    std::unordered_set<std::string> seen;
    {
        PerfScope trace(perf_trace, PerfPhase::SubresourceScan, "collect_dom_subresources");
        collect_dom_subresources(effective_base, wave1, seen);
        if (resource_mode == LayoutResourceMode::StylesheetsOnly) {
            wave1.erase(
                std::remove_if(
                    wave1.begin(),
                    wave1.end(),
                    [](const ResourceRequest& request) { return request.kind != ResourceKind::Stylesheet; }),
                wave1.end());
        }
        trace.set_count(wave1.size());
    }
    if (wave1.empty()) {
        return render_loader;
    }

    std::vector<ResourceResponse> fetched;
    try {
        // Lenient: a failed sub-resource must not abort the render; litehtml will
        // fall back to its placeholder for it.
        PerfScope trace(perf_trace, PerfPhase::ResourceLoad, "render_prefetch_wave1", wave1.size());
        trace.event().property = resource_mode == LayoutResourceMode::Full ? "render" : "geometry";
        trace.event().reason = "wave1";
        fetched = render_loader->load_all(wave1, BatchErrorMode::Lenient);
        trace.set_count(render_prefetch_loaded_bytes(fetched));
        emit_render_prefetch_responses(perf_trace, "wave1", wave1, fetched);
    } catch (...) {
        // Prefetch is purely an optimization; on any failure fall back to the
        // cache delegating each request to the inner loader on demand.
        return render_loader;
    }

    // Second wave: background images referenced by url() inside the external
    // stylesheets just fetched. Their url()s resolve against each stylesheet's
    // own URL, exactly as the litehtml container resolves them.
    std::vector<ResourceRequest> wave2;
    for (std::size_t i = 0; i < fetched.size() && i < wave1.size(); ++i) {
        if (wave1[i].kind != ResourceKind::Stylesheet) {
            continue;
        }
        const ResourceResponse& sheet = fetched[i];
        if (sheet.status < 200 || sheet.status >= 400 || sheet.body.empty()) {
            continue;
        }
        collect_css_attribute_selectors(sheet.body, attribute_usage);
        if (resource_mode != LayoutResourceMode::Full) {
            continue;
        }
        const std::string sheet_base = sheet.url.empty() ? wave1[i].url : sheet.url;
        for (const auto& ref : extract_css_urls(sheet.body)) {
            add_subresource(wave2, seen, sheet_base, effective_base, ref.url, ref.kind);
        }
    }
    apply_layout_sensitive_attributes(attribute_usage);

    if (!wave2.empty()) {
        try {
            PerfScope trace(perf_trace, PerfPhase::ResourceLoad, "render_prefetch_wave2", wave2.size());
            trace.event().property = "render";
            trace.event().reason = "wave2";
            const auto wave2_responses = render_loader->load_all(wave2, BatchErrorMode::Lenient);
            trace.set_count(render_prefetch_loaded_bytes(wave2_responses));
            emit_render_prefetch_responses(perf_trace, "wave2", wave2, wave2_responses);
        } catch (...) {
        }
    }

    return render_loader;
}

} // namespace pagecore
