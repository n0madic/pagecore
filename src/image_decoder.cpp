#include "pagecore/image_decoder.hpp"

#include <cairo.h>
#if defined(PAGECORE_IMAGE_DECODER_STB)
#define STBI_NO_STDIO
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_GIF
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#else
#include <gif_lib.h>
#include <turbojpeg.h>
#endif
#if PAGECORE_ENABLE_WEBP
#include <webp/decode.h>
#endif
#if PAGECORE_ENABLE_SVG
#include <lunasvg.h>
#endif

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pagecore {
namespace {

#if defined(PAGECORE_IMAGE_DECODER_STB)
struct StbImageDeleter {
    void operator()(stbi_uc* data) const
    {
        stbi_image_free(data);
    }
};
#else
struct StreamReader {
    std::string_view bytes;
    std::size_t offset = 0;
};

struct TurboJpegDeleter {
    void operator()(tjhandle handle) const
    {
        if (handle != nullptr) {
            (void) tjDestroy(handle);
        }
    }
};

struct GifMemoryReader {
    std::string_view bytes;
    std::size_t offset = 0;
};

struct GifFileDeleter {
    void operator()(GifFileType* gif) const
    {
        if (gif != nullptr) {
            int error = 0;
            (void) DGifCloseFile(gif, &error);
        }
    }
};
#endif

struct SvgViewport {
    int width = 300;
    int height = 150;
    double viewbox_x = 0.0;
    double viewbox_y = 0.0;
    double viewbox_width = 300.0;
    double viewbox_height = 150.0;
    bool has_viewbox = false;
};

std::string trim_copy(std::string_view value)
{
    std::size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) {
        ++begin;
    }

    std::size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return std::string(value.substr(begin, end - begin));
}

std::string lower_copy(std::string_view value)
{
    std::string out(value);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return out;
}

bool parse_double(std::string_view value, double& out)
{
    const std::string text = trim_copy(value);
    if (text.empty()) {
        return false;
    }

    char* end = nullptr;
    errno = 0;
    const double parsed = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || errno == ERANGE || !std::isfinite(parsed)) {
        return false;
    }
    out = parsed;
    return true;
}

double parse_length(std::string_view value, double fallback = 0.0)
{
    double parsed = 0.0;
    if (!parse_double(value, parsed)) {
        return fallback;
    }
    return parsed;
}

std::vector<double> parse_number_list(std::string_view value)
{
    std::vector<double> numbers;
    std::string text(value);
    const char* current = text.c_str();
    while (*current != '\0') {
        while (*current != '\0'
               && (std::isspace(static_cast<unsigned char>(*current)) || *current == ',')) {
            ++current;
        }
        if (*current == '\0') {
            break;
        }

        char* end = nullptr;
        errno = 0;
        const double parsed = std::strtod(current, &end);
        if (end == current || errno == ERANGE || !std::isfinite(parsed)) {
            ++current;
            continue;
        }
        numbers.push_back(parsed);
        current = end;
    }
    return numbers;
}

// Extracts the raw value of a single double- or single-quoted attribute from
// a tag's source markup, matching `name` case-insensitively at a word
// boundary. This is a narrow scan for the handful of attributes
// parse_svg_viewport needs from the root <svg> tag as a cheap pre-check --
// not a general SVG attribute parser (lunasvg owns real attribute parsing).
std::optional<std::string> find_svg_attribute(std::string_view tag_markup, std::string_view name)
{
    const std::string lower_markup = lower_copy(tag_markup);
    const std::string lower_name = lower_copy(name);

    std::size_t search_from = 0;
    while (true) {
        const std::size_t match = lower_markup.find(lower_name, search_from);
        if (match == std::string::npos) {
            return std::nullopt;
        }
        search_from = match + 1;

        const bool boundary_before = match == 0
            || std::isspace(static_cast<unsigned char>(lower_markup[match - 1]));
        std::size_t after = match + lower_name.size();
        while (after < lower_markup.size() && std::isspace(static_cast<unsigned char>(lower_markup[after]))) {
            ++after;
        }
        if (!boundary_before || after >= lower_markup.size() || lower_markup[after] != '=') {
            continue;
        }

        ++after;
        while (after < lower_markup.size() && std::isspace(static_cast<unsigned char>(lower_markup[after]))) {
            ++after;
        }
        if (after >= lower_markup.size() || (lower_markup[after] != '"' && lower_markup[after] != '\'')) {
            continue;
        }

        const char quote = tag_markup[after];
        const std::size_t value_begin = after + 1;
        const std::size_t value_end = tag_markup.find(quote, value_begin);
        if (value_end == std::string_view::npos) {
            return std::nullopt;
        }
        return std::string(tag_markup.substr(value_begin, value_end - value_begin));
    }
}

