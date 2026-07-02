#include "pagecore/page.hpp"

#include "cookie_jar.hpp"
#include "js_runtime.hpp"
#include "pagecore/render.hpp"
#include "pagecore/resource_loader.hpp"
#if defined(PAGECORE_ENABLE_RENDERING)
#include "web_fonts.hpp"
#endif

#include <memory>
#include <algorithm>
#include <cctype>
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

enum class LayoutResourceMode {
    Full,
    StylesheetsOnly,
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

struct CssUrlRef {
    std::string url;
    ResourceKind kind = ResourceKind::Image;
};

struct CssAttributeSelectorUsage {
    std::unordered_set<std::string> names;
    bool wildcard = false;
};

#if defined(PAGECORE_ENABLE_RENDERING)
struct PendingFontRequest {
    WebFontSource source;
    ResourceRequest request;
};
#endif

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

std::string ascii_lower(std::string_view value)
{
    std::string out(value);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return out;
}

std::string_view trim_ascii(std::string_view value)
{
    const auto first = value.find_first_not_of(" \t\n\r\f");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\n\r\f");
    return value.substr(first, last - first + 1);
}

bool ends_with_important(std::string_view value)
{
    value = trim_ascii(value);
    if (value.size() < 10) {
        return false;
    }
    return ascii_lower(value.substr(value.size() - 10)) == "!important";
}

std::optional<std::string> inline_style_property_value(std::string_view style, std::string_view property)
{
    const std::string wanted = ascii_lower(trim_ascii(property));
    if (wanted.empty()) {
        return std::nullopt;
    }

    std::size_t segment_start = 0;
    int paren_depth = 0;
    char quote = '\0';

    auto parse_segment = [&](std::string_view segment) -> std::optional<std::string> {
        segment = trim_ascii(segment);
        if (segment.empty()) {
            return std::nullopt;
        }

        std::size_t colon = std::string_view::npos;
        int local_paren_depth = 0;
        char local_quote = '\0';
        for (std::size_t i = 0; i < segment.size(); ++i) {
            const char ch = segment[i];
            if (local_quote != '\0') {
                if (ch == '\\' && i + 1 < segment.size()) {
                    ++i;
                    continue;
                }
                if (ch == local_quote) {
                    local_quote = '\0';
                }
                continue;
            }
            if (ch == '"' || ch == '\'') {
                local_quote = ch;
                continue;
            }
            if (ch == '(') {
                ++local_paren_depth;
                continue;
            }
            if (ch == ')' && local_paren_depth > 0) {
                --local_paren_depth;
                continue;
            }
            if (ch == ':' && local_paren_depth == 0) {
                colon = i;
                break;
            }
        }
        if (colon == std::string_view::npos) {
            return std::nullopt;
        }

        if (ascii_lower(trim_ascii(segment.substr(0, colon))) != wanted) {
            return std::nullopt;
        }

        std::string value(trim_ascii(segment.substr(colon + 1)));
        if (ends_with_important(value)) {
            value.erase(value.size() - 10);
            value = std::string(trim_ascii(value));
        }
        return value;
    };

    for (std::size_t i = 0; i <= style.size(); ++i) {
        const bool at_end = i == style.size();
        const char ch = at_end ? ';' : style[i];
        if (quote != '\0') {
            if (ch == '\\' && i + 1 < style.size()) {
                ++i;
                continue;
            }
            if (ch == quote) {
                quote = '\0';
            }
            continue;
        }
        if (ch == '"' || ch == '\'') {
            quote = ch;
            continue;
        }
        if (ch == '(') {
            ++paren_depth;
            continue;
        }
        if (ch == ')' && paren_depth > 0) {
            --paren_depth;
            continue;
        }
        if ((at_end || ch == ';') && paren_depth == 0) {
            if (auto value = parse_segment(style.substr(segment_start, i - segment_start))) {
                return value;
            }
            segment_start = i + 1;
        }
    }

    return std::nullopt;
}

std::optional<float> parse_css_px_length(std::string_view value)
{
    value = trim_ascii(value);
    if (value.empty()) {
        return std::nullopt;
    }
    if (ends_with_important(value)) {
        value = trim_ascii(value.substr(0, value.size() - 10));
    }

    std::string text(value);
    char* end = nullptr;
    const float number = std::strtof(text.c_str(), &end);
    if (end == text.c_str()) {
        return std::nullopt;
    }

    const std::string_view unit = trim_ascii(std::string_view(
        end,
        static_cast<std::size_t>((text.c_str() + text.size()) - end)));
    if (unit == "px" || (number == 0.0f && unit.empty())) {
        return number;
    }
    return std::nullopt;
}

std::optional<float> parse_css_percentage(std::string_view value)
{
    value = trim_ascii(value);
    if (value.empty()) {
        return std::nullopt;
    }
    if (ends_with_important(value)) {
        value = trim_ascii(value.substr(0, value.size() - 10));
    }

    std::string text(value);
    char* end = nullptr;
    const float number = std::strtof(text.c_str(), &end);
    if (end == text.c_str()) {
        return std::nullopt;
    }

    const std::string_view unit = trim_ascii(std::string_view(
        end,
        static_cast<std::size_t>((text.c_str() + text.size()) - end)));
    if (unit == "%") {
        return number;
    }
    return std::nullopt;
}

std::string default_display_for_tag(std::string_view tag)
{
    const std::string normalized = ascii_lower(tag);
    if (normalized == "li") return "list-item";
    if (normalized == "table") return "table";
    if (normalized == "thead") return "table-header-group";
    if (normalized == "tbody") return "table-row-group";
    if (normalized == "tfoot") return "table-footer-group";
    if (normalized == "tr") return "table-row";
    if (normalized == "td" || normalized == "th") return "table-cell";
    if (normalized == "input"
        || normalized == "button"
        || normalized == "select"
        || normalized == "textarea") {
        return "inline-block";
    }
    if (normalized == "script"
        || normalized == "style"
        || normalized == "template"
        || normalized == "head"
        || normalized == "meta"
        || normalized == "link"
        || normalized == "title") {
        return "none";
    }

    static const std::unordered_set<std::string_view> block_tags = {
        "address", "article", "aside", "blockquote", "body", "canvas", "dd", "div", "dl", "dt",
        "fieldset", "figcaption", "figure", "footer", "form", "h1", "h2", "h3", "h4", "h5", "h6",
        "header", "hr", "html", "legend", "main", "nav", "ol", "p", "pre", "section", "ul",
    };
    return block_tags.count(normalized) == 0 ? "inline" : "block";
}

std::optional<std::string> default_computed_style_property_value(
    std::string_view property,
    std::string_view tag)
{
    const std::string normalized = ascii_lower(property);
    if (normalized == "display") return default_display_for_tag(tag);
    if (normalized == "position") return "static";
    if (normalized == "float") return "none";
    if (normalized == "clear") return "none";
    if (normalized == "visibility") return "visible";
    if (normalized == "overflow") return "visible";
    if (normalized == "box-sizing") return "content-box";
    if (normalized == "z-index") return "auto";
    if (normalized == "opacity") return "1";
    if (normalized == "transform") return "none";
    if (normalized == "direction") return "ltr";
    if (normalized == "content") return "normal";

    if (normalized == "width"
        || normalized == "height"
        || normalized == "left"
        || normalized == "right"
        || normalized == "top"
        || normalized == "bottom") {
        return "auto";
    }
    if (normalized == "min-width" || normalized == "min-height") return "auto";
    if (normalized == "max-width" || normalized == "max-height") return "none";
    if (normalized == "margin-left"
        || normalized == "margin-right"
        || normalized == "margin-top"
        || normalized == "margin-bottom") {
        return "0px";
    }
    if (normalized == "padding-left"
        || normalized == "padding-right"
        || normalized == "padding-top"
        || normalized == "padding-bottom"
        || normalized == "text-indent") {
        return "0px";
    }
    if (normalized == "border-left-width"
        || normalized == "border-right-width"
        || normalized == "border-top-width"
        || normalized == "border-bottom-width") {
        return "0px";
    }
    if (normalized == "line-height") return "normal";
    if (normalized == "font-weight") return "400";
    if (normalized == "font-style") return "normal";
    if (normalized == "white-space") return "normal";
    if (normalized == "text-align") return "start";
    if (normalized == "vertical-align") return "baseline";
    if (normalized == "list-style-type") return "disc";
    if (normalized == "list-style-position") return "outside";
    if (normalized == "list-style-image") return "none";

    return std::nullopt;
}

class PerfScope final {
public:
    PerfScope(const PerfTraceCallback& callback, PerfPhase phase, std::string_view name, std::uint64_t count = 0)
        : callback_(callback)
        , event_{phase, std::string(name), 0, count}
        , start_(std::chrono::steady_clock::now())
    {
    }

