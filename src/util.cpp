#include "util.hpp"

#include <algorithm>
#include <cctype>

namespace pagecore {

std::string ascii_lower(std::string_view value)
{
    std::string out(value);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return out;
}

bool header_name_equals(std::string_view left, std::string_view right)
{
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(left[i])) != std::tolower(static_cast<unsigned char>(right[i]))) {
            return false;
        }
    }
    return true;
}

void append_utf8_codepoint(std::string& out, std::uint32_t code_point)
{
    if (code_point <= 0x7f) {
        out += static_cast<char>(code_point);
    } else if (code_point <= 0x7ff) {
        out += static_cast<char>(0xc0 | (code_point >> 6));
        out += static_cast<char>(0x80 | (code_point & 0x3f));
    } else if (code_point <= 0xffff) {
        out += static_cast<char>(0xe0 | (code_point >> 12));
        out += static_cast<char>(0x80 | ((code_point >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (code_point & 0x3f));
    } else {
        out += static_cast<char>(0xf0 | (code_point >> 18));
        out += static_cast<char>(0x80 | ((code_point >> 12) & 0x3f));
        out += static_cast<char>(0x80 | ((code_point >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (code_point & 0x3f));
    }
}

} // namespace pagecore
