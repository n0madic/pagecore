#include "pagecore/page.hpp"

#include "js_runtime.hpp"
#include "pagecore/render.hpp"
#include "pagecore/resource_loader.hpp"
#if defined(PAGECORE_ENABLE_RENDERING)
#include "web_fonts.hpp"
#endif

#include <memory>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pagecore {
namespace {

struct ScriptSource {
    NodeId node = kInvalidNodeId;
    std::string filename;
    std::string code;
    bool module = false;
};

std::string normalized_script_type(std::string_view value)
{
    const auto first = value.find_first_not_of(" \t\n\r\f");
    if (first == std::string_view::npos) {
        return {};
    }

    auto last = value.find_last_not_of(" \t\n\r\f");
    std::string out(value.substr(first, last - first + 1));
    const auto semicolon = out.find(';');
    if (semicolon != std::string::npos) {
        out.erase(semicolon);
    }
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return out;
}

bool is_standard_javascript_type(std::string_view type)
{
    return type == "module"
        || type == "text/javascript"
        || type == "application/javascript"
        || type == "application/ecmascript"
        || type == "text/ecmascript"
        || type == "application/x-javascript"
        || type == "text/jscript";
}

bool is_javascript_script_type(const std::optional<std::string>& type)
{
    if (!type || type->empty()) {
        return true;
    }
    const std::string normalized = normalized_script_type(*type);
    return normalized.empty() || is_standard_javascript_type(normalized);
}

bool is_module_script_type(const std::optional<std::string>& type)
{
    if (!type) {
        return false;
    }
    return normalized_script_type(*type) == "module";
}

// Identifies the inputs that fully determine a built styled document (the
// litehtml document backing both the rendered DisplayList and
// getComputedStyle()). If these match a previous build, the cached engine is
// reused instead of re-running the serialize/parse pipeline. The resource
// loader's own byte cache is intentionally not part of the key: a build is
// memoized against DOM state, viewport, and base URL, not against external
// resource contents.
struct StyledDocumentCacheKey {
    std::uint64_t layout_mutation_version = 0;
    int viewport_width = 0;
    int viewport_height = 0;
    float device_scale_factor = 0.0f;
    bool load_external_resources = false;
    std::string base_url;