    ~PerfScope()
    {
        const auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start_).count();
        try {
            event_.elapsed_us = elapsed_us;
            emit_perf_trace(callback_, event_);
        } catch (...) {
        }
    }

    void set_count(std::uint64_t count)
    {
        event_.count = count;
    }

    PerfEvent& event()
    {
        return event_;
    }

private:
    const PerfTraceCallback& callback_;
    PerfEvent event_;
    std::chrono::steady_clock::time_point start_;
};

long long elapsed_us_since(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count();
}

class BudgetedRenderResourceLoader final : public ResourceLoader {
public:
    using ResourceLoader::load;

    BudgetedRenderResourceLoader(
        std::shared_ptr<ResourceLoader> inner,
        RenderOptions options,
        PerfTraceCallback perf_trace)
        : inner_(std::move(inner))
        , max_loads_(options.max_external_resource_loads)
        , max_bytes_(options.max_external_resource_bytes)
        , max_time_(options.max_external_resource_time)
        , perf_trace_(std::move(perf_trace))
    {
        if (!inner_) {
            throw std::runtime_error("render resource budget loader requires an inner loader");
        }
    }

    ResourceResponse load(const ResourceRequest& request) override
    {
        if (auto reason = block_reason()) {
            return blocked_response(request, *reason);
        }

        const auto start = std::chrono::steady_clock::now();
        try {
            auto response = inner_->load(request);
            record_load(elapsed_us_since(start), response.body.size(), 1);
            return response;
        } catch (...) {
            record_load(elapsed_us_since(start), 0, 1);
            throw;
        }
    }

