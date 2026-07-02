#include "web_fonts.hpp"

#include "css_scan.hpp"

#include <fontconfig/fontconfig.h>
#include <fontconfig/fcfreetype.h>
#include <pango/pangocairo.h>
#include <pango/pangofc-fontmap.h>
#include <woff2/decode.h>
#include <woff2/output.h>
#include <zlib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pagecore {
namespace {

constexpr std::size_t kMaxDecodedFontBytes = 32 * 1024 * 1024;

bool starts_with(std::string_view text, std::string_view prefix)
{
    return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

std::string to_lower(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (char ch : text) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return out;
}

void ensure_fontconfig_initialized()
{
    static std::once_flag once;
    std::call_once(once, [] {
        // Keep the process-default config alive. Fontconfig's object registry is
        // ref-counted through FcConfig instances; short-lived app-font configs
        // can otherwise drop the registry to zero before Pango finishes cleanup.
        static FcConfig* object_registry_guard = FcConfigCreate();
        if (object_registry_guard == nullptr) {
            throw std::runtime_error("failed to retain Fontconfig object registry");
        }
        if (!FcInit()) {
            throw std::runtime_error("failed to initialize Fontconfig");
        }
        static FcConfig* default_config = FcConfigReference(nullptr);
        if (default_config == nullptr) {
            throw std::runtime_error("failed to retain default Fontconfig config");
        }
    });
}

FcConfig* create_font_config()
{
    ensure_fontconfig_initialized();
    return FcConfigCreate();
}

PangoFontMap* create_font_map()
{
    ensure_fontconfig_initialized();
    return pango_cairo_font_map_new_for_font_type(CAIRO_FONT_TYPE_FT);
}

std::string trim(std::string_view text)
{
    const auto first = text.find_first_not_of(" \t\r\n\f");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n\f");
    return std::string(text.substr(first, last - first + 1));
}

std::string unquote_css_string(std::string_view text)
{
    std::string value = trim(text);
    if (value.size() >= 2
        && ((value.front() == '"' && value.back() == '"')
            || (value.front() == '\'' && value.back() == '\''))) {
        value = value.substr(1, value.size() - 2);
    }

    std::string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\\' && i + 1 < value.size()) {
            out.push_back(value[++i]);
        } else {
            out.push_back(value[i]);
        }
    }
    return out;
}

bool iequals_prefix(std::string_view text, std::string_view prefix)
{
    if (text.size() < prefix.size()) {
        return false;
    }
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(text[i]))
            != std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

std::size_t skip_css_string(std::string_view css, std::size_t i)
{
    const char quote = css[i++];
    while (i < css.size()) {
        if (css[i] == '\\' && i + 1 < css.size()) {
            i += 2;
            continue;
        }
        if (css[i++] == quote) {
            break;
        }
    }
    return i;
}

std::size_t find_matching_brace(std::string_view css, std::size_t open)
{
    int depth = 0;
    for (std::size_t i = open; i < css.size(); ++i) {
        const char ch = css[i];
        if (ch == '"' || ch == '\'') {
            i = skip_css_string(css, i) - 1;
            continue;
        }
        if (ch == '/' && i + 1 < css.size() && css[i + 1] == '*') {
            i += 2;
            while (i + 1 < css.size() && !(css[i] == '*' && css[i + 1] == '/')) {
                ++i;
            }
            ++i;
            continue;
        }
        if (ch == '{') {
            ++depth;
        } else if (ch == '}') {
            --depth;
            if (depth == 0) {
                return i;
            }
        }
    }
    return std::string_view::npos;
}

std::vector<std::pair<std::string, std::string>> parse_declarations(std::string_view block)
{
    std::vector<std::pair<std::string, std::string>> out;
    std::size_t start = 0;
    int paren_depth = 0;
    for (std::size_t i = 0; i <= block.size(); ++i) {
        if (i < block.size()) {
            const char ch = block[i];
            if (ch == '"' || ch == '\'') {
                i = skip_css_string(block, i) - 1;
                continue;
            }
            if (ch == '(') {
                ++paren_depth;
                continue;
            }
            if (ch == ')' && paren_depth > 0) {
                --paren_depth;
                continue;
            }
            if (ch != ';' || paren_depth != 0) {
                continue;
            }
        }

        const std::string_view declaration = block.substr(start, i - start);
        const auto colon = declaration.find(':');
        if (colon != std::string_view::npos) {
            out.emplace_back(to_lower(trim(declaration.substr(0, colon))), trim(declaration.substr(colon + 1)));
        }
        start = i + 1;
    }
    return out;
}

