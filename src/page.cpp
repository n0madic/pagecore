#include "pagecore/page.hpp"

#include "js_runtime.hpp"
#include "pagecore/render.hpp"
#include "pagecore/resource_loader.hpp"

#include <memory>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
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

bool is_javascript_script_type(const std::optional<std::string>& type)
{
    if (!type || type->empty()) {
        return true;
    }

    const std::string normalized = normalized_script_type(*type);
    if (normalized.empty()) {
        return true;
    }

    return normalized == "module"
        || normalized == "text/javascript"
        || normalized == "application/javascript"
        || normalized == "application/ecmascript"
        || normalized == "text/ecmascript"
        || normalized == "application/x-javascript"
        || normalized == "text/jscript";
}

bool is_module_script_type(const std::optional<std::string>& type)
{
    return type && normalized_script_type(*type) == "module";
}

bool starts_with_at(std::string_view value, std::size_t offset, std::string_view prefix)
{
    return offset <= value.size()
        && prefix.size() <= value.size() - offset
        && value.substr(offset, prefix.size()) == prefix;
}

std::string remove_head_direct_text_nodes(std::string html)
{
    std::string lower(html);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });

    const std::size_t head_start = lower.find("<head");
    if (head_start == std::string::npos) {
        return html;
    }
    const std::size_t head_open_end = lower.find('>', head_start);
    const std::size_t head_close = lower.find("</head", head_open_end == std::string::npos ? head_start : head_open_end);
    if (head_open_end == std::string::npos || head_close == std::string::npos || head_open_end >= head_close) {
        return html;
    }

    std::string out;
    out.reserve(html.size());
    out.append(html, 0, head_open_end + 1);

    std::size_t pos = head_open_end + 1;
    while (pos < head_close) {
        if (html[pos] != '<') {
            pos = html.find('<', pos);
            if (pos == std::string::npos || pos > head_close) {
                pos = head_close;
            }
            continue;
        }

        if (starts_with_at(lower, pos, "<!--")) {
            const std::size_t comment_end = lower.find("-->", pos + 4);
            const std::size_t end = comment_end == std::string::npos ? head_close : std::min(head_close, comment_end + 3);
            out.append(html, pos, end - pos);
            pos = end;
            continue;
        }

        const std::size_t tag_end = lower.find('>', pos);
        if (tag_end == std::string::npos || tag_end >= head_close) {
            break;
        }

        out.append(html, pos, tag_end + 1 - pos);

        std::size_t name_start = pos + 1;
        if (name_start < tag_end && lower[name_start] == '/') {
            pos = tag_end + 1;
            continue;
        }
        while (name_start < tag_end && std::isspace(static_cast<unsigned char>(lower[name_start]))) {
            ++name_start;
        }
        std::size_t name_end = name_start;
        while (name_end < tag_end
            && (std::isalnum(static_cast<unsigned char>(lower[name_end])) || lower[name_end] == '-')) {
            ++name_end;
        }

        const std::string_view name(lower.data() + name_start, name_end - name_start);
        if (name == "script" || name == "style" || name == "title") {
            const std::string closing = "</" + std::string(name);
            const std::size_t closing_start = lower.find(closing, tag_end + 1);
            if (closing_start != std::string::npos && closing_start < head_close) {
                const std::size_t closing_end = lower.find('>', closing_start);
                const std::size_t end = closing_end == std::string::npos ? head_close : std::min(head_close, closing_end + 1);
                out.append(html, tag_end + 1, end - (tag_end + 1));
                pos = end;
                continue;
            }
        }

        pos = tag_end + 1;
    }

    out.append(html, head_close, std::string::npos);
    return out;
}

// Identifies the inputs that fully determine a rendered DisplayList. If these
// match a previous render, the cached list is reused instead of re-running the
// serialize/parse/layout pipeline. The resource loader's own byte cache is
// intentionally not part of the key: a render is memoized against DOM state,
// viewport, and base URL, not against external resource contents.
struct DisplayListCacheKey {
    std::uint64_t mutation_version = 0;
    int viewport_width = 0;
    int viewport_height = 0;
    float device_scale_factor = 0.0f;
    bool load_external_resources = false;
    std::string base_url;

    bool operator==(const DisplayListCacheKey&) const = default;
};

} // namespace

struct Page::Impl {
    LoadOptions options;
    DomDocument document;
    std::shared_ptr<ResourceLoader> loader;
    std::shared_ptr<LayoutEngineFactory> layout_factory;
    std::unique_ptr<JsRuntime> js;
    std::string current_url;

    bool display_list_valid = false;
    DisplayListCacheKey display_list_key;
    DisplayList display_list_cache;

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
        display_list_valid = false;
        display_list_cache.clear();
    }

    void ensure_js()
    {
        if (!js) {
            js = std::make_unique<JsRuntime>(document, options, loader);
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
            const auto type = document.get_attribute(script, "type");
            if (!is_javascript_script_type(type)) {
                continue;
            }
            const bool module = is_module_script_type(type);

            const auto src = document.get_attribute(script, "src");
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
            if (script.module) {
                js->execute("document.__setCurrentScript(null);", "<pagecore-current-script>");
                js->execute_module(script.code, script.filename);
                continue;
            }

            js->execute("document.__setCurrentScript(" + std::to_string(script.node) + ");", "<pagecore-current-script>");
            try {
                js->execute(script.code, script.filename);
            } catch (...) {
                try {
                    js->execute("document.__setCurrentScript(null);", "<pagecore-current-script>");
                } catch (...) {
                }
                throw;
            }
            js->execute("document.__setCurrentScript(null);", "<pagecore-current-script>");
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
        std::string html = remove_head_direct_text_nodes(document.serialize_html());

        DomDocument render_document;
        render_document.parse(html);
        const NodeId root = render_document.document_node();

        const NodeId head = render_document.head();
        if (head != kInvalidNodeId) {
            const auto children = render_document.child_nodes(head);
            for (NodeId child : children) {
                if (render_document.node_type(child) == 3) {
                    render_document.remove_child(head, child);
                }
            }
        }

        if (options.enable_js) {
            for (NodeId noscript : render_document.query_selector_all(root, "noscript")) {
                if (!render_document.is_connected(noscript)) {
                    continue;
                }

                const NodeId parent = render_document.parent_node(noscript);
                if (parent != kInvalidNodeId) {
                    render_document.remove_child(parent, noscript);
                }
            }
        }

        return render_document.serialize_html();
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

        layout->set_viewport(render_options.viewport);
        layout->set_resource_loader(render_options.load_external_resources ? loader : nullptr);
        layout->load_html(serialized_html_for_render(), render_base_url(render_options));
        layout->layout();
        return layout;
    }

    const DisplayList& cached_display_list(const RenderOptions& render_options)
    {
        DisplayListCacheKey key{
            document.mutation_version(),
            render_options.viewport.width,
            render_options.viewport.height,
            render_options.viewport.device_scale_factor,
            render_options.load_external_resources,
            render_base_url(render_options),
        };

        if (display_list_valid && display_list_key == key) {
            return display_list_cache;
        }

        auto layout = build_layout(render_options);
        display_list_cache = layout->display_list();
        display_list_key = std::move(key);
        display_list_valid = true;
        return display_list_cache;
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

} // namespace pagecore