    std::vector<ResourceResponse> load_all(
        const std::vector<ResourceRequest>& requests,
        BatchErrorMode mode = BatchErrorMode::FailFast) override
    {
        std::vector<ResourceResponse> responses(requests.size());
        std::vector<ResourceRequest> allowed_requests;
        std::vector<std::size_t> allowed_indices;
        allowed_requests.reserve(requests.size());
        allowed_indices.reserve(requests.size());

        for (std::size_t i = 0; i < requests.size(); ++i) {
            if (auto reason = block_reason(allowed_requests.size())) {
                responses[i] = blocked_response(requests[i], *reason);
                continue;
            }
            allowed_indices.push_back(i);
            allowed_requests.push_back(requests[i]);
        }

        if (allowed_requests.empty()) {
            return responses;
        }

        const auto start = std::chrono::steady_clock::now();
        std::vector<ResourceResponse> fetched;
        try {
            fetched = inner_->load_all(allowed_requests, mode);
        } catch (...) {
            record_load(elapsed_us_since(start), 0, allowed_requests.size());
            throw;
        }

        std::uint64_t bytes = 0;
        for (std::size_t i = 0; i < fetched.size() && i < allowed_indices.size(); ++i) {
            bytes += fetched[i].body.size();
            responses[allowed_indices[i]] = std::move(fetched[i]);
        }
        record_load(elapsed_us_since(start), bytes, fetched.size());
        return responses;
    }

private:
    std::optional<std::string> block_reason(std::size_t pending_loads = 0) const
    {
        if (max_loads_ && load_count_ + pending_loads >= *max_loads_) {
            return "budget:max_render_resource_loads";
        }
        if (max_bytes_ && loaded_bytes_ >= *max_bytes_) {
            return "budget:max_render_resource_bytes";
        }
        if (max_time_) {
            const auto max_us = std::chrono::duration_cast<std::chrono::microseconds>(*max_time_).count();
            if (elapsed_us_ >= max_us) {
                return "budget:max_render_resource_time_ms";
            }
        }
        return std::nullopt;
    }