SvgViewport parse_svg_viewport(std::string_view bytes)
{
    SvgViewport viewport;
    const std::string lower = lower_copy(bytes.substr(0, std::min<std::size_t>(bytes.size(), 4096)));
    const auto svg_pos = lower.find("<svg");
    if (svg_pos == std::string::npos) {
        throw std::runtime_error("decode SVG: missing root <svg>");
    }
    const auto tag_end = bytes.find('>', svg_pos);
    if (tag_end == std::string_view::npos) {
        throw std::runtime_error("decode SVG: unterminated root <svg>");
    }

    const std::string_view root_tag = bytes.substr(svg_pos, tag_end - svg_pos + 1);
    if (auto viewbox = find_svg_attribute(root_tag, "viewbox")) {
        const auto numbers = parse_number_list(*viewbox);
        if (numbers.size() >= 4 && numbers[2] > 0.0 && numbers[3] > 0.0) {
            viewport.viewbox_x = numbers[0];
            viewport.viewbox_y = numbers[1];
            viewport.viewbox_width = numbers[2];
            viewport.viewbox_height = numbers[3];
            viewport.has_viewbox = true;
        }
    }

    // Clamp the (attacker-controlled) length to a safe range as a double before
    // narrowing to int -- casting an out-of-range double to int is UB.
    const auto to_dimension = [](double value) {
        if (!std::isfinite(value)) {
            return 1;
        }
        return static_cast<int>(std::round(std::clamp(value, 1.0, 8192.0)));
    };
    if (auto width = find_svg_attribute(root_tag, "width")) {
        viewport.width = to_dimension(parse_length(*width, viewport.width));
    } else if (viewport.has_viewbox) {
        viewport.width = to_dimension(viewport.viewbox_width);
    }
    if (auto height = find_svg_attribute(root_tag, "height")) {
        viewport.height = to_dimension(parse_length(*height, viewport.height));
    } else if (viewport.has_viewbox) {
        viewport.height = to_dimension(viewport.viewbox_height);
    }

    viewport.width = std::clamp(viewport.width, 1, 8192);
    viewport.height = std::clamp(viewport.height, 1, 8192);
    if (!viewport.has_viewbox) {
        viewport.viewbox_width = viewport.width;
        viewport.viewbox_height = viewport.height;
    }
    return viewport;
}

