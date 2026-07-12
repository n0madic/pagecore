#include "html_encoding.hpp"

#include "util.hpp"

#include <lexbor/encoding/encoding.h>
#include <lexbor/html/encoding.h>

#include <algorithm>
#include <array>

namespace pagecore {
namespace {

// https://html.spec.whatwg.org/#prescan-a-byte-stream-to-determine-its-encoding
// suggests scanning only a bounded prefix; browsers commonly use 1024 bytes.
constexpr std::size_t kPrescanBudget = 1024;

const lxb_encoding_data_t* resolve_from_content_type(std::string_view content_type_header)
{
    if (content_type_header.empty()) {
        return nullptr;
    }

    const auto* begin = reinterpret_cast<const lxb_char_t*>(content_type_header.data());
    const lxb_char_t* end = begin + content_type_header.size();

    const lxb_char_t* name_end = nullptr;
    const lxb_char_t* name = lxb_html_encoding_content(begin, end, &name_end);
    if (name == nullptr || name_end == nullptr || name_end <= name) {
        return nullptr;
    }

    return lxb_encoding_data_by_pre_name(name, static_cast<std::size_t>(name_end - name));
}

const lxb_encoding_data_t* resolve_from_prescan(std::string_view raw_bytes)
{
    const std::size_t scan_length = std::min(raw_bytes.size(), kPrescanBudget);
    const auto* begin = reinterpret_cast<const lxb_char_t*>(raw_bytes.data());
    const lxb_char_t* end = begin + scan_length;

    lxb_html_encoding_t em;
    if (lxb_html_encoding_init(&em) != LXB_STATUS_OK) {
        return nullptr;
    }

    std::size_t name_length = 0;
    const lxb_char_t* name = lxb_html_encoding_prescan(&em, begin, end, &name_length);

    const lxb_encoding_data_t* resolved = nullptr;
    if (name != nullptr && name_length != 0) {
        resolved = lxb_encoding_data_prescan_validate(name, name_length);
    }

    lxb_html_encoding_destroy(&em, false);
    return resolved;
}

// Non-streaming full-buffer transcode into UTF-8, replacing invalid sequences
// with U+FFFD (matches TextDecoder's non-fatal default and the WHATWG HTML
// parser's own decode behavior -- a scraper should never abort on bad bytes).
std::string transcode_to_utf8(std::string_view raw_bytes, const lxb_encoding_data_t* encoding_data)
{
    lxb_encoding_decode_t decode;
    lxb_encoding_decode_init(&decode, encoding_data, nullptr, 0);

    lxb_codepoint_t replacement = LXB_ENCODING_REPLACEMENT_CODEPOINT;

    std::string out;
    out.reserve(raw_bytes.size());

    const auto* data = reinterpret_cast<const lxb_char_t*>(raw_bytes.data());
    const lxb_char_t* end = data + raw_bytes.size();

    std::array<lxb_codepoint_t, 256> codepoint_buffer{};

    auto drain = [&]() {
        const lxb_codepoint_t* codepoints = lxb_encoding_decode_buf(&decode);
        const std::size_t used = lxb_encoding_decode_buf_used(&decode);
        for (std::size_t i = 0; i < used; ++i) {
            append_utf8_codepoint(out, codepoints[i]);
        }
        lxb_encoding_decode_buf_used_set(&decode, 0);
    };

    while (data < end) {
        decode.buffer_out = codepoint_buffer.data();
        decode.buffer_length = codepoint_buffer.size();
        decode.buffer_used = 0;
        decode.replace_to = &replacement;
        decode.replace_len = 1;

        const lxb_status_t status = encoding_data->decode(&decode, &data, end);
        drain();
        if (status == LXB_STATUS_OK) {
            break;
        }
        // LXB_STATUS_SMALL_BUFFER/LXB_STATUS_CONTINUE both mean "call again with
        // more output space / more input"; replace_to guarantees no other status
        // is reachable in non-fatal mode.
    }

    decode.buffer_out = codepoint_buffer.data();
    decode.buffer_length = codepoint_buffer.size();
    decode.buffer_used = 0;
    if (lxb_encoding_decode_finish(&decode) == LXB_STATUS_OK) {
        drain();
    }

    return out;
}

} // namespace

DecodedHtml decode_html_bytes(std::string_view raw_bytes, std::string_view content_type_header)
{
    const auto* data = reinterpret_cast<const lxb_char_t*>(raw_bytes.data());
    const lxb_encoding_t bom_encoding = lxb_encoding_bom_sniff(data, raw_bytes.size());

    std::string_view body = raw_bytes;
    const lxb_encoding_data_t* resolved = nullptr;

    if (bom_encoding != LXB_ENCODING_DEFAULT) {
        const lxb_char_t* begin = data;
        std::size_t length = raw_bytes.size();
        switch (bom_encoding) {
        case LXB_ENCODING_UTF_8:
            lxb_encoding_utf_8_skip_bom(&begin, &length);
            break;
        case LXB_ENCODING_UTF_16BE:
            lxb_encoding_utf_16be_skip_bom(&begin, &length);
            break;
        case LXB_ENCODING_UTF_16LE:
            lxb_encoding_utf_16le_skip_bom(&begin, &length);
            break;
        default:
            break;
        }
        body = std::string_view(reinterpret_cast<const char*>(begin), length);
        resolved = lxb_encoding_data(bom_encoding);
    } else {
        resolved = resolve_from_content_type(content_type_header);
        if (resolved == nullptr) {
            resolved = resolve_from_prescan(raw_bytes);
        }
    }

    if (resolved == nullptr || resolved->encoding == LXB_ENCODING_UTF_8) {
        return DecodedHtml{std::string(body), "UTF-8"};
    }

    DecodedHtml result;
    result.character_set = reinterpret_cast<const char*>(resolved->name);
    result.utf8_html = transcode_to_utf8(body, resolved);
    return result;
}

} // namespace pagecore
