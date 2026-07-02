#pragma once

#include <string>
#include <string_view>

namespace pagecore {

// Canonical ASCII-only lowercasing shared across the codebase. Lowercases each
// byte via std::tolower on its unsigned char value, leaving non-ASCII bytes
// untouched under the C locale.
std::string ascii_lower(std::string_view value);

} // namespace pagecore