bool is_png(std::string_view bytes)
{
    constexpr std::array<unsigned char, 8> signature{0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    return bytes.size() >= signature.size()
        && std::memcmp(bytes.data(), signature.data(), signature.size()) == 0;
}

bool is_jpeg(std::string_view bytes)
{
    return bytes.size() >= 3
        && static_cast<unsigned char>(bytes[0]) == 0xff
        && static_cast<unsigned char>(bytes[1]) == 0xd8
        && static_cast<unsigned char>(bytes[2]) == 0xff;
}

bool is_webp(std::string_view bytes)
{
    return bytes.size() >= 12
        && std::memcmp(bytes.data(), "RIFF", 4) == 0
        && std::memcmp(bytes.data() + 8, "WEBP", 4) == 0;
}

bool is_gif(std::string_view bytes)
{
    return bytes.size() >= 6
        && (std::memcmp(bytes.data(), "GIF87a", 6) == 0
            || std::memcmp(bytes.data(), "GIF89a", 6) == 0);
}

bool is_svg(std::string_view bytes)
{
    const std::size_t limit = std::min<std::size_t>(bytes.size(), 2048);
    const std::string sample = lower_copy(bytes.substr(0, limit));
    return sample.find("<svg") != std::string::npos;
}

bool is_jpeg_sof_marker(unsigned char marker)
{
    return marker >= 0xc0
        && marker <= 0xcf
        && marker != 0xc4
        && marker != 0xc8
        && marker != 0xcc;
}

bool has_jpeg_eoi_marker(std::string_view bytes)
{
    for (std::size_t offset = 0; offset + 1 < bytes.size(); ++offset) {
        if (static_cast<unsigned char>(bytes[offset]) == 0xff
            && static_cast<unsigned char>(bytes[offset + 1]) == 0xd9) {
            return true;
        }
    }
    return false;
}

std::optional<std::pair<int, int>> jpeg_dimensions_from_header(std::string_view bytes)
{
    if (!is_jpeg(bytes)) {
        return std::nullopt;
    }

    std::size_t offset = 2;
    while (offset + 1 < bytes.size()) {
        if (static_cast<unsigned char>(bytes[offset]) != 0xff) {
            ++offset;
            continue;
        }

        while (offset < bytes.size() && static_cast<unsigned char>(bytes[offset]) == 0xff) {
            ++offset;
        }
        if (offset >= bytes.size()) {
            break;
        }

        const auto marker = static_cast<unsigned char>(bytes[offset++]);
        if (marker == 0x00 || marker == 0xff) {
            continue;
        }
        if (marker == 0xd9 || (marker >= 0xd0 && marker <= 0xd7)) {
            continue;
        }
        if (offset + 2 > bytes.size()) {
            break;
        }

        const auto segment_length =
            (static_cast<std::size_t>(static_cast<unsigned char>(bytes[offset])) << 8)
            | static_cast<std::size_t>(static_cast<unsigned char>(bytes[offset + 1]));
        if (segment_length < 2 || offset + segment_length > bytes.size()) {
            break;
        }
        if (is_jpeg_sof_marker(marker)) {
            if (segment_length < 7 || offset + 7 > bytes.size()) {
                break;
            }
            const int height =
                (static_cast<int>(static_cast<unsigned char>(bytes[offset + 3])) << 8)
                | static_cast<int>(static_cast<unsigned char>(bytes[offset + 4]));
            const int width =
                (static_cast<int>(static_cast<unsigned char>(bytes[offset + 5])) << 8)
                | static_cast<int>(static_cast<unsigned char>(bytes[offset + 6]));
            return std::pair<int, int>{width, height};
        }
        if (marker == 0xda) {
            break;
        }
        offset += segment_length;
    }

    return std::nullopt;
}

std::optional<std::pair<std::uint32_t, std::uint32_t>> png_dimensions_from_header(std::string_view bytes)
{
    // PNG layout: 8-byte signature, then the first chunk which must be IHDR
    // (4-byte length, "IHDR", 13-byte data starting with width/height as BE u32).
    if (!is_png(bytes) || bytes.size() < 24 || std::memcmp(bytes.data() + 12, "IHDR", 4) != 0) {
        return std::nullopt;
    }
    const auto read_be32 = [&](std::size_t at) {
        return (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at])) << 24)
            | (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 1])) << 16)
            | (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 2])) << 8)
            | static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 3]));
    };
    return std::pair<std::uint32_t, std::uint32_t>{read_be32(16), read_be32(20)};
}

std::uint8_t unpremultiply(std::uint8_t value, std::uint8_t alpha)
{
    if (alpha == 0) {
        return 0;
    }
    return static_cast<std::uint8_t>(std::clamp((static_cast<int>(value) * 255 + alpha / 2) / alpha, 0, 255));
}