    bool operator==(const StyledDocumentCacheKey&) const = default;
};

struct CssUrlRef {
    std::string url;
    ResourceKind kind = ResourceKind::Image;
};

#if defined(PAGECORE_ENABLE_RENDERING)
struct PendingFontRequest {
    WebFontSource source;
    ResourceRequest request;
};
#endif

struct GeometryCacheEntry {
    std::optional<ElementGeometry> geometry;
};

// Extracts url(...) targets from a CSS source so they can be prefetched. Handles
// comments, quoted/unquoted targets, and multi-value declarations. `@import`
// targets are returned as stylesheets; `src:` declarations (web fonts) are
// skipped here because they are fetched by the web-font pipeline, not the image
// prefetch path. Callers still resolve and filter the raw targets.
std::vector<CssUrlRef> extract_css_urls(std::string_view css)
{
    std::vector<CssUrlRef> out;
    const std::size_t n = css.size();
    std::size_t segment_start = 0;

    const auto classify = [&](std::size_t url_pos) -> int {
        // 0 = image, 1 = skip (font src), 2 = stylesheet (@import).
        std::string_view seg = css.substr(segment_start, url_pos - segment_start);
        const auto first = seg.find_first_not_of(" \t\r\n");
        if (first == std::string_view::npos) {
            return 0;
        }
        seg = seg.substr(first);
        if (seg.size() >= 7) {
            std::string head;
            for (char ch : seg.substr(0, 7)) {
                head.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
            }
            if (head == "@import") {
                return 2;
            }
        }
        const auto colon = seg.find(':');
        if (colon != std::string_view::npos) {
            std::string prop;
            for (std::size_t i = 0; i < colon; ++i) {
                const unsigned char ch = static_cast<unsigned char>(seg[i]);
                if (!std::isspace(ch)) {
                    prop.push_back(static_cast<char>(std::tolower(ch)));
                }
            }
            if (prop == "src") {
                return 1;
            }
        }
        return 0;
    };

    std::size_t i = 0;
    while (i < n) {
        const char ch = css[i];
        if (ch == '/' && i + 1 < n && css[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(css[i] == '*' && css[i + 1] == '/')) {
                ++i;
            }
            i = std::min(n, i + 2);
            continue;
        }
        if (ch == '"' || ch == '\'') {
            const char quote = ch;
            ++i;
            while (i < n && css[i] != quote) {
                if (css[i] == '\\' && i + 1 < n) {
                    ++i;
                }
                ++i;
            }
            if (i < n) {
                ++i;
            }
            continue;
        }
        if (ch == ';' || ch == '{' || ch == '}') {
            segment_start = i + 1;
            ++i;
            continue;
        }
        if ((ch == 'u' || ch == 'U') && i + 3 < n) {
            const bool prev_ident = i > 0
                && (std::isalnum(static_cast<unsigned char>(css[i - 1])) || css[i - 1] == '-' || css[i - 1] == '_');
            const char c1 = css[i + 1];
            const char c2 = css[i + 2];
            if (!prev_ident && (c1 == 'r' || c1 == 'R') && (c2 == 'l' || c2 == 'L')) {
                std::size_t j = i + 3;
                while (j < n && std::isspace(static_cast<unsigned char>(css[j]))) {
                    ++j;
                }
                if (j < n && css[j] == '(') {
                    const int cls = classify(i);
                    ++j;
                    while (j < n && std::isspace(static_cast<unsigned char>(css[j]))) {
                        ++j;
                    }
                    std::string target;
                    if (j < n && (css[j] == '"' || css[j] == '\'')) {
                        const char quote = css[j];
                        ++j;
                        while (j < n && css[j] != quote) {
                            if (css[j] == '\\' && j + 1 < n) {
                                target.push_back(css[j + 1]);
                                j += 2;
                            } else {
                                target.push_back(css[j]);
                                ++j;
                            }
                        }
                        if (j < n) {
                            ++j;
                        }
                        while (j < n && css[j] != ')') {
                            ++j;
                        }
                    } else {
                        while (j < n && css[j] != ')') {
                            target.push_back(css[j]);
                            ++j;
                        }
                        while (!target.empty() && std::isspace(static_cast<unsigned char>(target.back()))) {
                            target.pop_back();
                        }
                    }
                    if (j < n) {
                        ++j;
                    }
                    if (cls != 1 && !target.empty()) {
                        out.push_back(CssUrlRef{
                            std::move(target),
                            cls == 2 ? ResourceKind::Stylesheet : ResourceKind::Image,
                        });
                    }
                    i = j;
                    continue;
                }
            }
        }
        ++i;
    }
    return out;
}

} // namespace

struct Page::Impl {
    LoadOptions options;
    DomDocument document;
    std::shared_ptr<ResourceLoader> loader;
    std::shared_ptr<LayoutEngineFactory> layout_factory;
    std::unique_ptr<JsRuntime> js;
    std::string current_url;

    // The single litehtml document shared by the rendered DisplayList and by
    // getComputedStyle(): on a cache hit, cached_display_list() just runs
    // layout() on it if that hasn't happened yet for this build.
    std::unique_ptr<LayoutEngine> styled_document;
    StyledDocumentCacheKey styled_document_key;
    bool styled_document_valid = false;
    bool styled_document_laid_out = false;
    // getComputedStyle() has no viewport argument; it resolves against the
    // most recently used render viewport (or the engine default if the page
    // hasn't been rendered yet).
    RenderOptions last_render_options;

    // Synchronous geometry read-back (getBoundingClientRect / offsetWidth / ...)
    // needs a real litehtml layout() pass. litehtml has no incremental layout
    // against our Lexbor tree, so each geometry read that follows a DOM mutation
    // forces a full rebuild (serialize the whole DOM, re-parse, re-cascade) plus
    // a full layout. On a large DOM that's seconds per read; a script that reads
    // geometry in a tight read-modify-write loop (e.g. jQuery init) can then trip
    // the per-script execution deadline. Once forced geometry layouts consume a
    // small budget, later post-mutation geometry reads return the last exact
    // value seen for that NodeId (or nullopt/0 if none exists) until a render or
    // another non-geometry caller produces a fresh current layout. Cheap pages
    // keep exact synchronous geometry.
    bool styled_document_expensive = false;
    static constexpr long long kExpensiveStyledDocumentUs = 250'000; // 0.25s
    bool geometry_bounded_mode = false;
    int geometry_forced_layout_count = 0;
    long long geometry_forced_layout_us = 0;
    std::uint64_t geometry_cache_forget_version = 0;
    std::unordered_map<NodeId, GeometryCacheEntry> last_known_geometry;
    static constexpr int kGeometryForcedLayoutCountThreshold = 2;
    static constexpr long long kGeometryForcedLayoutBudgetUs = 100'000; // 0.10s