std::vector<std::string> parse_src_urls(std::string_view value)
{
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < value.size()) {
        const char ch = value[i];
        if (ch == '"' || ch == '\'') {
            i = skip_css_string(value, i);
            continue;
        }
        if ((ch == 'u' || ch == 'U') && i + 3 < value.size() && iequals_prefix(value.substr(i), "url")) {
            std::size_t j = i + 3;
            while (j < value.size() && std::isspace(static_cast<unsigned char>(value[j]))) {
                ++j;
            }
            if (j >= value.size() || value[j] != '(') {
                ++i;
                continue;
            }
            ++j;
            std::string target = parse_css_url_target(value, j);
            if (!target.empty()) {
                out.push_back(std::move(target));
            }
            i = j;
            continue;
        }
        ++i;
    }
    return out;
}

int parse_font_weight(std::string_view value)
{
    const std::string text = to_lower(trim(value));
    if (text == "normal") {
        return 400;
    }
    if (text == "bold") {
        return 700;
    }
    std::size_t i = 0;
    while (i < text.size() && !std::isdigit(static_cast<unsigned char>(text[i]))) {
        ++i;
    }
    if (i == text.size()) {
        return 400;
    }
    std::size_t end = i;
    while (end < text.size() && std::isdigit(static_cast<unsigned char>(text[end]))) {
        ++end;
    }
    try {
        return std::clamp(std::stoi(text.substr(i, end - i)), 100, 1000);
    } catch (...) {
        return 400;
    }
}

std::uint16_t read_u16_be(std::string_view bytes, std::size_t offset)
{
    return static_cast<std::uint16_t>(
        (static_cast<unsigned char>(bytes[offset]) << 8)
        | static_cast<unsigned char>(bytes[offset + 1]));
}

std::uint32_t read_u32_be(std::string_view bytes, std::size_t offset)
{
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset])) << 24)
        | (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 1])) << 16)
        | (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 2])) << 8)
        | static_cast<unsigned char>(bytes[offset + 3]);
}

void write_u16_be(std::string& out, std::size_t offset, std::uint16_t value)
{
    out[offset] = static_cast<char>((value >> 8) & 0xff);
    out[offset + 1] = static_cast<char>(value & 0xff);
}

void write_u32_be(std::string& out, std::size_t offset, std::uint32_t value)
{
    out[offset] = static_cast<char>((value >> 24) & 0xff);
    out[offset + 1] = static_cast<char>((value >> 16) & 0xff);
    out[offset + 2] = static_cast<char>((value >> 8) & 0xff);
    out[offset + 3] = static_cast<char>(value & 0xff);
}

std::uint32_t align4(std::uint32_t value)
{
    return (value + 3u) & ~3u;
}

struct WoffTable {
    std::uint32_t tag = 0;
    std::uint32_t offset = 0;
    std::uint32_t comp_length = 0;
    std::uint32_t orig_length = 0;
    std::uint32_t checksum = 0;
};

