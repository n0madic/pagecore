#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace pagecore {

// Canonical ASCII-only lowercasing shared across the codebase. Lowercases each
// byte via std::tolower on its unsigned char value, leaving non-ASCII bytes
// untouched under the C locale.
std::string ascii_lower(std::string_view value);

// Case-insensitive HTTP header-name comparison, shared across the codebase.
bool header_name_equals(std::string_view left, std::string_view right);

// Appends the UTF-8 encoding of a single Unicode code point to `out`.
void append_utf8_codepoint(std::string& out, std::uint32_t code_point);

} // namespace pagecore
