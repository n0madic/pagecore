#include "pagecore/page.hpp"

#include "cookie_jar.hpp"
#include "css_scan.hpp"
#include "js_runtime.hpp"
#include "perf_scope.hpp"
#include "render_resource_loaders.hpp"
#include "script_type.hpp"
#include "subresource_scanner.hpp"
#include "pagecore/render.hpp"
#include "pagecore/resource_loader.hpp"

#include <memory>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <chrono>
#include <cstdlib>
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
    // Distinguishes the exact first render pass (false) from the optional second
    // pass that injects render-locally derived absolute-%-width corrections
    // (true). Synchronous reads always pass false.
    bool absolute_percent_corrected = false;
    LayoutResourceMode resource_mode = LayoutResourceMode::Full;
    std::string base_url;

    bool operator==(const StyledDocumentCacheKey&) const = default;
};

// A single exact geometry measurement, tagged with the layout_mutation_version
// at which it was taken. Used only as an approximate, image-isolated deadline
// backstop for synchronous geometry reads (never for the final render); staleness
// is gated by the per-node subtree dirty epoch, never analytically patched.
struct GeometryCacheEntry {
    std::optional<ElementGeometry> geometry;
    std::uint64_t version = 0;
};

// A node's cached cascade result, tagged with the layout-input digest it was
// computed under. Reuse is sound iff the digest still matches (see
// DomDocument::layout_input_digest).
struct ComputedStyleCacheEntry {
    std::uint64_t digest = 0;
    std::optional<ComputedStyle> full;
    std::unordered_map<std::string, std::optional<std::string>> properties;
};

} // namespace

struct Page::Impl {
    LoadOptions options;
    DomDocument document;
    std::shared_ptr<ResourceLoader> loader;
    std::shared_ptr<LayoutEngineFactory> layout_factory;
    std::unique_ptr<JsRuntime> js;
    CookieJar cookie_jar;
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
    // small budget, later post-mutation geometry reads reuse the last exact
    // value seen for that NodeId. An uncached connected element still gets one
    // exact layout read, because JS layout libraries treat zero geometry as a
    // real measurement and can permanently commit bad inline layout state.
    // Cheap pages keep exact synchronous geometry.
    bool styled_document_expensive = false;
    static constexpr long long kExpensiveStyledDocumentUs = 250'000; // 0.25s
    bool geometry_bounded_mode = false;
    int geometry_forced_layout_count = 0;
    long long geometry_forced_layout_us = 0;
    std::uint64_t geometry_cache_forget_version = 0;
    std::unordered_map<NodeId, GeometryCacheEntry> last_known_geometry;
    static constexpr int kGeometryForcedLayoutCountThreshold = 2;
    static constexpr long long kGeometryForcedLayoutBudgetUs = 100'000; // 0.10s
    bool computed_style_property_bounded_mode = false;
    int computed_style_property_forced_rebuild_count = 0;
    long long computed_style_property_forced_rebuild_us = 0;
    std::uint64_t computed_style_cache_forget_version = 0;
    // Cascade results keyed by NodeId, each tagged with the layout-input digest
    // they were computed under. Reused iff the digest still matches.
    std::unordered_map<NodeId, ComputedStyleCacheEntry> computed_style_cache;
    static constexpr int kComputedStylePropertyForcedRebuildCountThreshold = 2;
    static constexpr long long kComputedStylePropertyForcedRebuildBudgetUs = 100'000; // 0.10s

    // Transient absolute-%-width corrections derived from render pass 1 and
    // injected into render pass 2. Populated only inside cached_display_list();
    // never sourced from any cross-time or cross-viewport cache.
    std::vector<DomDocument::LayoutStyleOverride> render_local_overrides;

    // Owns render sub-resource discovery and prefetch. The per-document render
    // resource cache (shared across styled-document rebuilds so a script-heavy
    // page fetches each image/stylesheet once) lives inside it and is dropped via
    // subresource_scanner.reset() when the document/loader identity changes.
    SubresourceScanner subresource_scanner;
    std::shared_ptr<CachingResourceLoader> js_resource_cache;

    explicit Impl(LoadOptions init_options)
        : options(std::move(init_options))
        , loader(std::make_shared<CurlResourceLoader>(options.user_agent))
        , subresource_scanner(document)
    {
#if defined(PAGECORE_ENABLE_RENDERING)
        layout_factory = create_litehtml_layout_engine_factory();
#endif
        // With JS disabled, <noscript> content must parse as the real fallback
        // elements browsers lay out (the flag carries across every parse()).
        document.set_scripting_enabled(options.enable_js);
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
        subresource_scanner.reset();
        js_resource_cache.reset();
        geometry_bounded_mode = false;
        geometry_forced_layout_count = 0;
        geometry_forced_layout_us = 0;
        geometry_cache_forget_version = document.forget_version();
        last_known_geometry.clear();
        computed_style_property_bounded_mode = false;
        computed_style_property_forced_rebuild_count = 0;
        computed_style_property_forced_rebuild_us = 0;
        computed_style_cache_forget_version = document.forget_version();
        computed_style_cache.clear();
        render_local_overrides.clear();
    }

    void ensure_js()
    {
        if (!js) {
            js = std::make_unique<JsRuntime>(document, options, script_resource_loader(), &cookie_jar);
            js->set_computed_style_resolver([this](NodeId node) { return computed_style(node); });
            js->set_computed_style_property_resolver(
                [this](NodeId node, std::string_view property) { return computed_style_property(node, property); });
            js->set_element_geometry_resolver([this](NodeId node) { return element_geometry(node); });
            js->set_viewport_resolver([this] { return last_render_options.viewport; });
            js->install();
        }
    }