std::string decompress_woff1(std::string_view input)
{
    if (input.size() < 44 || input.substr(0, 4) != "wOFF") {
        throw std::runtime_error("invalid WOFF font");
    }

    const std::uint32_t flavor = read_u32_be(input, 4);
    const std::uint32_t length = read_u32_be(input, 8);
    const std::uint16_t num_tables = read_u16_be(input, 12);
    const std::uint32_t total_sfnt_size = read_u32_be(input, 16);
    if (length > input.size() || num_tables == 0 || total_sfnt_size == 0 || total_sfnt_size > kMaxDecodedFontBytes) {
        throw std::runtime_error("invalid WOFF header");
    }
    if (44u + static_cast<std::uint32_t>(num_tables) * 20u > input.size()) {
        throw std::runtime_error("truncated WOFF table directory");
    }

    std::vector<WoffTable> tables;
    tables.reserve(num_tables);
    for (std::uint16_t i = 0; i < num_tables; ++i) {
        const std::size_t entry = 44u + static_cast<std::size_t>(i) * 20u;
        WoffTable table{
            read_u32_be(input, entry),
            read_u32_be(input, entry + 4),
            read_u32_be(input, entry + 8),
            read_u32_be(input, entry + 12),
            read_u32_be(input, entry + 16),
        };
        if (table.offset > input.size() || table.comp_length > input.size() - table.offset
            || table.orig_length > kMaxDecodedFontBytes || table.comp_length > table.orig_length) {
            throw std::runtime_error("invalid WOFF table bounds");
        }
        tables.push_back(table);
    }

    std::uint16_t entry_selector = 0;
    std::uint16_t max_power = 1;
    while (static_cast<std::uint16_t>(max_power * 2) <= num_tables) {
        max_power *= 2;
        ++entry_selector;
    }
    const std::uint16_t search_range = static_cast<std::uint16_t>(max_power * 16);
    const std::uint16_t range_shift = static_cast<std::uint16_t>(num_tables * 16 - search_range);

    const std::uint32_t table_directory_size = 12u + static_cast<std::uint32_t>(num_tables) * 16u;
    std::uint32_t data_offset = align4(table_directory_size);
    if (total_sfnt_size < table_directory_size || data_offset > total_sfnt_size) {
        throw std::runtime_error("WOFF sfnt size is too small");
    }

    std::string out(total_sfnt_size, '\0');
    write_u32_be(out, 0, flavor);
    write_u16_be(out, 4, num_tables);
    write_u16_be(out, 6, search_range);
    write_u16_be(out, 8, entry_selector);
    write_u16_be(out, 10, range_shift);

    for (std::size_t i = 0; i < tables.size(); ++i) {
        const auto& table = tables[i];
        if (data_offset > out.size() || table.orig_length > out.size() - data_offset) {
            throw std::runtime_error("WOFF sfnt size is too small");
        }

        std::string table_data(table.orig_length, '\0');
        if (table.comp_length == table.orig_length) {
            std::copy_n(input.data() + table.offset, table.orig_length, table_data.data());
        } else {
            uLongf uncompressed = table.orig_length;
            const int status = uncompress(
                reinterpret_cast<Bytef*>(table_data.data()),
                &uncompressed,
                reinterpret_cast<const Bytef*>(input.data() + table.offset),
                table.comp_length);
            if (status != Z_OK || uncompressed != table.orig_length) {
                throw std::runtime_error("failed to decompress WOFF table");
            }
        }

        const std::size_t entry = 12u + i * 16u;
        write_u32_be(out, entry, table.tag);
        write_u32_be(out, entry + 4, table.checksum);
        write_u32_be(out, entry + 8, data_offset);
        write_u32_be(out, entry + 12, table.orig_length);
        std::copy(table_data.begin(), table_data.end(), out.begin() + data_offset);
        data_offset = align4(data_offset + table.orig_length);
    }

    return out;
}

std::string decompress_woff2(std::string_view input)
{
    std::string out;
    woff2::WOFF2StringOut writer(&out);
    writer.SetMaxSize(kMaxDecodedFontBytes);
    if (!woff2::ConvertWOFF2ToTTF(
            reinterpret_cast<const std::uint8_t*>(input.data()),
            input.size(),
            &writer)) {
        throw std::runtime_error("failed to decompress WOFF2 font");
    }
    out.resize(writer.Size());
    return out;
}

