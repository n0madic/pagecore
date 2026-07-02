#include "script_type.hpp"

#include <algorithm>
#include <cctype>
#include <string_view>

namespace pagecore {
namespace {

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

} // namespace

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

} // namespace pagecore