    ResourceResponse blocked_response(const ResourceRequest& request, const std::string& reason) const
    {
        PerfEvent event{PerfPhase::ResourceLoad, "render_resource_blocked", 0, 0};
        event.property = resource_kind_name(request.kind);
        event.url = request.url;
        event.reason = reason;
        emit_perf_trace(perf_trace_, std::move(event));
        return ResourceResponse{request.url, {}, 0, {}, request.kind, false};
    }

    void record_load(long long elapsed_us, std::uint64_t bytes, std::size_t loads)
    {
        load_count_ += loads;
        loaded_bytes_ += bytes;
        elapsed_us_ += std::max<long long>(0, elapsed_us);
    }

    std::shared_ptr<ResourceLoader> inner_;
    std::optional<std::size_t> max_loads_;
    std::optional<std::size_t> max_bytes_;
    std::optional<std::chrono::milliseconds> max_time_;
    PerfTraceCallback perf_trace_;
    std::size_t load_count_ = 0;
    std::uint64_t loaded_bytes_ = 0;
    long long elapsed_us_ = 0;
};

class StylesheetOnlyResourceLoader final : public ResourceLoader {
public:
    using ResourceLoader::load;

    StylesheetOnlyResourceLoader(
        std::shared_ptr<ResourceLoader> inner,
        PerfTraceCallback perf_trace)
        : inner_(std::move(inner))
        , perf_trace_(std::move(perf_trace))
    {
        if (!inner_) {
            throw std::runtime_error("stylesheet-only resource loader requires an inner loader");
        }
    }

    ResourceResponse load(const ResourceRequest& request) override
    {
        if (request.kind != ResourceKind::Stylesheet) {
            return skipped_response(request);
        }
        return inner_->load(request);
    }

    std::vector<ResourceResponse> load_all(
        const std::vector<ResourceRequest>& requests,
        BatchErrorMode mode = BatchErrorMode::FailFast) override
    {
        std::vector<ResourceResponse> responses(requests.size());
        std::vector<ResourceRequest> stylesheet_requests;
        std::vector<std::size_t> stylesheet_indices;
        stylesheet_requests.reserve(requests.size());
        stylesheet_indices.reserve(requests.size());

        for (std::size_t i = 0; i < requests.size(); ++i) {
            if (requests[i].kind != ResourceKind::Stylesheet) {
                responses[i] = skipped_response(requests[i]);
                continue;
            }
            stylesheet_indices.push_back(i);
            stylesheet_requests.push_back(requests[i]);
        }

        if (stylesheet_requests.empty()) {
            return responses;
        }

        const auto fetched = inner_->load_all(stylesheet_requests, mode);
        for (std::size_t i = 0; i < fetched.size() && i < stylesheet_indices.size(); ++i) {
            responses[stylesheet_indices[i]] = fetched[i];
        }
        return responses;
    }

private:
    ResourceResponse skipped_response(const ResourceRequest& request) const
    {
        PerfEvent event{PerfPhase::ResourceLoad, "render_resource_skipped", 0, 0};
        event.property = resource_kind_name(request.kind);
        event.url = request.url;
        event.reason = "geometry:stylesheets_only";
        emit_perf_trace(perf_trace_, std::move(event));
        return ResourceResponse{request.url, {}, 0, {}, request.kind, false};
    }

    std::shared_ptr<ResourceLoader> inner_;
    PerfTraceCallback perf_trace_;
};

class CookieAwareResourceLoader final : public ResourceLoader {
public:
    using ResourceLoader::load;

    CookieAwareResourceLoader(
        std::shared_ptr<ResourceLoader> inner,
        CookieJar* cookie_jar,
        std::string document_url,
        CookieCredentials credentials)
        : inner_(std::move(inner))
        , cookie_jar_(cookie_jar)
        , document_url_(std::move(document_url))
        , credentials_(credentials)
    {
        if (!inner_) {
            throw std::runtime_error("cookie-aware resource loader requires an inner loader");
        }
    }

    ResourceResponse load(const ResourceRequest& request) override
    {
        ResourceRequest cookie_request = with_cookies(request);
        const std::string request_url = cookie_request.url;
        ResourceResponse response = inner_->load(cookie_request);
        store_cookies(request_url, response);
        return response;
    }

