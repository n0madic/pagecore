#pragma once

#include <string>
#include <string_view>

namespace pagecore {

enum class Base64DecodeMode {
    Strict,
    Forgiving,
};

std::string base64_encode(std::string_view bytes);
std::string base64_decode(std::string_view text, Base64DecodeMode mode = Base64DecodeMode::Strict);

} // namespace pagecore