std::string decode_font_to_sfnt(std::string_view input)
{
    if (input.size() < 4) {
        throw std::runtime_error("font is too small");
    }
    constexpr char kTrueTypeMagic[] = {'\0', '\1', '\0', '\0'};
    if (starts_with(input, std::string_view(kTrueTypeMagic, sizeof(kTrueTypeMagic))) || starts_with(input, "OTTO")
        || starts_with(input, "true") || starts_with(input, "typ1")) {
        if (input.size() > kMaxDecodedFontBytes) {
            throw std::runtime_error("font is too large");
        }
        return std::string(input);
    }
    if (starts_with(input, "wOFF")) {
        return decompress_woff1(input);
    }
    if (starts_with(input, "wOF2")) {
        return decompress_woff2(input);
    }
    throw std::runtime_error("unsupported font format");
}

std::filesystem::path make_temp_dir()
{
    static std::atomic<unsigned long long> counter{0};
    const auto base = std::filesystem::temp_directory_path();
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (int attempt = 0; attempt < 100; ++attempt) {
        auto path = base / ("pagecore-fonts-" + std::to_string(stamp) + "-"
            + std::to_string(counter.fetch_add(1)));
        std::error_code error;
        if (std::filesystem::create_directory(path, error)) {
            return path;
        }
    }
    throw std::runtime_error("failed to create temporary font directory");
}

void write_binary_file(const std::filesystem::path& path, std::string_view bytes)
{
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        throw std::runtime_error("failed to create temporary font file");
    }
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out) {
        throw std::runtime_error("failed to write temporary font file");
    }
}

std::string query_font_family(const std::filesystem::path& path)
{
    int count = 0;
    FcPattern* pattern = FcFreeTypeQuery(
        reinterpret_cast<const FcChar8*>(path.string().c_str()),
        0,
        nullptr,
        &count);
    if (pattern == nullptr) {
        return {};
    }
    std::unique_ptr<FcPattern, decltype(&FcPatternDestroy)> guard(pattern, FcPatternDestroy);
    FcChar8* family = nullptr;
    if (FcPatternGetString(pattern, FC_FAMILY, 0, &family) != FcResultMatch || family == nullptr) {
        return {};
    }
    return reinterpret_cast<const char*>(family);
}

void font_substitute(FcPattern* pattern, gpointer data)
{
    const auto* aliases = static_cast<const std::unordered_map<std::string, std::string>*>(data);
    if (aliases == nullptr || aliases->empty()) {
        return;
    }

    FcChar8* requested = nullptr;
    if (FcPatternGetString(pattern, FC_FAMILY, 0, &requested) != FcResultMatch || requested == nullptr) {
        return;
    }

    auto found = aliases->find(reinterpret_cast<const char*>(requested));
    if (found == aliases->end()) {
        return;
    }
    FcPatternAddString(pattern, FC_FAMILY, reinterpret_cast<const FcChar8*>(found->second.c_str()));
}

} // namespace

struct FontEnvironment {
    FontEnvironment()
        : temp_dir(make_temp_dir())
        , config(create_font_config())
        , font_map(create_font_map())
    {
        if (config == nullptr || font_map == nullptr || !PANGO_IS_FC_FONT_MAP(font_map)) {
            throw std::runtime_error("failed to create font environment");
        }

        const std::filesystem::path cache_dir = temp_dir / "cache";
        std::filesystem::create_directory(cache_dir);
        const std::string xml =
            "<?xml version=\"1.0\"?>"
            "<!DOCTYPE fontconfig SYSTEM \"fonts.dtd\">"
            "<fontconfig>"
            "<include ignore_missing=\"yes\">fonts.conf</include>"
            "<cachedir>" + cache_dir.string() + "</cachedir>"
            "</fontconfig>";
        FcConfigParseAndLoadFromMemory(config, reinterpret_cast<const FcChar8*>(xml.c_str()), FcFalse);
        FcConfigPreferAppFont(config, FcTrue);
        pango_fc_font_map_set_config(PANGO_FC_FONT_MAP(font_map), config);
        pango_fc_font_map_set_default_substitute(PANGO_FC_FONT_MAP(font_map), font_substitute, &aliases, nullptr);
    }

    FontEnvironment(const FontEnvironment&) = delete;
    FontEnvironment& operator=(const FontEnvironment&) = delete;

    ~FontEnvironment()
    {
        if (font_map != nullptr) {
            g_object_unref(font_map);
        }
        if (config != nullptr) {
            FcConfigDestroy(config);
        }
        std::error_code ignored;
        std::filesystem::remove_all(temp_dir, ignored);
    }

