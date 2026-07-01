#include "base64_codec.hpp"

#include <base64.hpp>

#include <stdexcept>

namespace pagecore {
namespace {

bool is_ascii_whitespace(unsigned char ch)
{
    return ch == '\t' || ch == '\n' || ch == '\f' || ch == '\r' || ch == ' ';
}

bool is_base64_alphabet(unsigned char ch)
{
    return (ch >= 'A' && ch <= 'Z')
        || (ch >= 'a' && ch <= 'z')
        || (ch >= '0' && ch <= '9')
        || ch == '+'
        || ch == '/';
}

std::string normalize_forgiving_base64(std::string_view text)
{
    std::string normalized;
    normalized.reserve(text.size());
    for (unsigned char ch : text) {
        if (!is_ascii_whitespace(ch)) {
            normalized.push_back(static_cast<char>(ch));
        }
    }

    if (normalized.empty()) {
        return normalized;
    }

    if (normalized.size() % 4 == 0) {
        std::size_t padding = 0;
        while (padding < normalized.size() && normalized[normalized.size() - 1 - padding] == '=') {
            ++padding;
        }
        if (padding > 2) {
            throw std::invalid_argument("invalid base64 padding");
        }
        if (padding > 0) {
            normalized.resize(normalized.size() - padding);
        }
    } else if (normalized.find('=') != std::string::npos) {
        throw std::invalid_argument("invalid base64 padding");
    }

    if (normalized.size() % 4 == 1) {
        throw std::invalid_argument("invalid base64 length");
    }

    for (unsigned char ch : normalized) {
        if (!is_base64_alphabet(ch)) {
            throw std::invalid_argument("invalid base64 character");
        }
    }

    while (normalized.size() % 4 != 0) {
        normalized.push_back('=');
    }
    return normalized;
}

} // namespace

std::string base64_encode(std::string_view bytes)
{
    return base64::to_base64(bytes);
}

std::string base64_decode(std::string_view text, Base64DecodeMode mode)
{
    if (mode == Base64DecodeMode::Forgiving) {
        return base64::from_base64(normalize_forgiving_base64(text));
    }
    return base64::from_base64(text);
}

} // namespace pagecore
