#pragma once

#include <string>
#include <string_view>

namespace pagecore {

struct DecodedHtml {
    std::string utf8_html;
    std::string character_set;
};

// Resolves the character encoding of raw HTML bytes and returns them transcoded
// to UTF-8, following the practically-relevant subset of the WHATWG "determining
// the character encoding" algorithm:
//   1. BOM sniff (UTF-8/UTF-16LE/UTF-16BE) -- wins unconditionally if present.
//   2. `content_type_header`'s charset parameter (transport layer), if present
//      and it names a known encoding.
//   3. `<meta charset>` / `<meta http-equiv=content-type>` prescan over the
//      first 1024 bytes.
//   4. Default: UTF-8.
// `content_type_header` may be empty (no transport-declared charset). Full
// chardet-style statistical sniffing is out of scope; step 4 matches the
// spec-permitted UTF-8 default.
DecodedHtml decode_html_bytes(std::string_view raw_bytes, std::string_view content_type_header);

} // namespace pagecore
