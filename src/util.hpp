#pragma once

#include <string>
#include <string_view>

namespace pagecore {

// Canonical ASCII-only lowercasing shared across the codebase. Lowercases each
// byte via std::tolower on its unsigned char value, leaving non-ASCII bytes
// untouched under the C locale.
std::string ascii_lower(std::string_view value);

// Case-insensitive HTTP header-name comparison, shared across the codebase.
bool header_name_equals(std::string_view left, std::string_view right);

} // namespace pagecore
