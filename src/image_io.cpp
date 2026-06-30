#include "pagecore/image_io.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace pagecore {
namespace {

constexpr std::array<std::uint8_t, 8> kPngSignature{
    0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n',
};

void append_u32_be(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xff));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
    out.push_back(static_cast<std::uint8_t>(value & 0xff));
}

std::uint32_t crc32(std::string_view type, const std::vector<std::uint8_t>& data)
{
    std::uint32_t crc = 0xffffffffu;
    const auto update = [&](std::uint8_t byte) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1) ^ (0xedb88320u & mask);
        }
    };

    for (unsigned char byte : type) {
        update(byte);
    }
    for (std::uint8_t byte : data) {
        update(byte);
    }
    return crc ^ 0xffffffffu;
}

std::uint32_t adler32(const std::vector<std::uint8_t>& data)
{
    constexpr std::uint32_t kMod = 65521;
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (std::uint8_t byte : data) {
        a = (a + byte) % kMod;
        b = (b + a) % kMod;
    }
    return (b << 16) | a;
}

void append_chunk(std::vector<std::uint8_t>& png, std::string_view type, const std::vector<std::uint8_t>& data)
{
    if (type.size() != 4) {
        throw std::runtime_error("PNG chunk type must be 4 bytes");
    }
    if (data.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
        throw std::runtime_error("PNG chunk is too large");
    }

    append_u32_be(png, static_cast<std::uint32_t>(data.size()));
    png.insert(png.end(), type.begin(), type.end());
    png.insert(png.end(), data.begin(), data.end());
    append_u32_be(png, crc32(type, data));
}

std::vector<std::uint8_t> make_scanlines(const RenderedImage& image)
{
    if (image.width <= 0 || image.height <= 0) {
        throw std::runtime_error("PNG image dimensions must be positive");
    }

    const std::size_t width = static_cast<std::size_t>(image.width);
    const std::size_t height = static_cast<std::size_t>(image.height);
    if (width > (std::numeric_limits<std::size_t>::max() / height / 4)) {
        throw std::runtime_error("PNG image dimensions are too large");
    }

    const std::size_t expected_rgba = width * height * 4;
    if (image.rgba.size() != expected_rgba) {
        throw std::runtime_error("PNG image RGBA buffer size does not match dimensions");
    }

    const std::size_t stride = width * 4;
    if (height > (std::numeric_limits<std::size_t>::max() / (stride + 1))) {
        throw std::runtime_error("PNG image is too large");
    }

    std::vector<std::uint8_t> scanlines;
    scanlines.reserve(height * (stride + 1));
    for (std::size_t y = 0; y < height; ++y) {
        scanlines.push_back(0); // filter type 0: none
        const auto begin = image.rgba.begin() + static_cast<std::ptrdiff_t>(y * stride);
        scanlines.insert(scanlines.end(), begin, begin + static_cast<std::ptrdiff_t>(stride));
    }
    return scanlines;
}

std::vector<std::uint8_t> zlib_store(const std::vector<std::uint8_t>& data)
{
    std::vector<std::uint8_t> out;
    out.reserve(data.size() + (data.size() / 65535 + 1) * 5 + 6);
    out.push_back(0x78);
    out.push_back(0x01);

    std::size_t offset = 0;
    do {
        const std::size_t remaining = data.size() - offset;
        const std::uint16_t len = static_cast<std::uint16_t>(remaining > 65535 ? 65535 : remaining);
        const bool final = offset + len == data.size();

        out.push_back(final ? 0x01 : 0x00);
        out.push_back(static_cast<std::uint8_t>(len & 0xff));
        out.push_back(static_cast<std::uint8_t>((len >> 8) & 0xff));
        const std::uint16_t nlen = static_cast<std::uint16_t>(~len);
        out.push_back(static_cast<std::uint8_t>(nlen & 0xff));
        out.push_back(static_cast<std::uint8_t>((nlen >> 8) & 0xff));

        out.insert(out.end(), data.begin() + static_cast<std::ptrdiff_t>(offset), data.begin() + static_cast<std::ptrdiff_t>(offset + len));
        offset += len;
    } while (offset < data.size());

    append_u32_be(out, adler32(data));
    return out;
}

} // namespace

std::vector<std::uint8_t> encode_png_rgba(const RenderedImage& image)
{
    const auto scanlines = make_scanlines(image);

    std::vector<std::uint8_t> ihdr;
    ihdr.reserve(13);
    append_u32_be(ihdr, static_cast<std::uint32_t>(image.width));
    append_u32_be(ihdr, static_cast<std::uint32_t>(image.height));
    ihdr.push_back(8); // bit depth
    ihdr.push_back(6); // RGBA
    ihdr.push_back(0); // deflate
    ihdr.push_back(0); // adaptive filtering
    ihdr.push_back(0); // no interlace

    std::vector<std::uint8_t> png;
    png.insert(png.end(), kPngSignature.begin(), kPngSignature.end());
    append_chunk(png, "IHDR", ihdr);
    append_chunk(png, "IDAT", zlib_store(scanlines));
    append_chunk(png, "IEND", {});
    return png;
}

void write_png_rgba(const RenderedImage& image, const std::string& path)
{
    const auto png = encode_png_rgba(image);
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        throw std::runtime_error("failed to open PNG output file: " + path);
    }
    out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    if (!out) {
        throw std::runtime_error("failed to write PNG output file: " + path);
    }
}

} // namespace pagecore