    // Sub-resource cache shared by every styled-document rebuild of the current
    // document, so a page that rebuilds many times during script execution
    // fetches each image/stylesheet once instead of on every rebuild. Reset when
    // the document or loader identity changes (invalidate_display_list_cache).
    std::shared_ptr<CachingResourceLoader> render_resource_cache;

    explicit Impl(LoadOptions init_options)
        : options(std::move(init_options))
        , loader(std::make_shared<CurlResourceLoader>(options.user_agent))
    {
#if defined(PAGECORE_ENABLE_RENDERING)
        layout_factory = create_litehtml_layout_engine_factory();
#endif
    }

    void invalidate_display_list_cache()
    {
        styled_document_valid = false;
        styled_document_laid_out = false;
        styled_document.reset();
        // Expensiveness is a property of the page's content; a fresh document
        // may be cheap, so re-measure it from scratch.
        styled_document_expensive = false;
        // Sub-resource URLs and the loader itself may differ for a new document,
        // so drop the cross-rebuild cache when the document/loader identity
        // changes.
        render_resource_cache.reset();
        geometry_bounded_mode = false;
        geometry_forced_layout_count = 0;
        geometry_forced_layout_us = 0;
        geometry_cache_forget_version = document.forget_version();
        last_known_geometry.clear();
    }

    void ensure_js()
    {
        if (!js) {
            js = std::make_unique<JsRuntime>(document, options, loader);
            js->set_computed_style_resolver([this](NodeId node) { return computed_style(node); });
            js->set_element_geometry_resolver([this](NodeId node) { return element_geometry(node); });
            js->set_viewport_resolver([this] { return last_render_options.viewport; });
            js->install();
        }
    }

    std::vector<ScriptSource> collect_scripts()
    {
        std::vector<ScriptSource> scripts;
        std::vector<ResourceRequest> external_requests;
        std::vector<std::size_t> external_slots;

        const NodeId root = document.document_node();
        std::size_t inline_script_index = 0;
        std::size_t inline_module_index = 0;
        for (NodeId script : document.query_selector_all(root, "script")) {
            if (document.has_attribute(script, "nomodule")) {
                continue;
            }
            const auto src = document.get_attribute(script, "src");
            const auto type = document.get_attribute(script, "type");
            if (!is_javascript_script_type(type)) {
                continue;
            }
            const bool module = is_module_script_type(type);

            if (src && !src->empty()) {
                const std::string base = current_url.empty() ? options.base_url : current_url;
                const std::string url = resolve_url(base, *src);
                // Record the slot; the body is fetched in one batched, concurrent
                // pass after the whole document is scanned. Execution still happens
                // in document order in execute_scripts().
                scripts.push_back(ScriptSource{script, url, std::string{}, module});
                external_slots.push_back(scripts.size() - 1);
                external_requests.push_back(ResourceRequest{url, ResourceKind::Script, base, base});
            } else {
                std::string filename = "<inline-script>";
                if (module) {
                    filename = (current_url.empty() ? options.base_url : current_url)
                        + "#inline-module-" + std::to_string(inline_module_index++);
                } else {
                    const std::string base = current_url.empty() ? options.base_url : current_url;
                    filename = base.empty()
                        ? "<inline-script-" + std::to_string(inline_script_index) + ">"
                        : base + "#inline-script-" + std::to_string(inline_script_index);
                    ++inline_script_index;
                }
                scripts.push_back(ScriptSource{script, std::move(filename), document.text_content(script), module});
            }
        }

        if (!external_requests.empty()) {
            std::vector<ResourceResponse> responses = loader->load_all(external_requests);
            for (std::size_t i = 0; i < responses.size() && i < external_slots.size(); ++i) {
                ScriptSource& slot = scripts[external_slots[i]];
                slot.filename = std::move(responses[i].url);
                slot.code = std::move(responses[i].body);
            }
        }

        return scripts;
    }