constexpr std::size_t kRgbaChannels = 4;
constexpr std::size_t kMaxDecodedImageBytes = 128ull * 1024ull * 1024ull;
constexpr std::size_t kMaxDecodedImagePixels = kMaxDecodedImageBytes / kRgbaChannels;

std::size_t checked_rgba_size(int width, int height, const char* operation)
{
    if (width <= 0 || height <= 0) {
        throw std::runtime_error(std::string(operation) + ": invalid image dimensions");
    }

    const auto checked_width = static_cast<std::size_t>(width);
    const auto checked_height = static_cast<std::size_t>(height);
    if (checked_width > std::numeric_limits<std::size_t>::max() / checked_height) {
        throw std::runtime_error(std::string(operation) + ": decoded image is too large");
    }

    const std::size_t pixels = checked_width * checked_height;
    if (pixels > kMaxDecodedImagePixels || pixels > std::numeric_limits<std::size_t>::max() / kRgbaChannels) {
        throw std::runtime_error(std::string(operation) + ": decoded image is too large");
    }

    const std::size_t bytes = pixels * kRgbaChannels;
    if (bytes > kMaxDecodedImageBytes) {
        throw std::runtime_error(std::string(operation) + ": decoded image is too large");
    }
    return bytes;
}

void resize_rgba(DecodedImage& image, int width, int height, const char* operation)
{
    image.width = width;
    image.height = height;
    image.rgba.resize(checked_rgba_size(width, height, operation));
}

// Rejects dimensions parsed straight from an image header (before any decoder
// allocates), so a crafted header cannot force a huge allocation that the
// post-decode checked_rgba_size guard would only catch after the fact.
void enforce_decoded_pixel_budget(std::uint64_t width, std::uint64_t height, const char* operation)
{
    if (width == 0 || height == 0) {
        throw std::runtime_error(std::string(operation) + ": invalid image dimensions");
    }
    if (width > static_cast<std::uint64_t>(kMaxDecodedImagePixels) / height
        || width * height > static_cast<std::uint64_t>(kMaxDecodedImagePixels)) {
        throw std::runtime_error(std::string(operation) + ": decoded image is too large");
    }
}

#if !defined(PAGECORE_IMAGE_DECODER_STB)
// Walks the GIF block structure (without decoding LZW) and enforces the pixel
// budget on the logical screen, every frame descriptor, and their cumulative
// pixel count, before DGifSlurp allocates any raster buffers. giflib imposes no
// size limit of its own, so a tiny file declaring 65535x65535 (or a flood of
// frames) would otherwise force a multi-GB allocation inside DGifSlurp.
void enforce_gif_dimension_budget(std::string_view bytes, const char* operation)
{
    if (bytes.size() < 13) {
        return; // Truncated header; giflib will reject it.
    }
    const auto u16 = [&](std::size_t at) {
        return static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at]))
            | (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 1])) << 8);
    };

    enforce_decoded_pixel_budget(u16(6), u16(8), operation);

    std::uint64_t total_pixels = 0;
    std::size_t offset = 13;
    const unsigned char screen_packed = static_cast<unsigned char>(bytes[10]);
    if (screen_packed & 0x80u) {
        offset += 3u * (1u << ((screen_packed & 0x07u) + 1));
    }

    while (offset < bytes.size()) {
        const unsigned char block = static_cast<unsigned char>(bytes[offset++]);
        if (block == 0x3Bu) { // Trailer.
            break;
        }
        if (block == 0x21u) { // Extension: label byte + data sub-blocks.
            if (offset >= bytes.size()) {
                break;
            }
            ++offset; // label
            while (offset < bytes.size()) {
                const unsigned char sub = static_cast<unsigned char>(bytes[offset++]);
                if (sub == 0) {
                    break;
                }
                offset += sub;
            }
            continue;
        }
        if (block == 0x2Cu) { // Image descriptor.
            if (offset + 9 > bytes.size()) {
                break;
            }
            const std::uint32_t frame_width = u16(offset + 4);
            const std::uint32_t frame_height = u16(offset + 6);
            enforce_decoded_pixel_budget(frame_width, frame_height, operation);
            total_pixels += static_cast<std::uint64_t>(frame_width) * frame_height;
            if (total_pixels > static_cast<std::uint64_t>(kMaxDecodedImagePixels)) {
                throw std::runtime_error(std::string(operation) + ": decoded image is too large");
            }
            const unsigned char frame_packed = static_cast<unsigned char>(bytes[offset + 8]);
            offset += 9;
            if (frame_packed & 0x80u) {
                offset += 3u * (1u << ((frame_packed & 0x07u) + 1));
            }
            if (offset >= bytes.size()) {
                break;
            }
            ++offset; // LZW minimum code size.
            while (offset < bytes.size()) {
                const unsigned char sub = static_cast<unsigned char>(bytes[offset++]);
                if (sub == 0) {
                    break;
                }
                offset += sub;
            }
            continue;
        }
        break; // Unknown block; leave the rest for giflib to reject.
    }
}
#endif