    std::vector<ResourceResponse> load_all(
        const std::vector<ResourceRequest>& requests,
        BatchErrorMode mode = BatchErrorMode::FailFast) override
    {
        std::vector<ResourceRequest> cookie_requests;
        cookie_requests.reserve(requests.size());
        std::vector<std::string> request_urls;
        request_urls.reserve(requests.size());
        for (const auto& request : requests) {
            cookie_requests.push_back(with_cookies(request));
            request_urls.push_back(cookie_requests.back().url);
        }

        std::vector<ResourceResponse> responses = inner_->load_all(cookie_requests, mode);
        for (std::size_t i = 0; i < responses.size() && i < request_urls.size(); ++i) {
            store_cookies(request_urls[i], responses[i]);
        }
        return responses;
    }

private:
    ResourceRequest with_cookies(const ResourceRequest& request) const
    {
        if (cookie_jar_ == nullptr) {
            return request;
        }
        return cookie_jar_->with_cookie_header(request, credentials_, document_url_);
    }

    void store_cookies(std::string_view request_url, const ResourceResponse& response) const
    {
        if (cookie_jar_ != nullptr) {
            cookie_jar_->store_from_response(request_url, response, credentials_, document_url_);
        }
    }

    std::shared_ptr<ResourceLoader> inner_;
    CookieJar* cookie_jar_ = nullptr;
    std::string document_url_;
    CookieCredentials credentials_ = CookieCredentials::SameOrigin;
};

std::string ascii_lower_copy(std::string_view value)
{
    std::string out;
    out.reserve(value.size());
    for (unsigned char ch : value) {
        out.push_back(static_cast<char>(std::tolower(ch)));
    }
    return out;
}

bool is_css_attribute_name_char(unsigned char ch)
{
    return std::isalnum(ch) || ch == '-' || ch == '_' || ch == ':';
}

void record_css_attribute_selector(std::string_view content, CssAttributeSelectorUsage& usage)
{
    std::size_t i = 0;
    while (i < content.size() && std::isspace(static_cast<unsigned char>(content[i]))) {
        ++i;
    }
    if (i >= content.size()) {
        usage.wildcard = true;
        return;
    }
    if (content.find('\\') != std::string_view::npos) {
        usage.wildcard = true;
        return;
    }

    // Namespaced attribute selectors such as [svg|href] depend on the local
    // attribute name for our invalidation purposes.
    const auto pipe = content.find('|', i);
    if (pipe != std::string_view::npos) {
        i = pipe + 1;
    }
    while (i < content.size() && std::isspace(static_cast<unsigned char>(content[i]))) {
        ++i;
    }

    const std::size_t start = i;
    while (i < content.size() && is_css_attribute_name_char(static_cast<unsigned char>(content[i]))) {
        ++i;
    }
    if (i == start) {
        usage.wildcard = true;
        return;
    }
    usage.names.insert(ascii_lower_copy(content.substr(start, i - start)));
}

void collect_css_attribute_selectors(std::string_view css, CssAttributeSelectorUsage& usage)
{
    const std::size_t n = css.size();
    for (std::size_t i = 0; i < n; ++i) {
        const char ch = css[i];
        if (ch == '/' && i + 1 < n && css[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(css[i] == '*' && css[i + 1] == '/')) {
                ++i;
            }
            i = std::min(n, i + 1);
            continue;
        }
        if (ch == '"' || ch == '\'') {
            const char quote = ch;
            ++i;
            while (i < n) {
                if (css[i] == '\\') {
                    i += 2;
                    continue;
                }
                if (css[i] == quote) {
                    break;
                }
                ++i;
            }
            continue;
        }
        if (ch != '[') {
            continue;
        }

        const std::size_t start = i + 1;
        bool closed = false;
        ++i;
        while (i < n) {
            if (css[i] == '"' || css[i] == '\'') {
                const char quote = css[i++];
                while (i < n) {
                    if (css[i] == '\\') {
                        i += 2;
                        continue;
                    }
                    if (css[i] == quote) {
                        break;
                    }
                    ++i;
                }
            } else if (css[i] == ']') {
                closed = true;
                break;
            }
            ++i;
        }
        if (!closed) {
            usage.wildcard = true;
            return;
        }
        record_css_attribute_selector(css.substr(start, i - start), usage);
    }
}

