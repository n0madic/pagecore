#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "util.hpp"
#include "pagecore/resource_loader.hpp"

namespace pagecore {

// A url(...) target discovered in a CSS source, tagged with the resource kind it
// should be prefetched as.
struct CssUrlRef {
    std::string url;
    ResourceKind kind = ResourceKind::Image;
};

// The set of attribute names referenced by attribute selectors in a stylesheet.
// `wildcard` is set when a selector cannot be reduced to a concrete name (an
// unterminated bracket, an escaped/computed name, or an empty target), which
// forces callers to treat any attribute mutation as layout-sensitive.
struct CssAttributeSelectorUsage {
    std::unordered_set<std::string> names;
    bool wildcard = false;
};

// ASCII-only whitespace trimming used across the CSS scanner. (ASCII lowercasing
// is provided by util.hpp's ascii_lower, included above.)
std::string_view trim_ascii(std::string_view value);

// Returns the value of `property` from an inline `style="..."` declaration list,
// honouring quoting/parenthesis nesting and stripping a trailing `!important`.
std::optional<std::string> inline_style_property_value(
    std::string_view style,
    std::string_view property);

// Parses a bare CSS percentage value (e.g. "50%"), stripping `!important`.
std::optional<float> parse_css_percentage(std::string_view value);

// True when an inline `style="..."` declaration list contains any property that
// changes the layout tree structure rather than only an element's own box
// metrics (display, position, float, content, direction, list-style*). Such a
// change cannot be applied by an in-place restyle that reuses render items and
// must force a full rebuild instead.
bool css_declarations_require_tree_rebuild(std::string_view style);

// Returns the UA default computed value for a CSS property on an element with
// the given tag name, or nullopt when there is no simple tag-independent default.
std::optional<std::string> default_computed_style_property_value(
    std::string_view property,
    std::string_view tag);

// Collects attribute-selector usage from a CSS source into `usage`.
void collect_css_attribute_selectors(std::string_view css, CssAttributeSelectorUsage& usage);

// Flattens the concrete attribute names collected in `usage` into a vector.
std::vector<std::string> css_attribute_selector_names(const CssAttributeSelectorUsage& usage);

// Parses a single CSS url(...) target starting at `pos`, which must index the
// character just after the opening '(' (leading whitespace is allowed). Handles
// quoted and unquoted targets with backslash-unescaping, advances `pos` past the
// closing ')', and returns the unescaped target (which may be empty). This is the
// shared primitive behind both extract_css_urls and the web-font src parser.
std::string parse_css_url_target(std::string_view css, std::size_t& pos);

// Extracts url(...) targets from a CSS source so they can be prefetched. Handles
// comments, quoted/unquoted targets, and multi-value declarations. `@import`
// targets are returned as stylesheets; `src:` declarations (web fonts) are
// skipped here because they are fetched by the web-font pipeline, not the image
// prefetch path. Callers still resolve and filter the raw targets.
std::vector<CssUrlRef> extract_css_urls(std::string_view css);

} // namespace pagecore