#if !defined(PAGECORE_IMAGE_DECODER_STB)
int read_gif_stream(GifFileType* gif, GifByteType* data, int length)
{
    auto* reader = static_cast<GifMemoryReader*>(gif->UserData);
    const std::size_t requested = static_cast<std::size_t>(std::max(length, 0));
    const std::size_t available = reader->bytes.size() - std::min(reader->offset, reader->bytes.size());
    const std::size_t count = std::min(requested, available);
    if (count > 0) {
        std::memcpy(data, reader->bytes.data() + static_cast<std::ptrdiff_t>(reader->offset), count);
        reader->offset += count;
    }
    return static_cast<int>(count);
}

cairo_status_t read_png_stream(void* closure, unsigned char* data, unsigned int length)
{
    auto* reader = static_cast<StreamReader*>(closure);
    if (reader->offset + length > reader->bytes.size()) {
        return CAIRO_STATUS_READ_ERROR;
    }

    std::memcpy(data, reader->bytes.data() + static_cast<std::ptrdiff_t>(reader->offset), length);
    reader->offset += length;
    return CAIRO_STATUS_SUCCESS;
}
#endif

std::shared_ptr<const DecodedImage> surface_to_decoded_image(cairo_surface_t* surface, const char* operation)
{
    cairo_surface_flush(surface);
    const cairo_status_t status = cairo_surface_status(surface);
    if (status != CAIRO_STATUS_SUCCESS) {
        throw std::runtime_error(std::string(operation) + ": " + cairo_status_to_string(status));
    }

    auto image = std::make_shared<DecodedImage>();
    resize_rgba(*image, cairo_image_surface_get_width(surface), cairo_image_surface_get_height(surface), operation);

    const cairo_format_t format = cairo_image_surface_get_format(surface);
    const int stride = cairo_image_surface_get_stride(surface);
    const auto* pixels = cairo_image_surface_get_data(surface);
    for (int y = 0; y < image->height; ++y) {
        const auto* row = pixels + static_cast<std::ptrdiff_t>(y) * stride;
        for (int x = 0; x < image->width; ++x) {
            std::uint32_t native_argb = 0;
            std::memcpy(&native_argb, row + static_cast<std::ptrdiff_t>(x) * 4, sizeof(native_argb));

            const auto alpha = format == CAIRO_FORMAT_RGB24
                ? static_cast<std::uint8_t>(255)
                : static_cast<std::uint8_t>((native_argb >> 24) & 0xff);
            const auto red = static_cast<std::uint8_t>((native_argb >> 16) & 0xff);
            const auto green = static_cast<std::uint8_t>((native_argb >> 8) & 0xff);
            const auto blue = static_cast<std::uint8_t>(native_argb & 0xff);

            auto* out = &image->rgba[(static_cast<std::size_t>(y) * image->width + x) * 4];
            out[0] = unpremultiply(red, alpha);
            out[1] = unpremultiply(green, alpha);
            out[2] = unpremultiply(blue, alpha);
            out[3] = alpha;
        }
    }

    return image;
}