    std::shared_ptr<CachingResourceLoader> script_resource_loader()
    {
        if (!js_resource_cache) {
            js_resource_cache = std::make_shared<CachingResourceLoader>(loader, 4096);
        }
        return js_resource_cache;
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
            const auto src = document.get_attribute(script, "src");
            const auto type = document.get_attribute(script, "type");
            if (!is_javascript_script_type(type)) {
                continue;
            }
            const bool module = is_module_script_type(type);
            // Per the HTML spec, `nomodule` suppresses only classic scripts; a
            // module-capable engine must still run `<script type="module" nomodule>`.
            if (!module && document.has_attribute(script, "nomodule")) {
                continue;
            }

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
            PerfScope trace(options.perf_trace, PerfPhase::ResourceLoad, "initial_script_load_all", external_requests.size());
            trace.event().property = "script";
            trace.event().url = current_url.empty() ? options.base_url : current_url;
            auto loader = std::make_shared<CookieAwareResourceLoader>(
                script_resource_loader(),
                &cookie_jar,
                current_url.empty() ? options.base_url : current_url,
                CookieCredentials::SameOrigin);
            // Lenient: a single failed external script (timeout, transport error,
            // SSRF/policy block, oversize) must not abort the whole page load. A
            // failed slot yields an empty body and is simply skipped at execution,
            // matching how a browser continues past a broken <script src>.
            std::vector<ResourceResponse> responses =
                loader->load_all(external_requests, BatchErrorMode::Lenient);
            std::uint64_t loaded_bytes = 0;
            for (std::size_t i = 0; i < responses.size() && i < external_slots.size(); ++i) {
                loaded_bytes += responses[i].body.size();
                PerfEvent response_event{
                    PerfPhase::ResourceLoad,
                    "initial_script_response",
                    0,
                    responses[i].body.size(),
                };
                response_event.property = responses[i].from_cache ? "script:cache" : "script";
                response_event.url = responses[i].url.empty() ? external_requests[i].url : responses[i].url;
                emit_perf_trace(options.perf_trace, std::move(response_event));
                ScriptSource& slot = scripts[external_slots[i]];
                slot.filename = std::move(responses[i].url);
                slot.code = std::move(responses[i].body);
            }
            trace.set_count(loaded_bytes);
        }