    void execute_scripts()
    {
        ensure_js();
        for (const auto& script : collect_scripts()) {
            js->execute("document.__markScriptStarted(" + std::to_string(script.node) + ");", "<pagecore-script-state>");
            if (script.module) {
                js->execute("document.__setCurrentScript(null);", "<pagecore-current-script>");
                try {
                    js->execute_module(script.code, script.filename);
                } catch (const std::exception& error) {
                    js->log_console("error", error.what());
                } catch (...) {
                    js->log_console("error", "JS exception in module script");
                }
                continue;
            }

            js->execute("document.__setCurrentScript(" + std::to_string(script.node) + ");", "<pagecore-current-script>");
            try {
                js->execute(script.code, script.filename);
            } catch (const std::exception& error) {
                js->log_console("error", error.what());
            } catch (...) {
                js->log_console("error", "JS exception in script");
            }
            try {
                js->execute("document.__setCurrentScript(null);", "<pagecore-current-script>");
            } catch (...) {
            }
        }
        js->execute(
            "if (typeof __pagecore_fireDOMContentLoaded === 'function') __pagecore_fireDOMContentLoaded();\n"
            "if (typeof __pagecore_fireLoad === 'function') __pagecore_fireLoad();",
            "<pagecore-lifecycle>");
        js->run_until_idle();
    }

    std::string render_base_url(const RenderOptions& render_options) const
    {
        if (!render_options.base_url.empty()) {
            return render_options.base_url;
        }
        if (!current_url.empty()) {
            return current_url;
        }
        return options.base_url;
    }

    std::string serialized_html_for_render() const
    {
        return document.serialize_html_for_layout(options.enable_js);
    }

    // The document's effective base URL for resolving sub-resources, honouring a
    // <base href> exactly as the litehtml container does, so warmed cache keys
    // match its on-demand requests (the container uses this base as the referrer).
    std::string effective_base_url(const RenderOptions& render_options)
    {
        const std::string render_base = render_base_url(render_options);
        const NodeId base_node = document.query_selector(document.document_node(), "base[href]");
        if (base_node != kInvalidNodeId) {
            const auto base_href = document.get_attribute(base_node, "href");
            if (base_href && !base_href->empty()) {
                return resolve_url(render_base, *base_href);
            }
        }
        return render_base;
    }