#if defined(PAGECORE_IMAGE_DECODER_STB)
std::string stb_failure_message(const char* operation)
{
    const char* reason = stbi_failure_reason();
    if (reason == nullptr || reason[0] == '\0') {
        reason = "decode failed";
    }
    return std::string(operation) + ": " + reason;
}

std::shared_ptr<const DecodedImage> decode_stb_rgba(std::string_view bytes, const char* operation)
{
    if (bytes.empty()) {
        throw std::runtime_error(std::string(operation) + ": empty input");
    }
    if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error(std::string(operation) + ": input is too large");
    }

    const auto* input = reinterpret_cast<const stbi_uc*>(bytes.data());
    const int length = static_cast<int>(bytes.size());

    int width = 0;
    int height = 0;
    int channels = 0;
    if (stbi_info_from_memory(input, length, &width, &height, &channels) == 0) {
        throw std::runtime_error(stb_failure_message(operation));
    }
    (void) channels;
    (void) checked_rgba_size(width, height, operation);

    int decoded_width = 0;
    int decoded_height = 0;
    int decoded_channels = 0;
    std::unique_ptr<stbi_uc, StbImageDeleter> decoded(stbi_load_from_memory(
        input,
        length,
        &decoded_width,
        &decoded_height,
        &decoded_channels,
        STBI_rgb_alpha));
    if (!decoded) {
        throw std::runtime_error(stb_failure_message(operation));
    }
    (void) decoded_channels;

    auto image = std::make_shared<DecodedImage>();
    resize_rgba(*image, decoded_width, decoded_height, operation);
    std::copy(decoded.get(), decoded.get() + image->rgba.size(), image->rgba.begin());
    return image;
}
#endif

} // namespace

std::shared_ptr<const DecodedImage> decode_image_rgba(std::string_view bytes)
{
    if (is_png(bytes)) {
        return decode_png_rgba(bytes);
    }
    if (is_jpeg(bytes)) {
        return decode_jpeg_rgba(bytes);
    }
    if (is_webp(bytes)) {
        return decode_webp_rgba(bytes);
    }
    if (is_gif(bytes)) {
        return decode_gif_rgba(bytes);
    }
    if (is_svg(bytes)) {
        return decode_svg_rgba(bytes);
    }
    throw std::runtime_error("decode image: unsupported image format");
}

std::shared_ptr<const DecodedImage> decode_png_rgba(std::string_view bytes)
{
#if defined(PAGECORE_IMAGE_DECODER_STB)
    return decode_stb_rgba(bytes, "decode PNG");
#else
    if (bytes.empty()) {
        throw std::runtime_error("decode PNG: empty input");
    }
    if (auto dimensions = png_dimensions_from_header(bytes)) {
        enforce_decoded_pixel_budget(dimensions->first, dimensions->second, "decode PNG");
    }

    StreamReader reader{bytes};
    cairo_surface_t* raw_surface = cairo_image_surface_create_from_png_stream(read_png_stream, &reader);
    std::unique_ptr<cairo_surface_t, decltype(&cairo_surface_destroy)> surface(raw_surface, cairo_surface_destroy);

    const cairo_status_t status = cairo_surface_status(surface.get());
    if (status != CAIRO_STATUS_SUCCESS) {
        throw std::runtime_error(std::string("decode PNG: ") + cairo_status_to_string(status));
    }

    return surface_to_decoded_image(surface.get(), "decode PNG");
#endif
}