        return scripts;
    }

    void execute_scripts()
    {
        ensure_js();
        js->activity_tracker().reset(document.mutation_version());
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
        js->activity_tracker().mark_load_fired();
        js->run_until_ready(PageReadinessOptions{
            options.wait_until,
            options.wait_time,
            options.stable_window,
        });
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

    const PerfTraceCallback& perf_trace_for(const RenderOptions& render_options) const
    {
        return render_options.perf_trace ? render_options.perf_trace : options.perf_trace;
    }

    std::string serialized_html_for_layout(
        const RenderOptions& render_options,
        bool absolute_percent_corrected)
    {
        PerfScope trace(perf_trace_for(render_options), PerfPhase::SerializeHtml, "serialize_html_for_layout");
        // Overrides are ONLY the render-local absolute-%-width corrections derived
        // within cached_display_list(); reads never inject anything.
        static const std::vector<DomDocument::LayoutStyleOverride> kNoOverrides;
        const auto& style_overrides = absolute_percent_corrected ? render_local_overrides : kNoOverrides;
        trace.event().property = "absolute_percent_corrected:" + std::to_string(absolute_percent_corrected ? 1 : 0)
            + ";style_overrides:" + std::to_string(style_overrides.size());
        std::string html = document.serialize_html_for_layout(
            options.enable_js,
            style_overrides);
        trace.set_count(html.size());
        return html;
    }

    // Loads the current document into `engine`, honouring the configured
    // layout tree input. DirectDom hands the live DOM to the engine
    // (LayoutEngine::load_dom, no serialize/re-parse); engines without
    // direct-DOM support fall back to the serialized-HTML round trip.
    void load_engine_document(
        LayoutEngine& engine,
        const RenderOptions& render_options,
        bool absolute_percent_corrected)
    {
        static const std::vector<DomDocument::LayoutStyleOverride> kNoOverrides;
        if (options.layout_tree_input == LayoutTreeInput::DirectDom) {
            const auto& style_overrides = absolute_percent_corrected ? render_local_overrides : kNoOverrides;
            const std::string base = render_base_url(render_options);
            LayoutEngine::DomLayoutRequest request;
            request.document = &document;
            request.base_url = base;
            request.omit_js_disabled_content = options.enable_js;
            request.style_overrides = &style_overrides;

            // Time load_dom manually and emit the "load_dom" event only when it
            // actually loaded the document. A PerfScope here would fire even on
            // the fallback path, emitting a spurious "load_dom" alongside the
            // real "load_html" for engines that don't support direct DOM input.
            const auto t0 = std::chrono::steady_clock::now();
            const bool loaded = engine.load_dom(request);
            if (loaded) {
                PerfEvent event{PerfPhase::LitehtmlLoadHtml, "load_dom",
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - t0).count(),
                    0};
                event.property = "absolute_percent_corrected:"
                    + std::to_string(absolute_percent_corrected ? 1 : 0)
                    + ";style_overrides:" + std::to_string(style_overrides.size());
                emit_perf_trace(perf_trace_for(render_options), std::move(event));
                return;
            }
        }

        std::string html = serialized_html_for_layout(render_options, absolute_percent_corrected);
        PerfScope trace(perf_trace_for(render_options), PerfPhase::LitehtmlLoadHtml, "load_html", html.size());
        engine.load_html(html, render_base_url(render_options));
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

    StyledDocumentCacheKey styled_document_cache_key(
        const RenderOptions& render_options,
        bool absolute_percent_corrected,
        LayoutResourceMode resource_mode) const
    {
        return StyledDocumentCacheKey{
            document.layout_mutation_version(),
            render_options.viewport.width,
            render_options.viewport.height,
            render_options.viewport.device_scale_factor,
            render_options.load_external_resources,
            absolute_percent_corrected,
            resource_mode,
            render_base_url(render_options),
        };
    }

    std::string styled_document_cache_reason(const StyledDocumentCacheKey& key) const
    {
        if (!styled_document_valid) {
            return "empty";
        }
        if (styled_document_key.layout_mutation_version != key.layout_mutation_version) {
            return "layout_mutation_version_changed";
        }
        if (styled_document_key.viewport_width != key.viewport_width
            || styled_document_key.viewport_height != key.viewport_height) {
            return "viewport_changed";
        }
        if (styled_document_key.device_scale_factor != key.device_scale_factor) {
            return "scale_changed";
        }
        if (styled_document_key.load_external_resources != key.load_external_resources) {
            return "load_external_resources_changed";
        }
        if (styled_document_key.absolute_percent_corrected != key.absolute_percent_corrected) {
            return "layout_serialization_mode_changed";
        }
        if (styled_document_key.resource_mode != key.resource_mode) {
            return "resource_mode_changed";
        }
        if (styled_document_key.base_url != key.base_url) {
            return "base_url_changed";
        }
        return "valid";
    }

    bool has_current_layout(
        const RenderOptions& render_options,
        bool absolute_percent_corrected,
        LayoutResourceMode resource_mode) const
    {
        return styled_document_valid
            && styled_document_laid_out
            && styled_document_key == styled_document_cache_key(render_options, absolute_percent_corrected, resource_mode);
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

    // The border-box width an absolute %-width element WOULD get if litehtml
    // resolved the percentage against its positioned containing block's used width.
    // Kept proportional to litehtml's OWN (sometimes under-measured) ancestor
    // widths, so a JS grid library that divides its container width by an item's
    // width computes the same column count litehtml's boxes imply — which, for
    // Bootstrap/Isotope grids, is the browser's column count even when litehtml's
    // absolute box-model differs. Used only for synchronous reads, never the paint.
    std::optional<float> absolute_percentage_border_box_width(LayoutEngine& engine, NodeId node)
    {
        const std::string node_key = std::to_string(node);
        const auto position = engine.computed_style_property(node_key, "position");
        if (!position || ascii_lower(trim_ascii(*position)) != "absolute") {
            return std::nullopt;
        }

        const auto box_sizing = engine.computed_style_property(node_key, "box-sizing");
        if (!box_sizing || ascii_lower(trim_ascii(*box_sizing)) != "border-box") {
            return std::nullopt;
        }

        const auto width = engine.computed_style_property(node_key, "width");
        if (!width) {
            return std::nullopt;
        }
        const auto percent = parse_css_percentage(*width);
        if (!percent) {
            return std::nullopt;
        }

        std::optional<ElementGeometry> containing_block;
        const NodeId root = document.document_node();
        for (NodeId parent = document.parent_node(node);
             parent != kInvalidNodeId && parent != root;
             parent = document.parent_node(parent)) {
            const std::string parent_key = std::to_string(parent);
            const auto parent_position = engine.computed_style_property(parent_key, "position");
            if (!parent_position) {
                continue;
            }
            const std::string normalized = ascii_lower(trim_ascii(*parent_position));
            if (normalized.empty() || normalized == "static") {
                continue;
            }
            containing_block = engine.element_geometry(parent_key);
            break;
        }
        if (!containing_block) {
            return std::nullopt;
        }

        const float corrected_width = containing_block->padding_box.width * (*percent / 100.0f);
        if (corrected_width <= 0.0f) {
            return std::nullopt;
        }
        return corrected_width;
    }

    std::optional<ElementGeometry> adjust_absolute_percentage_width_geometry(
        LayoutEngine& engine,
        NodeId node,
        std::optional<ElementGeometry> geometry)
    {
        if (!geometry) {
            return std::nullopt;
        }

        const auto corrected_width = absolute_percentage_border_box_width(engine, node);
        if (!corrected_width) {
            return geometry;
        }

        ElementGeometry adjusted = *geometry;
        const float border_delta = adjusted.border_box.width - adjusted.padding_box.width;
        adjusted.border_box.width = *corrected_width;
        adjusted.padding_box.width = std::max(0.0f, *corrected_width - border_delta);
        return adjusted;
    }

    bool should_snapshot_geometry_after_forced_layout(long long elapsed_us) const
    {
        const long long normalized_elapsed = std::max<long long>(0, elapsed_us);
        return styled_document_expensive
            || normalized_elapsed > kExpensiveStyledDocumentUs
            || (geometry_forced_layout_count + 1 >= kGeometryForcedLayoutCountThreshold
                && geometry_forced_layout_us + normalized_elapsed > kGeometryForcedLayoutBudgetUs);
    }

    // Caches every connected element's exact geometry at the current version, so
    // later bounded reads can answer without another forced layout. Used only as
    // the approximate deadline backstop; never feeds the paint.
    void snapshot_layout_geometry(LayoutEngine& engine)
    {
        sync_geometry_cache_for_forget_version();
        const NodeId root = document.document_node();
        if (root == kInvalidNodeId) {
            return;
        }
        const std::uint64_t version = document.layout_mutation_version();
        for (NodeId node : document.query_selector_all(root, "*")) {
            if (!document.is_connected(node)) {
                last_known_geometry.erase(node);
                continue;
            }
            auto geometry = adjust_absolute_percentage_width_geometry(
                engine, node, engine.element_geometry(std::to_string(node)));
            last_known_geometry[node] = GeometryCacheEntry{std::move(geometry), version};
        }
    }

    void remember_geometry(NodeId node, std::optional<ElementGeometry> geometry)
    {
        sync_geometry_cache_for_forget_version();
        if (!document.is_connected(node)) {
            last_known_geometry.erase(node);
            return;
        }
        last_known_geometry[node] = GeometryCacheEntry{
            std::move(geometry),
            document.layout_mutation_version(),
        };
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

    // Render-local correction, derived entirely from one pass and universal (no
    // per-site heuristic, no containing-block guessing).
    //
    // litehtml sizes a `position:absolute; width:%` element's OWN box, but does not
    // establish it as a definite-width containing block, so its percentage-width
    // children collapse. The fix is to make that width EXPLICIT — and the only value
    // that is guaranteed correct is litehtml's OWN computed width for the element.
    // We pin exactly that (never a formula, never larger or smaller), so a
    // correctly-sized element is untouched (the second pass is a no-op for it) and
    // only its collapsing children are repaired. Fixing litehtml's own sizing of
    // such elements, if it is ever wrong, belongs in litehtml, not here.
    //
    // The engine collects these with typed getters in a single tree walk, so the
    // common case (no such elements) returns empty with no string lookups or Lexbor
    // query. We only parse the injected data-pc-sid back into a NodeId and re-check
    // connectivity before turning each into a layout override.
    std::vector<DomDocument::LayoutStyleOverride> compute_render_local_absolute_percent_overrides(LayoutEngine& engine)
    {
        std::vector<DomDocument::LayoutStyleOverride> overrides;
        const auto engine_overrides = engine.collect_absolute_percent_width_overrides();
        overrides.reserve(engine_overrides.size());
        for (const auto& engine_override : engine_overrides) {
            // node_key is std::to_string(NodeId), injected as data-pc-sid during
            // serialization; parse it back exactly (whole string, non-zero id).
            NodeId node = kInvalidNodeId;
            const char* begin = engine_override.node_key.data();
            const char* end = begin + engine_override.node_key.size();
            const auto parsed = std::from_chars(begin, end, node);
            if (parsed.ec != std::errc{} || parsed.ptr != end || node == kInvalidNodeId) {
                continue;
            }
            if (!document.is_connected(node)) {
                continue;
            }
            overrides.push_back(DomDocument::LayoutStyleOverride{
                node,
                "width:" + std::to_string(engine_override.border_box_width_px) + "px;",
            });
        }
        return overrides;
    }

    std::uint64_t computed_style_digest(NodeId node) const
    {
        const std::uint64_t digest = document.layout_input_digest(
            node,
            last_render_options.viewport.width,
            last_render_options.viewport.height,
            last_render_options.viewport.device_scale_factor);
        // layout_input_digest returns 0 to signal "no such node"; preserve it.
        if (digest == 0) {
            return 0;
        }
        // Fold the base URL: relative @import/url() in the cascade resolve against
        // it, so the same DOM at the same viewport but a different base can yield a
        // different computed style. The styled-document cache already keys on it.
        std::uint64_t combined = digest;
        const std::size_t base_hash = std::hash<std::string>{}(render_base_url(last_render_options));
        combined ^= static_cast<std::uint64_t>(base_hash) + 0x9e3779b97f4a7c15ULL
            + (combined << 6) + (combined >> 2);
        return combined == 0 ? 0x9e3779b97f4a7c15ULL : combined;
    }

    void sync_computed_style_cache_for_forget_version()
    {
        const std::uint64_t current_forget_version = document.forget_version();
        if (computed_style_cache_forget_version == current_forget_version) {
            return;
        }
        computed_style_cache_forget_version = current_forget_version;
        computed_style_cache.clear();
    }

    void remember_computed_style_property(
        NodeId node,
        std::uint64_t digest,
        std::string_view property,
        std::optional<std::string> value)
    {
        sync_computed_style_cache_for_forget_version();
        if (!document.is_connected(node)) {
            computed_style_cache.erase(node);
            return;
        }
        auto& entry = computed_style_cache[node];
        if (entry.digest != digest) {
            entry = ComputedStyleCacheEntry{};
            entry.digest = digest;
        }
        entry.properties[std::string(property)] = std::move(value);
    }

    void remember_computed_style_full(NodeId node, std::uint64_t digest, const std::optional<ComputedStyle>& style)
    {
        sync_computed_style_cache_for_forget_version();
        if (!document.is_connected(node)) {
            computed_style_cache.erase(node);
            return;
        }
        auto& entry = computed_style_cache[node];
        if (entry.digest != digest) {
            entry = ComputedStyleCacheEntry{};
            entry.digest = digest;
        }
        entry.full = style;
    }

    // Sound reuse: returns true and sets `out` iff node N has a value cached under
    // an unchanged digest (from a stored property or the full cascade). A digest
    // mismatch means N's cascade inputs changed, so no reuse.
    bool try_reuse_computed_style_property(
        NodeId node,
        std::uint64_t digest,
        std::string_view property,
        std::optional<std::string>& out)
    {
        sync_computed_style_cache_for_forget_version();
        const auto found = computed_style_cache.find(node);
        if (found == computed_style_cache.end() || found->second.digest != digest) {
            return false;
        }
        const auto& entry = found->second;
        const auto property_found = entry.properties.find(std::string(property));
        if (property_found != entry.properties.end()) {
            out = property_found->second;
            return true;
        }
        if (entry.full) {
            for (const auto& [name, value] : entry.full->properties) {
                if (name == property) {
                    out = value;
                    return true;
                }
            }
            out = std::nullopt;  // absent from a full cascade means genuinely unset
            return true;
        }
        return false;
    }

    void note_computed_style_property_forced_rebuild(long long elapsed_us)
    {
        ++computed_style_property_forced_rebuild_count;
        computed_style_property_forced_rebuild_us += std::max<long long>(0, elapsed_us);
        if (styled_document_expensive
            || (computed_style_property_forced_rebuild_count >= kComputedStylePropertyForcedRebuildCountThreshold
                && computed_style_property_forced_rebuild_us > kComputedStylePropertyForcedRebuildBudgetUs)) {
            computed_style_property_bounded_mode = true;
        }
    }

    // Deadline backstop, explicitly approximate: inline value -> last-known cached
    // value (regardless of digest) -> CSS initial-value table.
    std::optional<std::string> bounded_computed_style_property(NodeId node, std::string_view property)
    {
        sync_computed_style_cache_for_forget_version();
        if (!document.is_connected(node)) {
            computed_style_cache.erase(node);
            return std::nullopt;
        }

        if (const auto inline_style = document.get_attribute(node, "style")) {
            if (auto value = inline_style_property_value(*inline_style, property)) {
                remember_computed_style_property(node, computed_style_digest(node), property, value);
                return value;
            }
        }

        if (const auto found = computed_style_cache.find(node); found != computed_style_cache.end()) {
            const auto property_found = found->second.properties.find(std::string(property));
            if (property_found != found->second.properties.end() && property_found->second) {
                return property_found->second;
            }
            if (found->second.full) {
                for (const auto& [name, value] : found->second.full->properties) {
                    if (name == property) {
                        return value;
                    }
                }
            }
        }

        auto value = default_computed_style_property_value(property, document.tag_name(node));
        remember_computed_style_property(node, computed_style_digest(node), property, value);
        return value;
    }

    std::unique_ptr<LayoutEngine> build_layout(
        const RenderOptions& render_options,
        LayoutResourceMode resource_mode = LayoutResourceMode::Full)
    {
        if (!layout_factory) {
            throw std::runtime_error("no layout engine factory configured; build with PAGECORE_ENABLE_RENDERING or set one explicitly");
        }

        auto layout = layout_factory->create_layout_engine();
        if (!layout) {
            throw std::runtime_error("layout engine factory returned null");
        }

        const std::string effective_base = effective_base_url(render_options);
        auto render_loader = subresource_scanner.prepare_render_loader(
            loader,
            cookie_jar,
            render_options,
            resource_mode,
            render_base_url(render_options),
            effective_base,
            perf_trace_for(render_options));
        auto font_environment = subresource_scanner.prepare_font_environment(
            render_options,
            render_loader,
            resource_mode,
            effective_base);
        layout->set_viewport(render_options.viewport);
        layout->set_resource_loader(std::move(render_loader));
        layout->set_font_environment(std::move(font_environment));
        load_engine_document(*layout, render_options, /*absolute_percent_corrected=*/false);
        {
            PerfScope trace(perf_trace_for(render_options), PerfPhase::LitehtmlLayout, "layout");
            layout->layout();
            trace.set_count(layout->display_list().commands.size());
        }
        return layout;
    }

    // Builds (or reuses) the litehtml document backing both the rendered
    // DisplayList and getComputedStyle(), without running layout() on it —
    // callers decide whether they need a full layout pass or just the
    // cascade (compute_styles_only()).
    // Attempts to satisfy `key` by patching the current styled document's inline
    // styles in place instead of rebuilding it. All of the following must hold,
    // otherwise the caller falls back to a full rebuild:
    //  1. DirectDom input and a valid current styled document.
    //  2. `key` matches the current key in every field except the layout version,
    //     and neither is the absolute-%-corrected second pass (which never patches).
    //  3. the layout-mutation journal since the built version is complete,
    //  4. and holds only inline-style mutations,
    //  5. with "style" not referenced by any attribute selector,
    //  6. and no old/new value carrying a structure-affecting property.
    //  7. Every touched node is still connected; its patch is read from the
    //     current DOM style value (multi-write coalescing is free, last wins).
    //  8. The engine applies the whole batch in place.
    bool try_patch_styled_document(
        const RenderOptions& render_options,
        const StyledDocumentCacheKey& key,
        bool absolute_percent_corrected,
        LayoutResourceMode resource_mode)
    {
        (void) resource_mode;

        // Gate 1.
        if (options.layout_tree_input != LayoutTreeInput::DirectDom
            || !styled_document_valid || !styled_document) {
            return false;
        }
        // Gate 2: pass-2 never patches; every other key field must already match.
        if (absolute_percent_corrected
            || key.absolute_percent_corrected
            || styled_document_key.absolute_percent_corrected) {
            return false;
        }
        StyledDocumentCacheKey version_normalized = styled_document_key;
        version_normalized.layout_mutation_version = key.layout_mutation_version;
        if (!(version_normalized == key)) {
            return false;
        }

        // Gate 3: a complete journal of everything since the built version.
        auto journal = document.layout_mutations_since(styled_document_key.layout_mutation_version);
        if (!journal.complete || journal.records.empty()) {
            return false;
        }
        // Gates 4 + 6.
        for (const auto& record : journal.records) {
            if (record.kind != LayoutMutationRecord::Kind::InlineStyle) {
                return false;
            }
            if (css_declarations_require_tree_rebuild(record.old_value)
                || css_declarations_require_tree_rebuild(record.new_value)) {
                return false;
            }
        }
        // Gate 5.
        if (document.is_layout_sensitive_attribute("style")) {
            return false;
        }

        // Gate 7: unique connected nodes (last-write-wins), current DOM value.
        std::vector<NodeId> order;
        std::unordered_set<NodeId> seen;
        for (const auto& record : journal.records) {
            if (seen.insert(record.node).second) {
                order.push_back(record.node);
            }
        }
        std::vector<LayoutEngine::InlineStylePatch> patches;
        patches.reserve(order.size());
        for (const NodeId node : order) {
            if (!document.is_connected(node)) {
                return false;
            }
            const auto style = document.get_attribute(node, "style");
            if (style && css_declarations_require_tree_rebuild(*style)) {
                return false;
            }
            patches.push_back(LayoutEngine::InlineStylePatch{std::to_string(node), style.value_or(std::string())});
        }

        // Gate 8: apply in place, timed and traced as the "patched" outcome.
        bool applied = false;
        {
            PerfScope trace(
                perf_trace_for(render_options), PerfPhase::LitehtmlLoadHtml, "patch_inline_styles", patches.size());
            trace.event().property = "outcome:patched";
            applied = styled_document->apply_inline_style_patches(patches);
        }
        if (!applied) {
            return false;
        }

        // Promote the cached key's version and force a fresh layout() pass; every
        // downstream consumer then behaves exactly as after a rebuild.
        styled_document_key.layout_mutation_version = key.layout_mutation_version;
        styled_document_laid_out = false;
        return true;
    }

    LayoutEngine& ensure_styled_document(
        const RenderOptions& render_options,
        bool absolute_percent_corrected,
        LayoutResourceMode resource_mode)
    {
        StyledDocumentCacheKey key = styled_document_cache_key(render_options, absolute_percent_corrected, resource_mode);

        if (styled_document_valid && styled_document_key == key) {
            return *styled_document;
        }

        if (try_patch_styled_document(render_options, key, absolute_percent_corrected, resource_mode)) {
            return *styled_document;
        }

        if (!layout_factory) {
            throw std::runtime_error("no layout engine factory configured; build with PAGECORE_ENABLE_RENDERING or set one explicitly");
        }

        auto engine = layout_factory->create_layout_engine();
        if (!engine) {
            throw std::runtime_error("layout engine factory returned null");
        }

        const std::string effective_base = effective_base_url(render_options);
        auto render_loader = subresource_scanner.prepare_render_loader(
            loader,
            cookie_jar,
            render_options,
            resource_mode,
            render_base_url(render_options),
            effective_base,
            perf_trace_for(render_options));
        auto font_environment = subresource_scanner.prepare_font_environment(
            render_options,
            render_loader,
            resource_mode,
            effective_base);
        engine->set_viewport(render_options.viewport);
        engine->set_resource_loader(std::move(render_loader));
        engine->set_font_environment(std::move(font_environment));
        {
            const auto t0 = std::chrono::steady_clock::now();
            load_engine_document(*engine, render_options, absolute_percent_corrected);
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
    LayoutEngine& ensure_layout(
        const RenderOptions& render_options,
        bool absolute_percent_corrected,
        LayoutResourceMode resource_mode)
    {
        auto& engine = ensure_styled_document(render_options, absolute_percent_corrected, resource_mode);
        if (!styled_document_laid_out) {
            const auto t0 = std::chrono::steady_clock::now();
            {
                PerfScope trace(perf_trace_for(render_options), PerfPhase::LitehtmlLayout, "layout");
                engine.layout();
                trace.set_count(engine.display_list().commands.size());
            }
            const auto layout_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - t0).count();
            if (layout_us > kExpensiveStyledDocumentUs) {
                styled_document_expensive = true;
            }
            styled_document_laid_out = true;
        }
        return engine;
    }

    // === Cache soundness contract ===
    // This cache never influences the final rendered image: cached_display_list()
    // always performs an exact Full layout at the requested viewport/version
    // (optionally a second same-viewport/same-version pass that injects
    // *render-locally derived* absolute-%-width corrections). No value measured
    // during a synchronous JS read is ever fed back into a paint. Read-time caches
    // exist only to answer synchronous getComputedStyle/getBoundingClientRect
    // cheaply between DOM mutations. Computed-style reuse is *sound*: a cached value
    // is reused for node N iff N's layout-input digest is unchanged, where the
    // digest is a conservative superset of every DOM-derived input to N's cascade.
    // Geometry read-back across a version bump is *approximate by design*: the last
    // exact measurement as-is under budget pressure (approximate-nonzero — a known
    // value is never turned into null, because JS grid libraries collapse on null/
    // zero geometry), or nullopt only when uncached/disconnected. It is never
    // analytically patched; the per-node subtree dirty epoch only labels the value
    // sound vs approximate. Product correctness is guaranteed by the exact final
    // render, not by these caches.
    const DisplayList& cached_display_list(const RenderOptions& render_options)
    {
        last_render_options = render_options;
        render_local_overrides.clear();

        // Pass 1: exact Full layout at the requested viewport, no injected widths.
        auto& first_pass = ensure_layout(
            render_options,
            /*absolute_percent_corrected=*/false,
            LayoutResourceMode::Full);

        auto overrides = compute_render_local_absolute_percent_overrides(first_pass);
        if (overrides.empty()) {
            return first_pass.display_list();
        }

        // Pass 2: same viewport/version, re-laid out with the corrections litehtml
        // gets wrong for position:absolute; width:% elements — derived entirely
        // from pass 1, never from any cross-time or cross-viewport cache.
        render_local_overrides = std::move(overrides);
        auto& second_pass = ensure_layout(
            render_options,
            /*absolute_percent_corrected=*/true,
            LayoutResourceMode::Full);
        return second_pass.display_list();
    }

    // Shared cache-state prologue for the computed-style read APIs
    // (computed_style / computed_style_property), which apply an identical
    // cache-key and soundness rule. Keeping it in one place stops the two paths
    // from silently drifting apart. element_geometry deliberately does NOT use
    // this: it keys on layout validity (has_current_layout), not styled-document
    // validity, so its hit predicate is genuinely different.
    struct StyleCacheState {
        StyledDocumentCacheKey key;
        StyledDocumentCacheKey full_key;
        bool full_cache_hit = false;
        bool stylesheets_cache_hit = false;
        std::string cache_reason;

        bool cache_hit() const { return cache_reason == "valid"; }
    };

    StyleCacheState styled_document_style_cache_state() const
    {
        StyleCacheState state;
        state.key = styled_document_cache_key(
            last_render_options, /*absolute_percent_corrected=*/false, LayoutResourceMode::StylesheetsOnly);
        state.full_key = styled_document_cache_key(
            last_render_options, /*absolute_percent_corrected=*/false, LayoutResourceMode::Full);
        state.full_cache_hit = styled_document_valid && styled_document_key == state.full_key;
        state.stylesheets_cache_hit = styled_document_valid && styled_document_key == state.key;
        state.cache_reason = (state.full_cache_hit || state.stylesheets_cache_hit)
            ? "valid"
            : styled_document_cache_reason(state.key);
        return state;
    }

    std::optional<ComputedStyle> computed_style(NodeId node)
    {
        const StyleCacheState cache = styled_document_style_cache_state();
        PerfScope trace(perf_trace_for(last_render_options), PerfPhase::ComputedStyle, "computed_style", 1);
        trace.event().node_id = node;
        trace.event().mutation_version = document.mutation_version();
        trace.event().layout_mutation_version = document.layout_mutation_version();
        trace.event().styled_document_cache_hit = cache.cache_hit();
        trace.event().styled_document_cache_reason = cache.cache_reason;

        if (!document.is_connected(node)) {
            computed_style_cache.erase(node);
            return std::nullopt;
        }

        // Current-version styled document is valid -> exact cascade.
        if (cache.full_cache_hit || cache.stylesheets_cache_hit) {
            auto& engine = cache.full_cache_hit
                ? *styled_document
                : ensure_styled_document(last_render_options, false, LayoutResourceMode::StylesheetsOnly);
            engine.compute_styles_only();
            auto style = engine.computed_style(std::to_string(node));
            remember_computed_style_full(node, computed_style_digest(node), style);
            trace.set_count(style ? style->properties.size() : 0);
            return style;
        }

        // Digest unchanged -> reuse the cached full cascade (sound).
        const std::uint64_t digest = computed_style_digest(node);
        if (const auto found = computed_style_cache.find(node);
            found != computed_style_cache.end() && found->second.digest == digest && found->second.full) {
            trace.event().styled_document_cache_hit = true;
            trace.event().styled_document_cache_reason = "digest_reuse:" + cache.cache_reason;
            trace.set_count(found->second.full->properties.size());
            return found->second.full;
        }

        // Exact rebuild.
        auto& engine = ensure_styled_document(last_render_options, false, LayoutResourceMode::StylesheetsOnly);
        engine.compute_styles_only();
        auto style = engine.computed_style(std::to_string(node));
        remember_computed_style_full(node, digest, style);
        trace.set_count(style ? style->properties.size() : 0);
        return style;
    }

    std::optional<std::string> computed_style_property(NodeId node, std::string_view property)
    {
        const StyleCacheState cache = styled_document_style_cache_state();
        const bool cache_hit = cache.cache_hit();
        PerfScope trace(perf_trace_for(last_render_options), PerfPhase::ComputedStyle, "computed_style_property", 1);
        trace.event().node_id = node;
        trace.event().mutation_version = document.mutation_version();
        trace.event().layout_mutation_version = document.layout_mutation_version();
        trace.event().styled_document_cache_hit = cache_hit;
        trace.event().styled_document_cache_reason = cache.cache_reason;
        trace.event().property = std::string(property);

        if (!document.is_connected(node)) {
            computed_style_cache.erase(node);
            return std::nullopt;
        }

        // Current-version styled document is valid -> exact cascade.
        if (cache_hit) {
            auto& engine = cache.full_cache_hit
                ? *styled_document
                : ensure_styled_document(last_render_options, false, LayoutResourceMode::StylesheetsOnly);
            engine.compute_styles_only();
            auto value = engine.computed_style_property(std::to_string(node), property);
            remember_computed_style_property(node, computed_style_digest(node), property, value);
            trace.set_count(value ? 1 : 0);
            return value;
        }

        // Digest unchanged -> sound reuse without a rebuild. This is the free hot
        // path across version bumps that don't touch N's cascade inputs.
        const std::uint64_t digest = computed_style_digest(node);
        std::optional<std::string> reused;
        if (try_reuse_computed_style_property(node, digest, property, reused)) {
            trace.event().styled_document_cache_hit = true;
            trace.event().styled_document_cache_reason = "digest_reuse:" + cache.cache_reason;
            trace.set_count(reused ? 1 : 0);
            return reused;
        }

        // Within budget -> exact rebuild.
        if (!(computed_style_property_bounded_mode || styled_document_expensive)) {
            const auto t0 = std::chrono::steady_clock::now();
            auto& engine = ensure_styled_document(last_render_options, false, LayoutResourceMode::StylesheetsOnly);
            engine.compute_styles_only();
            auto value = engine.computed_style_property(std::to_string(node), property);
            const auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - t0).count();
            note_computed_style_property_forced_rebuild(elapsed_us);
            remember_computed_style_property(node, digest, property, value);
            trace.set_count(value ? 1 : 0);
            return value;
        }

        // Deadline backstop -> approximate (inline / last-known / CSS defaults).
        computed_style_property_bounded_mode = true;
        trace.event().styled_document_cache_reason = "bounded_mode:" + cache.cache_reason;
        auto value = bounded_computed_style_property(node, property);
        trace.set_count(value && !value->empty() ? 1 : 0);
        return value;
    }

    std::optional<ElementGeometry> element_geometry(NodeId node)
    {
        const StyledDocumentCacheKey key = styled_document_cache_key(
            last_render_options, /*absolute_percent_corrected=*/false, LayoutResourceMode::StylesheetsOnly);
        const bool full_layout_hit = has_current_layout(
            last_render_options, false, LayoutResourceMode::Full);
        const bool stylesheets_layout_hit = has_current_layout(
            last_render_options, false, LayoutResourceMode::StylesheetsOnly);
        const std::string cache_reason = full_layout_hit ? "valid" : styled_document_cache_reason(key);
        PerfScope trace(perf_trace_for(last_render_options), PerfPhase::Geometry, "element_geometry", 1);
        trace.event().node_id = node;
        trace.event().mutation_version = document.mutation_version();
        trace.event().layout_mutation_version = document.layout_mutation_version();
        trace.event().styled_document_cache_hit = full_layout_hit || stylesheets_layout_hit;
        trace.event().styled_document_cache_reason = cache_reason;
        sync_geometry_cache_for_forget_version();
        if (!document.is_connected(node)) {
            last_known_geometry.erase(node);
            return std::nullopt;
        }

        const std::string node_key = std::to_string(node);

        // A layout for the current version exists -> exact geometry from it.
        if (full_layout_hit || stylesheets_layout_hit) {
            auto geometry = adjust_absolute_percentage_width_geometry(
                *styled_document, node, styled_document->element_geometry(node_key));
            remember_geometry(node, geometry);
            return geometry;
        }

        // Not yet in bounded mode -> force one exact layout. This runs even when the
        // styled document is expensive: the first such layout snapshots EVERY
        // element's geometry (see should_snapshot_geometry_after_forced_layout),
        // seeding the cache before note_geometry_forced_layout() trips bounded mode.
        // Without this, an expensive page (e.g. one with large stylesheets) would
        // enter bounded mode before any element was ever measured, and JS grid
        // libraries (Isotope/Masonry) that read every item's geometry during load
        // would see only nulls and collapse the whole grid to (0,0).
        if (!geometry_bounded_mode) {
            const auto t0 = std::chrono::steady_clock::now();
            auto& engine = ensure_layout(last_render_options, false, LayoutResourceMode::StylesheetsOnly);
            const auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - t0).count();
            auto geometry = adjust_absolute_percentage_width_geometry(
                engine, node, engine.element_geometry(node_key));
            if (should_snapshot_geometry_after_forced_layout(elapsed_us)) {
                snapshot_layout_geometry(engine);
            }
            remember_geometry(node, geometry);
            note_geometry_forced_layout(elapsed_us);
            return geometry;
        }

        // Deadline backstop: return the last exact measurement AS-IS (never
        // analytically patched; never touches the paint). This is approximate by
        // design and returns a non-null value whenever one is known, because JS
        // layout libraries (Isotope/Masonry/jQuery) treat null/zero geometry as a
        // real measurement and permanently commit collapsed layout state. The
        // per-node subtree dirty epoch only labels the value sound vs approximate.
        geometry_bounded_mode = true;
        const auto found = last_known_geometry.find(node);
        if (found == last_known_geometry.end() || !found->second.geometry) {
            trace.event().styled_document_cache_reason = "bounded_mode_uncached:" + cache_reason;
            return found == last_known_geometry.end() ? std::nullopt : found->second.geometry;
        }
        const bool stale = document.subtree_dirty_layout_version(node) > found->second.version;
        trace.event().styled_document_cache_reason =
            (stale ? "bounded_mode_approx:" : "bounded_mode_sound:") + cache_reason;
        return found->second.geometry;
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
    const auto t0 = std::chrono::steady_clock::now();
    ResourceRequest request{std::string(url), ResourceKind::Document};
    request = impl_->cookie_jar.with_cookie_header(std::move(request), CookieCredentials::Include, url);
    auto response = impl_->loader->load(request);
    const std::string document_url = response.url.empty() ? std::string(url) : response.url;
    impl_->cookie_jar.store_from_response(request.url, response, CookieCredentials::Include, document_url);
    const auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - t0).count();
    PerfEvent event{PerfPhase::ResourceLoad, "document_load", elapsed_us, response.body.size()};
    event.property = "document";
    event.url = response.url.empty() ? std::string(url) : response.url;
    emit_perf_trace(impl_->options.perf_trace, std::move(event));
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

bool Page::run_until_ready(PageReadinessOptions options)
{
    if (!impl_->js) {
        return true;
    }
    return impl_->js->run_until_ready(options);
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
    PerfTraceCallback perf_trace = render_options.perf_trace ? render_options.perf_trace : impl_->options.perf_trace;
    DisplayList display = display_list(render_options);
    PerfScope trace(perf_trace, PerfPhase::Raster, "raster", display.commands.size());
    return backend.render(display);
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

std::optional<std::string> Page::computed_style_property(NodeId node, std::string_view property) const
{
    return impl_->computed_style_property(node, property);
}

std::optional<ElementGeometry> Page::element_geometry(NodeId node) const
{
    return impl_->element_geometry(node);
}

} // namespace pagecore