std::vector<std::string> css_attribute_selector_names(const CssAttributeSelectorUsage& usage)
{
    std::vector<std::string> names;
    names.reserve(usage.names.size());
    for (const auto& name : usage.names) {
        names.push_back(name);
    }
    return names;
}

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

    // Sub-resource cache shared by every styled-document rebuild of the current
    // document, so a page that rebuilds many times during script execution
    // fetches each image/stylesheet once instead of on every rebuild. Reset when
    // the document or loader identity changes (invalidate_display_list_cache).
    std::shared_ptr<CachingResourceLoader> render_resource_cache;
    std::shared_ptr<CachingResourceLoader> js_resource_cache;

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

    void collect_inline_style_attribute_selectors(CssAttributeSelectorUsage& usage)
    {
        const NodeId root = document.document_node();
        for (NodeId style : document.query_selector_all(root, "style")) {
            collect_css_attribute_selectors(document.text_content(style), usage);
        }
    }

    void apply_layout_sensitive_attributes(const CssAttributeSelectorUsage& usage)
    {
        document.set_layout_sensitive_attributes(css_attribute_selector_names(usage), usage.wildcard);
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
        const std::shared_ptr<ResourceLoader>& render_loader,
        LayoutResourceMode resource_mode)
    {
        if (resource_mode != LayoutResourceMode::Full) {
            return nullptr;
        }
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
        const std::shared_ptr<ResourceLoader>&,
        LayoutResourceMode)
    {
        return nullptr;
    }
