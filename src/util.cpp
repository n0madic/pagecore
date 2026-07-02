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

} // namespace pagecore