    // Resolves a raw sub-resource reference against `resolve_base`, drops
    // targets that do not benefit from prefetching (data:/blob:/fragment),
    // de-duplicates by kind+URL, and appends a request whose referrer matches
    // the litehtml container's.
    void add_subresource(
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
    void collect_dom_subresources(
        const std::string& effective_base,
        std::vector<ResourceRequest>& requests,
        std::unordered_set<std::string>& seen)
    {
        const NodeId root = document.document_node();

        for (NodeId img : document.query_selector_all(root, "img[src]")) {
            if (const auto src = document.get_attribute(img, "src")) {
                add_subresource(requests, seen, effective_base, effective_base, *src, ResourceKind::Image);
            }
        }
        for (NodeId link : document.query_selector_all(root, "link[rel='stylesheet'][href]")) {
            if (const auto href = document.get_attribute(link, "href")) {
                add_subresource(requests, seen, effective_base, effective_base, *href, ResourceKind::Stylesheet);
            }
        }
        for (NodeId styled : document.query_selector_all(root, "[style]")) {
            if (const auto style = document.get_attribute(styled, "style")) {
                for (const auto& ref : extract_css_urls(*style)) {
                    add_subresource(requests, seen, effective_base, effective_base, ref.url, ref.kind);
                }
            }
        }
        for (NodeId style : document.query_selector_all(root, "style")) {
            for (const auto& ref : extract_css_urls(document.text_content(style))) {
                add_subresource(requests, seen, effective_base, effective_base, ref.url, ref.kind);
            }
        }
    }

#if defined(PAGECORE_ENABLE_RENDERING)
    void add_font_request(
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

    void collect_font_requests_from_css(
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

    std::shared_ptr<const FontEnvironment> prepare_font_environment(
        const RenderOptions& render_options,
        const std::shared_ptr<ResourceLoader>& render_loader)
    {
        if (!render_options.load_external_resources || !render_loader) {
            return nullptr;
        }

        const std::string effective_base = effective_base_url(render_options);
        std::vector<PendingFontRequest> pending_fonts;
        std::unordered_set<std::string> seen_fonts;
        const NodeId root = document.document_node();

        for (NodeId style : document.query_selector_all(root, "style")) {
            collect_font_requests_from_css(
                document.text_content(style),
                effective_base,
                effective_base,
                pending_fonts,
                seen_fonts);
        }

        std::vector<ResourceRequest> stylesheet_requests;
        std::unordered_set<std::string> seen_stylesheets;
        for (NodeId link : document.query_selector_all(root, "link[rel='stylesheet'][href]")) {
            if (const auto href = document.get_attribute(link, "href")) {
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
    std::shared_ptr<const FontEnvironment> prepare_font_environment(
        const RenderOptions&,
        const std::shared_ptr<ResourceLoader>&)
    {
        return nullptr;
    }
#endif

    // Returns the loader litehtml should use for a render. When external loading
    // is enabled, the page's sub-resources are prefetched concurrently into a
    // cache so the layout's synchronous requests hit warm entries.
    //
    // The cache persists across styled-document rebuilds for the lifetime of the
    // current document: a script-heavy page invalidates the styled-document
    // cache on every DOM mutation, so it rebuilds (and re-lays-out) its litehtml
    // document many times during a single load, and each layout re-requests the
    // page's images and stylesheets. A per-rebuild cache would re-fetch all of
    // them from the network every time; a persistent one fetches each resource
    // once. It is reset whenever the loader or document identity changes (see
    // invalidate_display_list_cache).
    std::shared_ptr<ResourceLoader> prepare_render_loader(const RenderOptions& render_options)
    {
        if (!render_options.load_external_resources || !loader) {
            return nullptr;
        }

        if (!render_resource_cache) {
            render_resource_cache = std::make_shared<CachingResourceLoader>(loader, 4096);
        }
        const std::shared_ptr<CachingResourceLoader>& cache = render_resource_cache;

        const std::string effective_base = effective_base_url(render_options);

        std::vector<ResourceRequest> wave1;
        std::unordered_set<std::string> seen;
        collect_dom_subresources(effective_base, wave1, seen);
        if (wave1.empty()) {
            return cache;
        }

        std::vector<ResourceResponse> fetched;
        try {
            // Lenient: a failed sub-resource must not abort the render; litehtml
            // will fall back to its placeholder for it.
            fetched = cache->load_all(wave1, BatchErrorMode::Lenient);
        } catch (...) {
            // Prefetch is purely an optimization; on any failure fall back to the
            // cache delegating each request to the inner loader on demand.
            return cache;
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
            const std::string sheet_base = sheet.url.empty() ? wave1[i].url : sheet.url;
            for (const auto& ref : extract_css_urls(sheet.body)) {
                add_subresource(wave2, seen, sheet_base, effective_base, ref.url, ref.kind);
            }
        }

        if (!wave2.empty()) {
            try {
                cache->load_all(wave2, BatchErrorMode::Lenient);
            } catch (...) {
            }
        }

        return cache;
    }

    StyledDocumentCacheKey styled_document_cache_key(const RenderOptions& render_options) const
    {
        return StyledDocumentCacheKey{
            document.layout_mutation_version(),
            render_options.viewport.width,
            render_options.viewport.height,
            render_options.viewport.device_scale_factor,
            render_options.load_external_resources,
            render_base_url(render_options),
        };
    }

    bool has_current_layout(const RenderOptions& render_options) const
    {
        return styled_document_valid
            && styled_document_laid_out
            && styled_document_key == styled_document_cache_key(render_options);
    }

    void sync_geometry_cache_for_forget_version()
    {
        const std::uint64_t current_forget_version = document.forget_version();
        if (geometry_cache_forget_version == current_forget_version) {
            return;
        }
        geometry_cache_forget_version = current_forget_version;
        last_known_geometry.clear();
    }

    std::optional<ElementGeometry> cached_geometry(NodeId node)
    {
        sync_geometry_cache_for_forget_version();
        if (!document.is_connected(node)) {
            last_known_geometry.erase(node);
            return std::nullopt;
        }

        const auto found = last_known_geometry.find(node);
        return found == last_known_geometry.end() ? std::nullopt : found->second.geometry;
    }

    void remember_geometry(NodeId node, std::optional<ElementGeometry> geometry)
    {
        sync_geometry_cache_for_forget_version();
        if (!document.is_connected(node)) {
            last_known_geometry.erase(node);
            return;
        }
        last_known_geometry[node] = GeometryCacheEntry{std::move(geometry)};
    }

    void note_geometry_forced_layout(long long elapsed_us)
    {
        ++geometry_forced_layout_count;
        geometry_forced_layout_us += std::max<long long>(0, elapsed_us);
        if (styled_document_expensive
            || (geometry_forced_layout_count >= kGeometryForcedLayoutCountThreshold
                && geometry_forced_layout_us > kGeometryForcedLayoutBudgetUs)) {
            geometry_bounded_mode = true;
        }
    }

    std::unique_ptr<LayoutEngine> build_layout(const RenderOptions& render_options)
    {
        if (!layout_factory) {
            throw std::runtime_error("no layout engine factory configured; build with PAGECORE_ENABLE_RENDERING or set one explicitly");
        }

        auto layout = layout_factory->create_layout_engine();
        if (!layout) {
            throw std::runtime_error("layout engine factory returned null");
        }

        auto render_loader = prepare_render_loader(render_options);
        auto font_environment = prepare_font_environment(render_options, render_loader);
        layout->set_viewport(render_options.viewport);
        layout->set_resource_loader(std::move(render_loader));
        layout->set_font_environment(std::move(font_environment));
        layout->load_html(serialized_html_for_render(), render_base_url(render_options));
        layout->layout();
        return layout;
    }

    // Builds (or reuses) the litehtml document backing both the rendered
    // DisplayList and getComputedStyle(), without running layout() on it —
    // callers decide whether they need a full layout pass or just the
    // cascade (compute_styles_only()).
    LayoutEngine& ensure_styled_document(const RenderOptions& render_options)
    {
        StyledDocumentCacheKey key = styled_document_cache_key(render_options);

        if (styled_document_valid && styled_document_key == key) {
            return *styled_document;
        }

        if (!layout_factory) {
            throw std::runtime_error("no layout engine factory configured; build with PAGECORE_ENABLE_RENDERING or set one explicitly");
        }

        auto engine = layout_factory->create_layout_engine();
        if (!engine) {
            throw std::runtime_error("layout engine factory returned null");
        }

        auto render_loader = prepare_render_loader(render_options);
        auto font_environment = prepare_font_environment(render_options, render_loader);
        engine->set_viewport(render_options.viewport);
        engine->set_resource_loader(std::move(render_loader));
        engine->set_font_environment(std::move(font_environment));
        {
            const auto t0 = std::chrono::steady_clock::now();
            engine->load_html(serialized_html_for_render(), render_base_url(render_options));
            const auto rebuild_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - t0).count();
            if (rebuild_us > kExpensiveStyledDocumentUs) {
                styled_document_expensive = true;
            }
        }

        styled_document = std::move(engine);
        styled_document_key = std::move(key);
        styled_document_valid = true;
        styled_document_laid_out = false;
        return *styled_document;
    }

    // Builds (or reuses) the styled document and runs a full layout() pass
    // on it if that hasn't happened yet for this build. Shared by
    // cached_display_list() and element_geometry(), both of which need real
    // render_item geometry rather than just the cascade.
    LayoutEngine& ensure_layout(const RenderOptions& render_options)
    {
        auto& engine = ensure_styled_document(render_options);
        if (!styled_document_laid_out) {
            const auto t0 = std::chrono::steady_clock::now();
            engine.layout();
            const auto layout_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - t0).count();
            if (layout_us > kExpensiveStyledDocumentUs) {
                styled_document_expensive = true;
            }
            styled_document_laid_out = true;
        }
        return engine;
    }

    const DisplayList& cached_display_list(const RenderOptions& render_options)
    {
        last_render_options = render_options;
        return ensure_layout(render_options).display_list();
    }

    std::optional<ComputedStyle> computed_style(NodeId node)
    {
        auto& engine = ensure_styled_document(last_render_options);
        engine.compute_styles_only();
        return engine.computed_style(std::to_string(node));
    }

    std::optional<ElementGeometry> element_geometry(NodeId node)
    {
        sync_geometry_cache_for_forget_version();
        if (!document.is_connected(node)) {
            last_known_geometry.erase(node);
            return std::nullopt;
        }

        const std::string node_key = std::to_string(node);
        if (has_current_layout(last_render_options)) {
            auto geometry = styled_document->element_geometry(node_key);
            remember_geometry(node, geometry);
            return geometry;
        }

        if (geometry_bounded_mode || styled_document_expensive) {
            geometry_bounded_mode = true;
            return cached_geometry(node);
        }

        // The (re)build itself may reveal the page is expensive; re-check before
        // paying for a full layout() so the first geometry read on a heavy page
        // costs at most the cascade build it would have paid for anyway. The
        // time still counts against the geometry budget.
        const auto t0 = std::chrono::steady_clock::now();
        ensure_styled_document(last_render_options);
        if (styled_document_expensive) {
            const auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - t0).count();
            note_geometry_forced_layout(elapsed_us);
            return cached_geometry(node);
        }

        auto& engine = ensure_layout(last_render_options);
        const auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - t0).count();
        auto geometry = engine.element_geometry(node_key);
        remember_geometry(node, geometry);
        note_geometry_forced_layout(elapsed_us);
        return geometry;
    }
};

Page::Page(LoadOptions options)
    : impl_(std::make_unique<Impl>(std::move(options)))
{
}

Page::~Page() = default;

void Page::set_resource_loader(std::shared_ptr<ResourceLoader> loader)
{
    if (!loader) {
        throw std::runtime_error("resource loader must not be null");
    }
    impl_->loader = std::move(loader);
    impl_->invalidate_display_list_cache();
}

void Page::set_layout_engine_factory(std::shared_ptr<LayoutEngineFactory> factory)
{
    if (!factory) {
        throw std::runtime_error("layout engine factory must not be null");
    }
    impl_->layout_factory = std::move(factory);
    impl_->invalidate_display_list_cache();
}

void Page::load_html(std::string_view html, std::string base_url)
{
    impl_->js.reset();
    impl_->invalidate_display_list_cache();
    impl_->current_url = std::move(base_url);
    impl_->options.base_url = impl_->current_url;
    impl_->document.parse(html);

    if (impl_->options.enable_js) {
        impl_->execute_scripts();
    }
}

void Page::load_url(std::string_view url)
{
    auto response = impl_->loader->load(ResourceRequest{std::string(url), ResourceKind::Document});
    load_html(response.body, response.url.empty() ? std::string(url) : response.url);
}

std::string Page::eval(std::string_view script)
{
    impl_->ensure_js();
    return impl_->js->evaluate(script);
}

void Page::run_until_idle()
{
    if (impl_->js) {
        impl_->js->run_until_idle();
    }
}

DomDocument& Page::document()
{
    return impl_->document;
}

const DomDocument& Page::document() const
{
    return impl_->document;
}

std::string Page::serialize_html() const
{
    return impl_->document.serialize_html();
}

std::unique_ptr<LayoutEngine> Page::layout(RenderOptions render_options) const
{
    return impl_->build_layout(render_options);
}

DisplayList Page::display_list(RenderOptions render_options) const
{
    return impl_->cached_display_list(render_options);
}

RenderedImage Page::render(RenderOptions render_options) const
{
    auto backend = create_default_raster_backend();
    return render(*backend, std::move(render_options));
}

RenderedImage Page::render(RasterBackend& backend, RenderOptions render_options) const
{
    return backend.render(display_list(std::move(render_options)));
}

std::optional<std::string> Page::text_content(std::string_view selector)
{
    const NodeId found = impl_->document.query_selector(impl_->document.document_node(), selector);
    if (found == kInvalidNodeId) {
        return std::nullopt;
    }
    return impl_->document.text_content(found);
}

std::optional<std::string> Page::outer_html(std::string_view selector)
{
    const NodeId found = impl_->document.query_selector(impl_->document.document_node(), selector);
    if (found == kInvalidNodeId) {
        return std::nullopt;
    }
    return impl_->document.outer_html(found);
}

std::optional<ComputedStyle> Page::computed_style(NodeId node) const
{
    return impl_->computed_style(node);
}

std::optional<ElementGeometry> Page::element_geometry(NodeId node) const
{
    return impl_->element_geometry(node);
}

} // namespace pagecore