#endif

    std::uint64_t render_prefetch_loaded_bytes(const std::vector<ResourceResponse>& responses) const
    {
        std::uint64_t bytes = 0;
        for (const auto& response : responses) {
            bytes += response.body.size();
        }
        return bytes;
    }

    void emit_render_prefetch_responses(
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
    std::shared_ptr<ResourceLoader> prepare_render_loader(
        const RenderOptions& render_options,
        LayoutResourceMode resource_mode)
    {
        CssAttributeSelectorUsage attribute_usage;
        collect_inline_style_attribute_selectors(attribute_usage);
        apply_layout_sensitive_attributes(attribute_usage);

        if (!render_options.load_external_resources || !loader) {
            return nullptr;
        }

        if (!render_resource_cache) {
            render_resource_cache = std::make_shared<CachingResourceLoader>(loader, 4096);
        }
        const std::shared_ptr<CachingResourceLoader>& cache = render_resource_cache;
        std::shared_ptr<ResourceLoader> render_loader = std::make_shared<CookieAwareResourceLoader>(
            cache,
            &cookie_jar,
            render_base_url(render_options),
            CookieCredentials::SameOrigin);
        if (resource_mode == LayoutResourceMode::StylesheetsOnly) {
            render_loader = std::make_shared<StylesheetOnlyResourceLoader>(
                render_loader,
                perf_trace_for(render_options));
        }
        if (render_options.max_external_resource_loads
            || render_options.max_external_resource_bytes
            || render_options.max_external_resource_time) {
            render_loader = std::make_shared<BudgetedRenderResourceLoader>(
                render_loader,
                render_options,
                perf_trace_for(render_options));
        }

        const std::string effective_base = effective_base_url(render_options);

        std::vector<ResourceRequest> wave1;
        std::unordered_set<std::string> seen;
        {
            PerfScope trace(perf_trace_for(render_options), PerfPhase::SubresourceScan, "collect_dom_subresources");
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
            // Lenient: a failed sub-resource must not abort the render; litehtml
            // will fall back to its placeholder for it.
            PerfScope trace(perf_trace_for(render_options), PerfPhase::ResourceLoad, "render_prefetch_wave1", wave1.size());
            trace.event().property = resource_mode == LayoutResourceMode::Full ? "render" : "geometry";
            trace.event().reason = "wave1";
            fetched = render_loader->load_all(wave1, BatchErrorMode::Lenient);
            trace.set_count(render_prefetch_loaded_bytes(fetched));
            emit_render_prefetch_responses(perf_trace_for(render_options), "wave1", wave1, fetched);
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
                PerfScope trace(perf_trace_for(render_options), PerfPhase::ResourceLoad, "render_prefetch_wave2", wave2.size());
                trace.event().property = "render";
                trace.event().reason = "wave2";
                const auto wave2_responses = render_loader->load_all(wave2, BatchErrorMode::Lenient);
                trace.set_count(render_prefetch_loaded_bytes(wave2_responses));
                emit_render_prefetch_responses(perf_trace_for(render_options), "wave2", wave2, wave2_responses);
            } catch (...) {
            }
        }

        return render_loader;
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

    // A connected element whose width litehtml leaves indefinite in a way that
    // collapses percentage-width children: computed position:absolute, box-sizing
    // border-box, and a percentage width. (border-box is required so that pinning
    // `width:Npx` sets the border box to exactly N.)
    bool is_absolute_percent_width_border_box(LayoutEngine& engine, NodeId node)
    {
        const std::string key = std::to_string(node);
        const auto position = engine.computed_style_property(key, "position");
        if (!position || ascii_lower(trim_ascii(*position)) != "absolute") {
            return false;
        }
        const auto box_sizing = engine.computed_style_property(key, "box-sizing");
        if (!box_sizing || ascii_lower(trim_ascii(*box_sizing)) != "border-box") {
            return false;
        }
        const auto width = engine.computed_style_property(key, "width");
        return width && parse_css_percentage(*width).has_value();
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
    std::vector<DomDocument::LayoutStyleOverride> compute_render_local_absolute_percent_overrides(LayoutEngine& engine)
    {
        std::vector<DomDocument::LayoutStyleOverride> overrides;
        const NodeId root = document.document_node();
        if (root == kInvalidNodeId) {
            return overrides;
        }
        for (NodeId node : document.query_selector_all(root, "*")) {
            if (!document.is_connected(node) || !is_absolute_percent_width_border_box(engine, node)) {
                continue;
            }
            const auto native = engine.element_geometry(std::to_string(node));
            if (!native || native->border_box.width < 1.0f) {
                continue;  // not laid out (e.g. display:none) — nothing to pin
            }
            const int width_px = std::max(1, static_cast<int>(native->border_box.width + 0.5f));
            overrides.push_back(DomDocument::LayoutStyleOverride{
                node,
                "width:" + std::to_string(width_px) + "px;",
            });
        }
        return overrides;
    }

    std::uint64_t computed_style_digest(NodeId node) const
    {
        return document.layout_input_digest(
            node,
            last_render_options.viewport.width,
            last_render_options.viewport.height,
            last_render_options.viewport.device_scale_factor);
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

        auto render_loader = prepare_render_loader(render_options, resource_mode);
        auto font_environment = prepare_font_environment(render_options, render_loader, resource_mode);
        layout->set_viewport(render_options.viewport);
        layout->set_resource_loader(std::move(render_loader));
        layout->set_font_environment(std::move(font_environment));
        std::string html = serialized_html_for_layout(
            render_options,
            /*absolute_percent_corrected=*/false);
        {
            PerfScope trace(perf_trace_for(render_options), PerfPhase::LitehtmlLoadHtml, "load_html", html.size());
            layout->load_html(html, render_base_url(render_options));
        }
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
    LayoutEngine& ensure_styled_document(
        const RenderOptions& render_options,
        bool absolute_percent_corrected,
        LayoutResourceMode resource_mode)
    {
        StyledDocumentCacheKey key = styled_document_cache_key(render_options, absolute_percent_corrected, resource_mode);

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

        auto render_loader = prepare_render_loader(render_options, resource_mode);
        auto font_environment = prepare_font_environment(render_options, render_loader, resource_mode);
        engine->set_viewport(render_options.viewport);
        engine->set_resource_loader(std::move(render_loader));
        engine->set_font_environment(std::move(font_environment));
        {
            const auto t0 = std::chrono::steady_clock::now();
            std::string html = serialized_html_for_layout(render_options, absolute_percent_corrected);
            {
                PerfScope trace(perf_trace_for(render_options), PerfPhase::LitehtmlLoadHtml, "load_html", html.size());
                engine->load_html(html, render_base_url(render_options));
            }
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