std::shared_ptr<const DecodedImage> decode_jpeg_rgba(std::string_view bytes)
{
    if (bytes.empty()) {
        throw std::runtime_error("decode JPEG: empty input");
    }
    if (!has_jpeg_eoi_marker(bytes)) {
        throw std::runtime_error("decode JPEG: truncated input");
    }
    if (auto dimensions = jpeg_dimensions_from_header(bytes)) {
        (void) checked_rgba_size(dimensions->first, dimensions->second, "decode JPEG");
    }

#if defined(PAGECORE_IMAGE_DECODER_STB)
    return decode_stb_rgba(bytes, "decode JPEG");
#else
    tjhandle handle = tjInitDecompress();
    if (handle == nullptr) {
        throw std::runtime_error("decode JPEG: failed to initialize TurboJPEG");
    }
    std::unique_ptr<void, TurboJpegDeleter> cleanup(handle);

    int width = 0;
    int height = 0;
    int subsamp = 0;
    int colorspace = 0;
    if (tjDecompressHeader3(
            handle,
            reinterpret_cast<const unsigned char*>(bytes.data()),
            static_cast<unsigned long>(bytes.size()),
            &width,
            &height,
            &subsamp,
            &colorspace)
        != 0) {
        throw std::runtime_error(std::string("decode JPEG: ") + tjGetErrorStr2(handle));
    }

    auto image = std::make_shared<DecodedImage>();
    resize_rgba(*image, width, height, "decode JPEG");

    if (tjDecompress2(
            handle,
            reinterpret_cast<const unsigned char*>(bytes.data()),
            static_cast<unsigned long>(bytes.size()),
            image->rgba.data(),
            width,
            0,
            height,
            TJPF_RGBA,
            TJFLAG_ACCURATEDCT)
        != 0) {
        throw std::runtime_error(std::string("decode JPEG: ") + tjGetErrorStr2(handle));
    }

    return image;
#endif
}

std::shared_ptr<const DecodedImage> decode_webp_rgba(std::string_view bytes)
{
#if PAGECORE_ENABLE_WEBP
    if (bytes.empty()) {
        throw std::runtime_error("decode WebP: empty input");
    }

    int width = 0;
    int height = 0;
    if (!WebPGetInfo(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), &width, &height)
        || width <= 0
        || height <= 0) {
        throw std::runtime_error("decode WebP: invalid header");
    }

    auto image = std::make_shared<DecodedImage>();
    resize_rgba(*image, width, height, "decode WebP");

    uint8_t* decoded = WebPDecodeRGBAInto(
        reinterpret_cast<const uint8_t*>(bytes.data()),
        bytes.size(),
        image->rgba.data(),
        image->rgba.size(),
        width * static_cast<int>(kRgbaChannels));
    if (decoded == nullptr) {
        throw std::runtime_error("decode WebP: decode failed");
    }

    return image;
#else
    (void) bytes;
    throw std::runtime_error("decode WebP: WebP support is disabled");
#endif
}

