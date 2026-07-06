#include "css_scan.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdlib>

#include "util.hpp"

namespace pagecore {

std::string_view trim_ascii(std::string_view value)
{
    const auto first = value.find_first_not_of(" \t\n\r\f");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\n\r\f");
    return value.substr(first, last - first + 1);
}

namespace {

bool ends_with_important(std::string_view value)
{
    value = trim_ascii(value);
    if (value.size() < 10) {
        return false;
    }
    return ascii_lower(value.substr(value.size() - 10)) == "!important";
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
    usage.names.insert(ascii_lower(content.substr(start, i - start)));
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

} // namespace

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

bool css_declarations_require_tree_rebuild(std::string_view style)
{
    auto property_forces_rebuild = [](std::string_view name) {
        const std::string lower = ascii_lower(trim_ascii(name));
        return lower == "display"
            || lower == "position"
            || lower == "float"
            || lower == "content"
            || lower == "direction"
            || lower.rfind("list-style", 0) == 0;
    };

    std::size_t segment_start = 0;
    int paren_depth = 0;
    char quote = '\0';

    auto scan_segment = [&](std::string_view segment) {
        const std::size_t colon = segment.find(':');
        if (colon == std::string_view::npos) {
            return false;
        }
        return property_forces_rebuild(segment.substr(0, colon));
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
        if (ch == ';' && paren_depth == 0) {
            if (scan_segment(style.substr(segment_start, i - segment_start))) {
                return true;
            }
            segment_start = i + 1;
        }
    }
    return false;
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

std::string parse_css_url_target(std::string_view css, std::size_t& pos)
{
    const std::size_t n = css.size();
    std::size_t j = pos;
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
    pos = j;
    return target;
}

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
                    std::string target = parse_css_url_target(css, j);
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

} // namespace pagecore