    bool add_font(const WebFontSource& source, std::size_t index)
    {
        std::string sfnt;
        try {
            sfnt = decode_font_to_sfnt(source.body);
        } catch (const std::runtime_error&) {
            return false;
        }

        const std::filesystem::path path = temp_dir / ("font-" + std::to_string(index) + ".ttf");
        try {
            write_binary_file(path, sfnt);
        } catch (const std::runtime_error&) {
            return false;
        }

        if (!FcConfigAppFontAddFile(config, reinterpret_cast<const FcChar8*>(path.string().c_str()))) {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
            return false;
        }

        const std::string internal_family = query_font_family(path);
        if (!source.css_family.empty() && !internal_family.empty() && source.css_family != internal_family) {
            aliases.emplace(source.css_family, internal_family);
        }
        font_files.push_back(path);
        return true;
    }

    void finish()
    {
        FcConfigBuildFonts(config);
        pango_fc_font_map_config_changed(PANGO_FC_FONT_MAP(font_map));
    }

    std::filesystem::path temp_dir;
    FcConfig* config = nullptr;
    PangoFontMap* font_map = nullptr;
    std::vector<std::filesystem::path> font_files;
    std::unordered_map<std::string, std::string> aliases;
};

std::vector<CssFontFace> extract_font_faces(std::string_view css)
{
    std::vector<CssFontFace> faces;
    for (std::size_t i = 0; i < css.size();) {
        if (css[i] == '"' || css[i] == '\'') {
            i = skip_css_string(css, i);
            continue;
        }
        if (css[i] == '/' && i + 1 < css.size() && css[i + 1] == '*') {
            i += 2;
            while (i + 1 < css.size() && !(css[i] == '*' && css[i + 1] == '/')) {
                ++i;
            }
            i = std::min(css.size(), i + 2);
            continue;
        }
        if (css[i] == '@' && iequals_prefix(css.substr(i), "@font-face")) {
            const auto open = css.find('{', i + 10);
            if (open == std::string_view::npos) {
                break;
            }
            const auto close = find_matching_brace(css, open);
            if (close == std::string_view::npos) {
                break;
            }

            CssFontFace face;
            for (const auto& [property, value] : parse_declarations(css.substr(open + 1, close - open - 1))) {
                if (property == "font-family") {
                    face.family = unquote_css_string(value);
                } else if (property == "src") {
                    face.sources = parse_src_urls(value);
                } else if (property == "font-weight") {
                    face.weight = parse_font_weight(value);
                } else if (property == "font-style") {
                    const std::string style = to_lower(value);
                    face.italic = style.find("italic") != std::string::npos || style.find("oblique") != std::string::npos;
                }
            }
            if (!face.family.empty() && !face.sources.empty()) {
                faces.push_back(std::move(face));
            }
            i = close + 1;
            continue;
        }
        ++i;
    }
    return faces;
}

std::shared_ptr<const FontEnvironment> create_font_environment(const std::vector<WebFontSource>& fonts)
{
    if (fonts.empty()) {
        return nullptr;
    }

    auto environment = std::make_shared<FontEnvironment>();
    std::size_t loaded = 0;
    for (std::size_t i = 0; i < fonts.size(); ++i) {
        if (environment->add_font(fonts[i], i)) {
            ++loaded;
        }
    }
    if (loaded == 0) {
        return nullptr;
    }
    environment->finish();
    return environment;
}

PangoLayout* create_pango_layout_for_cairo(cairo_t* cr, const std::shared_ptr<const FontEnvironment>& font_environment)
{
    ensure_fontconfig_initialized();
    if (!font_environment || font_environment->font_map == nullptr) {
        return pango_cairo_create_layout(cr);
    }

    PangoContext* context = pango_font_map_create_context(font_environment->font_map);
    if (context == nullptr) {
        return nullptr;
    }
    pango_cairo_update_context(cr, context);
    PangoLayout* layout = pango_layout_new(context);
    g_object_unref(context);
    return layout;
}

} // namespace pagecore