std::shared_ptr<const DecodedImage> decode_gif_rgba(std::string_view bytes)
{
#if defined(PAGECORE_IMAGE_DECODER_STB)
    return decode_stb_rgba(bytes, "decode GIF");
#else
    if (bytes.empty()) {
        throw std::runtime_error("decode GIF: empty input");
    }
    enforce_gif_dimension_budget(bytes, "decode GIF");

    GifMemoryReader reader{bytes};
    int open_error = 0;
    GifFileType* raw_gif = DGifOpen(&reader, read_gif_stream, &open_error);
    if (raw_gif == nullptr) {
        throw std::runtime_error("decode GIF: failed to open image");
    }
    std::unique_ptr<GifFileType, GifFileDeleter> gif(raw_gif);

    if (DGifSlurp(gif.get()) != GIF_OK) {
        throw std::runtime_error("decode GIF: failed to decode image data");
    }
    if (gif->SWidth <= 0 || gif->SHeight <= 0 || gif->ImageCount <= 0 || gif->SavedImages == nullptr) {
        throw std::runtime_error("decode GIF: missing first frame");
    }

    const SavedImage& frame = gif->SavedImages[0];
    const GifImageDesc& desc = frame.ImageDesc;
    const ColorMapObject* color_map = desc.ColorMap != nullptr ? desc.ColorMap : gif->SColorMap;
    if (color_map == nullptr || color_map->ColorCount <= 0 || frame.RasterBits == nullptr) {
        throw std::runtime_error("decode GIF: missing color table");
    }

    std::optional<int> transparent_index;
    for (int i = 0; i < frame.ExtensionBlockCount; ++i) {
        const ExtensionBlock& extension = frame.ExtensionBlocks[i];
        if (extension.Function == GRAPHICS_EXT_FUNC_CODE && extension.ByteCount >= 4 && extension.Bytes != nullptr) {
            if ((extension.Bytes[0] & 0x01) != 0) {
                transparent_index = extension.Bytes[3];
            }
        }
    }

    auto image = std::make_shared<DecodedImage>();
    image->width = gif->SWidth;
    image->height = gif->SHeight;
    image->rgba.assign(checked_rgba_size(image->width, image->height, "decode GIF"), 0);

    const int frame_width = std::max(0, desc.Width);
    const int frame_height = std::max(0, desc.Height);
    for (int y = 0; y < frame_height; ++y) {
        const int dst_y = desc.Top + y;
        if (dst_y < 0 || dst_y >= image->height) {
            continue;
        }
        for (int x = 0; x < frame_width; ++x) {
            const int dst_x = desc.Left + x;
            if (dst_x < 0 || dst_x >= image->width) {
                continue;
            }

            const int index = frame.RasterBits[static_cast<std::size_t>(y) * frame_width + x];
            if (transparent_index && index == *transparent_index) {
                continue;
            }
            if (index < 0 || index >= color_map->ColorCount) {
                continue;
            }

            const GifColorType& color = color_map->Colors[index];
            auto* out = &image->rgba[(static_cast<std::size_t>(dst_y) * image->width + dst_x) * 4];
            out[0] = color.Red;
            out[1] = color.Green;
            out[2] = color.Blue;
            out[3] = 255;
        }
    }

    return image;
#endif
}

std::shared_ptr<const DecodedImage> decode_svg_rgba(std::string_view bytes)
{
#if PAGECORE_ENABLE_SVG
    if (bytes.empty()) {
        throw std::runtime_error("decode SVG: empty input");
    }

    // Cheap pre-check on the declared root <svg> dimensions before handing the
    // document to lunasvg, so a crafted width/height/viewBox cannot force a
    // costly parse/layout of a document whose declared size is already
    // rejected by the decoded-image byte budget.
    const SvgViewport viewport = parse_svg_viewport(bytes);
    (void) checked_rgba_size(viewport.width, viewport.height, "decode SVG");

    auto document = lunasvg::Document::loadFromData(bytes.data(), bytes.size());
    if (!document) {
        throw std::runtime_error("decode SVG: parse failed");
    }

    // Authoritative check against lunasvg's own reported intrinsic size,
    // before renderToBitmap allocates the real pixel buffer.
    const auto to_dimension = [](float value) {
        if (!std::isfinite(value)) {
            return 1;
        }
        return static_cast<int>(std::lround(std::clamp(value, 1.0f, 8192.0f)));
    };
    const int width = to_dimension(document->width());
    const int height = to_dimension(document->height());
    (void) checked_rgba_size(width, height, "decode SVG");

    lunasvg::Bitmap bitmap = document->renderToBitmap(width, height, 0x00000000);
    if (bitmap.isNull()) {
        throw std::runtime_error("decode SVG: render failed");
    }
    bitmap.convertToRGBA();

    auto image = std::make_shared<DecodedImage>();
    resize_rgba(*image, width, height, "decode SVG");
    const std::uint8_t* pixels = bitmap.data();
    const int stride = bitmap.stride();
    const std::size_t row_bytes = static_cast<std::size_t>(width) * kRgbaChannels;
    for (int y = 0; y < height; ++y) {
        std::memcpy(
            &image->rgba[static_cast<std::size_t>(y) * row_bytes],
            pixels + static_cast<std::ptrdiff_t>(y) * stride,
            row_bytes);
    }

    return image;
#else
    (void) bytes;
    throw std::runtime_error("decode SVG: SVG support is disabled");
#endif
}

} // namespace pagecore
