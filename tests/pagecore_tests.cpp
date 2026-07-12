#include "async_loader.hpp"
#include "base64_codec.hpp"
#include "css_scan.hpp"
#include "curl_async_loader.hpp"
#include "event_loop.hpp"
#include "page_activity_tracker.hpp"
#include "script_type.hpp"
#include "util.hpp"

#include "pagecore/image_io.hpp"
#include "pagecore/image_decoder.hpp"
#include "pagecore/dom.hpp"
#include "pagecore/page.hpp"
#include "pagecore/render.hpp"
#include "pagecore/resource_loader.hpp"

#if defined(PAGECORE_ENABLE_RENDERING) && defined(PAGECORE_ENABLE_WEBP) && PAGECORE_ENABLE_WEBP
#include <webp/encode.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

// Some rendering assertions are pinned to a specific font-rendering environment
// (exact glyph metrics, text-run counts, pixel coordinates) and only hold where
// the reference artifacts were generated. They are skipped when
// PAGECORE_SKIP_ENV_SENSITIVE_TESTS is set (e.g. on cross-platform CI), while the
// font-independent bulk of the suite still runs.
bool skip_env_sensitive_tests()
{
    static const bool skip = std::getenv("PAGECORE_SKIP_ENV_SENSITIVE_TESTS") != nullptr;
    return skip;
}

#if !defined(_WIN32)
struct BoundTestServer {
    int fd = -1;
    int port = 0;
};

BoundTestServer bind_loopback_test_server(int backlog, std::string_view label)
{
    std::string last_error;
    for (int attempt = 0; attempt < 8; ++attempt) {
        const int server_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd < 0) {
            last_error = std::strerror(errno);
            continue;
        }

        int reuse = 1;
        (void) ::setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        // Bound accept(): if the client makes fewer connections than expected, the
        // server thread's accept() would otherwise block forever and the test would
        // hang. SO_RCVTIMEO makes accept() fail after the deadline so the thread ends.
        timeval accept_timeout{};
        accept_timeout.tv_sec = 15;
        (void) ::setsockopt(server_fd, SOL_SOCKET, SO_RCVTIMEO, &accept_timeout, sizeof(accept_timeout));

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
        address.sin_len = sizeof(address);
#endif

        if (::bind(server_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            last_error = std::strerror(errno);
            ::close(server_fd);
            continue;
        }
        if (::listen(server_fd, backlog) != 0) {
            last_error = std::strerror(errno);
            ::close(server_fd);
            continue;
        }

        socklen_t address_len = sizeof(address);
        if (::getsockname(server_fd, reinterpret_cast<sockaddr*>(&address), &address_len) != 0) {
            last_error = std::strerror(errno);
            ::close(server_fd);
            continue;
        }

        return BoundTestServer{server_fd, ntohs(address.sin_port)};
    }

    throw std::runtime_error(std::string(label) + " test server should bind/listen on loopback: " + last_error);
}
#endif

class RecordingResourceLoader final : public pagecore::ResourceLoader {
public:
    using ResourceLoader::load;

    void add(
        std::string url,
        std::string body,
        std::string mime_type = {},
        std::vector<std::pair<std::string, std::string>> headers = {},
        int status = 200,
        std::string status_text = {})
    {
        const std::string key = url;
        if (!mime_type.empty()
            && std::none_of(headers.begin(), headers.end(), [](const auto& header) {
                   std::string name = header.first;
                   std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) {
                       return static_cast<char>(std::tolower(ch));
                   });
                   return name == "content-type";
               })) {
            headers.emplace_back("Content-Type", mime_type);
        }
        resources_[key] = pagecore::ResourceResponse{
            std::move(url),
            std::move(body),
            status,
            std::move(mime_type),
            pagecore::ResourceKind::Other,
            false,
            status_text.empty() && status == 200 ? "OK" : std::move(status_text),
            std::move(headers),
        };
    }

    pagecore::ResourceResponse load(const pagecore::ResourceRequest& request) override
    {
        requests.push_back(request);
        auto found = resources_.find(request.url);
        if (found == resources_.end()) {
            throw pagecore::ResourceError(pagecore::ResourceErrorCode::NotFound, request.url, "missing test resource");
        }

        pagecore::ResourceResponse response = found->second;
        response.kind = request.kind;
        response.from_cache = false;
        return response;
    }

    std::vector<pagecore::ResourceRequest> requests;

private:
    std::unordered_map<std::string, pagecore::ResourceResponse> resources_;
};

bool has_request_kind(
    const RecordingResourceLoader& loader,
    std::string_view url,
    pagecore::ResourceKind kind)
{
    for (const auto& request : loader.requests) {
        if (request.url == url && request.kind == kind) {
            return true;
        }
    }
    return false;
}

const pagecore::ResourceRequest* find_request(const RecordingResourceLoader& loader, std::string_view url)
{
    for (const auto& request : loader.requests) {
        if (request.url == url) {
            return &request;
        }
    }
    return nullptr;
}

bool has_header(const pagecore::ResourceRequest& request, std::string_view name, std::string_view value)
{
    for (const auto& [header_name, header_value] : request.headers) {
        if (header_name == name && header_value == value) {
            return true;
        }
    }
    return false;
}

using pagecore::header_name_equals;

bool header_contains(const pagecore::ResourceRequest& request, std::string_view name, std::string_view value)
{
    for (const auto& [header_name, header_value] : request.headers) {
        if (header_name_equals(header_name, name) && header_value.find(value) != std::string::npos) {
            return true;
        }
    }
    return false;
}

const std::uint8_t* pixel_at(const pagecore::RenderedImage& image, int x, int y)
{
    return &image.rgba[(static_cast<std::size_t>(y) * image.width + x) * 4];
}

bool pixel_matches(const pagecore::RenderedImage& image, int x, int y, pagecore::Color color)
{
    const auto* pixel = pixel_at(image, x, y);
    return pixel[0] == color.r && pixel[1] == color.g && pixel[2] == color.b && pixel[3] == color.a;
}

bool pixel_close(const pagecore::RenderedImage& image, int x, int y, pagecore::Color color, int tolerance)
{
    const auto* pixel = pixel_at(image, x, y);
    return std::abs(static_cast<int>(pixel[0]) - color.r) <= tolerance
        && std::abs(static_cast<int>(pixel[1]) - color.g) <= tolerance
        && std::abs(static_cast<int>(pixel[2]) - color.b) <= tolerance
        && std::abs(static_cast<int>(pixel[3]) - color.a) <= tolerance;
}

bool pixel_is_dark(const pagecore::RenderedImage& image, int x, int y)
{
    const auto* pixel = pixel_at(image, x, y);
    return pixel[0] < 80 && pixel[1] < 80 && pixel[2] < 80 && pixel[3] == 255;
}

bool image_has_pixel(const pagecore::RenderedImage& image, pagecore::Color color)
{
    for (std::size_t offset = 0; offset + 3 < image.rgba.size(); offset += 4) {
        if (image.rgba[offset] == color.r
            && image.rgba[offset + 1] == color.g
            && image.rgba[offset + 2] == color.b
            && image.rgba[offset + 3] == color.a) {
            return true;
        }
    }
    return false;
}

bool image_has_close_pixel(const pagecore::RenderedImage& image, pagecore::Color color, int tolerance)
{
    for (std::size_t offset = 0; offset + 3 < image.rgba.size(); offset += 4) {
        const bool matches =
            std::abs(static_cast<int>(image.rgba[offset]) - color.r) <= tolerance
            && std::abs(static_cast<int>(image.rgba[offset + 1]) - color.g) <= tolerance
            && std::abs(static_cast<int>(image.rgba[offset + 2]) - color.b) <= tolerance
            && std::abs(static_cast<int>(image.rgba[offset + 3]) - color.a) <= tolerance;
        if (matches) {
            return true;
        }
    }
    return false;
}

bool region_has_close_pixel(
    const pagecore::RenderedImage& image,
    int x,
    int y,
    int width,
    int height,
    pagecore::Color color,
    int tolerance)
{
    for (int row = std::max(0, y); row < std::min(image.height, y + height); ++row) {
        for (int col = std::max(0, x); col < std::min(image.width, x + width); ++col) {
            if (pixel_close(image, col, row, color, tolerance)) {
                return true;
            }
        }
    }
    return false;
}

bool region_has_saturated_pixel(
    const pagecore::RenderedImage& image,
    int x,
    int y,
    int width,
    int height)
{
    for (int row = std::max(0, y); row < std::min(image.height, y + height); ++row) {
        for (int col = std::max(0, x); col < std::min(image.width, x + width); ++col) {
            const auto* pixel = pixel_at(image, col, row);
            const int max_channel = std::max({pixel[0], pixel[1], pixel[2]});
            const int min_channel = std::min({pixel[0], pixel[1], pixel[2]});
            if (pixel[3] == 255 && max_channel - min_channel > 45) {
                return true;
            }
        }
    }
    return false;
}

bool image_has_non_solid_text_pixel(const pagecore::RenderedImage& image)
{
    for (std::size_t offset = 0; offset + 3 < image.rgba.size(); offset += 4) {
        const bool white = image.rgba[offset] == 255
            && image.rgba[offset + 1] == 255
            && image.rgba[offset + 2] == 255
            && image.rgba[offset + 3] == 255;
        const bool black = image.rgba[offset] == 0
            && image.rgba[offset + 1] == 0
            && image.rgba[offset + 2] == 0
            && image.rgba[offset + 3] == 255;
        if (!white && !black) {
            return true;
        }
    }
    return false;
}

std::string png_body(pagecore::Color color, int width = 2, int height = 2)
{
    pagecore::RenderedImage image;
    image.width = width;
    image.height = height;
    image.rgba.resize(static_cast<std::size_t>(width) * height * 4);
    for (std::size_t offset = 0; offset + 3 < image.rgba.size(); offset += 4) {
        image.rgba[offset] = color.r;
        image.rgba[offset + 1] = color.g;
        image.rgba[offset + 2] = color.b;
        image.rgba[offset + 3] = color.a;
    }

    const auto png = pagecore::encode_png_rgba(image);
    return std::string(reinterpret_cast<const char*>(png.data()), png.size());
}

int count_dark_pixels(
    const pagecore::RenderedImage& image,
    int x,
    int y,
    int width,
    int height)
{
    int count = 0;
    for (int row = std::max(0, y); row < std::min(image.height, y + height); ++row) {
        for (int col = std::max(0, x); col < std::min(image.width, x + width); ++col) {
            if (pixel_is_dark(image, col, row)) {
                ++count;
            }
        }
    }
    return count;
}

std::string pagecore_icon_font_body(std::string_view format)
{
    if (format == "ttf") {
        return pagecore::base64_decode(
            "AAEAAAAKAIAAAwAgT1MvMkUBM38AAAEoAAAAYGNtYXC/8SCdAAABlAAAADxnbHlmKNwP5AAAAdgAAAAa"
            "aGVhZC6lbSAAAACsAAAANmhoZWEFegJcAAAA5AAAACRobXR4BaoAAAAAAYgAAAAMbG9jYQAAAA0AAAHQ"
            "AAAACG1heHAABQAGAAABCAAAACBuYW1lWe7ItwAAAfQAAAFocG9zdEytn5sAAANcAAAAMAABAAAAAQAA"
            "iSYiuV8PPPUAAQPoAAAAAOZqlMIAAAAA5mqUwgAyAAACigK8AAAAAwACAAAAAAAAAAEAAAMg/zgAAAK8"
            "AAAAZAJYAAEAAAAAAAAAAAAAAAAAAAADAAEAAAADAAQAAQAAAAAAAgAAAAAAAAAAAAAAAAAAAAAAAwHj"
            "AZAABQAEAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABEAAAAAAAAAAAAAAAPz8/PwAAAC"
            "DgAAMg/zgAAAMgAMgAAAAAAAAAAAAAAAAAAAAgAAAB9AAAAPoAAAK8AAAAAAACAAAAAwAAABQAAwAB"
            "AAAAFAAEACgAAAAGAAQAAQACACDgAP//AAAAIOAA////4SACAAEAAAAAAAAAAAAAAAAADQABADIAAAKK"
            "ArwAAwAAMyERITICWP2oArwAAAAAAAAMAJYAAQAAAAAAAQAMAAAAAQAAAAAAAgAHAAwAAQAAAAAAAwAU"
            "ABMAAQAAAAAABAAUABMAAQAAAAAABQALACcAAQAAAAAABgAUADIAAwABBAkAAQAYAEYAAwABBAkAAgAO"
            "AF4AAwABBAkAAwAoAGwAAwABBAkABAAoAGwAAwABBAkABQAWAJQAAwABBAkABgAoAKpQYWdlQ29yZUlj"
            "b25SZWd1bGFyUGFnZUNvcmVJY29uIFJlZ3VsYXJWZXJzaW9uIDEuMFBhZ2VDb3JlSWNvbi1SZWd1bGFy"
            "AFAAYQBnAGUAQwBvAHIAZQBJAGMAbwBuAFIAZQBnAHUAbABhAHIAUABhAGcAZQBDAG8AcgBlAEkAYwBv"
            "AG4AIABSAGUAZwB1AGwAYQByAFYAZQByAHMAaQBvAG4AIAAxAC4AMABQAGEAZwBlAEMAbwByAGUASQBj"
            "AG8AbgAtAFIAZQBnAHUAbABhAHIAAgAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAADAAAAAwECB3Vu"
            "aUUwMDA=");
    }
    if (format == "woff") {
        return pagecore::base64_decode(
            "d09GRgABAAAAAALAAAoAAAAAA4wAAQAAAAAAAAAAAAAAAAAAAAAAAAAAAABPUy8yAAABZAAAAC4AAABg"
            "RQEzf2NtYXAAAAGgAAAAMQAAADy/8SCdZ2x5ZgAAAdwAAAAaAAAAGijcD+RoZWFkAAAA9AAAADYAAAA2"
            "LqVtIGhoZWEAAAEsAAAAHwAAACQFegJcaG10eAAAAZQAAAAMAAAADAWqAABsb2NhAAAB1AAAAAgAAAAI"
            "AAAADW1heHAAAAFMAAAAGAAAACAABQAGbmFtZQAAAfgAAACsAAABaFnuyLdwb3N0AAACpAAAABkAAAAw"
            "TK2fmwABAAAAAQAAiSYiuV8PPPUAAQPoAAAAAOZqlMIAAAAA5mqUwgAyAAACigK8AAAAAwACAAAAAAAA"
            "eJxjYGRgYFb4b8HAwLSHgYEhhSmCASiCApgBSkkC3AB4nGNgZGBgYGZgYQDRDAxMDGgAAAEtAAx4nGNg"
            "ZnzMOIGBlYGFgTBgFEDi2AMBkFJ4wMCs8N+CAUgynEBTr8DAAAAFkwX0AAAB9AAAAPoAAAK8AAB4nGNg"
            "YGBiYGBgBmIRIMkIplkYNIA0G5BmBMoqPGD4/x/IB9P/HyowgVUBAQCV+ghqAAAAAAAAAAAAAA0AAQAy"
            "AAACigK8AAMAADMhESEyAlj9qAK8AAAAeJx1zc0KglAQhuHXn4wo2hTR0l1tlOoWgsCduHAZiBxEEIUj"
            "3kl0EV1lE83CAs/mPPPNDAOseOLweY4Ytctcqq89tmzU/sgzlhzUgeQXmXT8hSR7bmqXNXe1x5FG7Y88"
            "Y8dDHUj+SovKXDtrkrJrM1MNTWHHUahZbmxfS3mOT+N2pG1SCioMVzqs/AmlqCUTVwxyv2B6Kvyby6Wy"
            "9NTaPRNzmtyOfrffwDc5lnicY2BiwA+YQZiRib00L9PVwMAAAAs4AjQAAAA=");
    }
    if (format == "woff2") {
        return pagecore::base64_decode(
            "d09GMgABAAAAAAGkAAoAAAAAA4wAAAFbAAEAAAAAAAAAAAAAAAAAAAAAAAAAAAAABmAAPAocOAE2AiQD"
            "DAsIAAQgBYJoBzAb8QIongPu7FRE3BkPi04Wyot1sf7yEZLMElTL1rO7d09M8hW8isLgEBpFVhQ2Cov+"
            "wgiUQlhciOVTgmibpym9dEFDFsPaWFiX8YgwfPgx2IDTFCIIjGc5eSFx/nY+9xOO3nmUdql2UZdIGOBv"
            "H51/wSey8+Es8QGNbOeLhl1OcUQNPkiCxcWVvVGOu1ilUB5ToNUzNK/l5n9ILgHSFBMy5loCc4oWakVI"
            "mtf+D3n8/41EAgAZ1AgUVIACIMroZDXRXxdAIDhdTlebk74S8jM87g9Gt/qkswRZAmGOyZr2UgQAQI4X"
            "8AX2v4GsCJAAADDmnABhDAGShjEBsjmzAhQHKqNWrmtz+rIow8KSNYJkwgZkLQdqgcqCQay1XJxZaOb6"
            "fGY/VY0CL2wv1V1td0ssf0NOw6Qa49ThYbaLmn6vAZ0SqW/KfRZyrKet26HLMyJzeb80NQ736qV0AQAA");
    }
    throw std::runtime_error("unknown font fixture format");
}

#if defined(PAGECORE_ENABLE_RENDERING)
std::string jpeg_body()
{
    return pagecore::base64_decode(
        "/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEB"
        "AQEBAQEBAQEBAQEBAQEBAQH/2wBDAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEB"
        "AQEBAQEBAQEBAQEBAQH/wAARCAAEAAQDASIAAhEBAxEB/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAA"
        "AgEDAwIEAwUFBAQAAAF9AQIDAAQRBRIhMUEGE1FhByJxFDKBkaEII0KxwRVS0fAkM2JyggkKFhcYGRolJicoKSo0NTY3ODk6"
        "Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqDhIWGh4iJipKTlJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5usLDxMXG"
        "x8jJytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/8QAHwEAAwEBAQEBAQEBAQAAAAAAAAECAwQFBgcICQoL/8QAtREA"
        "AgECBAQDBAcFBAQAAQJ3AAECAxEEBSExBhJBUQdhcRMiMoEIFEKRobHBCSMzUvAVYnLRChYkNOEl8RcYGRomJygpKjU2Nzg5"
        "OkNERUZHSElKU1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6goOEhYaHiImKkpOUlZaXmJmaoqOkpaanqKmqsrO0tba3uLm6wsPE"
        "xcbHyMnK0tPU1dbX2Nna4uPk5ebn6Onq8vP09fb3+Pn6/9oADAMBAAIRAxEAPwD8t6KKK/ic/rg//9k=");
}

std::string jpeg_header_with_dimensions(std::uint16_t width, std::uint16_t height)
{
    const unsigned char bytes[] = {
        0xff, 0xd8,
        0xff, 0xc0,
        0x00, 0x11,
        0x08,
        static_cast<unsigned char>((height >> 8) & 0xff),
        static_cast<unsigned char>(height & 0xff),
        static_cast<unsigned char>((width >> 8) & 0xff),
        static_cast<unsigned char>(width & 0xff),
        0x03,
        0x01, 0x22, 0x00,
        0x02, 0x11, 0x01,
        0x03, 0x11, 0x01,
        0xff, 0xd9,
    };
    return std::string(reinterpret_cast<const char*>(bytes), sizeof(bytes));
}

#endif

#if defined(PAGECORE_ENABLE_RENDERING) && defined(PAGECORE_ENABLE_WEBP) && PAGECORE_ENABLE_WEBP
std::string webp_body(pagecore::Color color, int width = 4, int height = 4)
{
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) * height * 4);
    for (std::size_t offset = 0; offset + 3 < rgba.size(); offset += 4) {
        rgba[offset] = color.r;
        rgba[offset + 1] = color.g;
        rgba[offset + 2] = color.b;
        rgba[offset + 3] = color.a;
    }

    std::uint8_t* encoded = nullptr;
    const std::size_t encoded_size = WebPEncodeLosslessRGBA(
        rgba.data(),
        width,
        height,
        width * 4,
        &encoded);
    require(encoded_size > 0, "WebP should encode test image");
    const std::string body(reinterpret_cast<const char*>(encoded), encoded_size);
    WebPFree(encoded);
    return body;
}
#endif

std::string gif_body()
{
    const unsigned char bytes[] = {
        0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x01, 0x00, 0x01, 0x00, 0xf0, 0x00,
        0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x01, 0x00, 0x00, 0x02, 0x02, 0x44, 0x01, 0x00, 0x3b,
    };
    return std::string(reinterpret_cast<const char*>(bytes), sizeof(bytes));
}

// A hand-crafted 2x2, 16-bit-per-channel RGBA PNG (color type 6, bit depth
// 16), solid color 0xea7527ff at full 16-bit precision (0xea75/0xffff,
// 0x7527/0xffff, 0x2710/0xffff, opaque). Cairo's PNG loader preserves such
// >8-bit sources as floating-point surfaces (CAIRO_FORMAT_RGBA128F) instead
// of downsampling to ARGB32, which the decoder must also handle correctly.
std::string png_body_16bit()
{
    const unsigned char bytes[] = {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
        0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02,
        0x10, 0x06, 0x00, 0x00, 0x00, 0x22, 0x26, 0xd1, 0x67, 0x00, 0x00, 0x00,
        0x16, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0x78, 0x95, 0x50, 0x6a,
        0xa0, 0x2e, 0xf0, 0xff, 0x3f, 0x8c, 0x66, 0x40, 0x17, 0x00, 0x00, 0x14,
        0xd5, 0x10, 0x91, 0xa0, 0x80, 0xa7, 0x40, 0x00, 0x00, 0x00, 0x00, 0x49,
        0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
    };
    return std::string(reinterpret_cast<const char*>(bytes), sizeof(bytes));
}

std::string svg_body(pagecore::Color color = pagecore::Color{240, 20, 30, 255})
{
    return "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"4\" height=\"3\" viewBox=\"0 0 4 3\">"
           "<rect width=\"4\" height=\"3\" fill=\"rgb("
        + std::to_string(color.r) + "," + std::to_string(color.g) + "," + std::to_string(color.b) + ")\"/>"
          "</svg>";
}

bool color_close(const std::vector<std::uint8_t>& rgba, pagecore::Color color, int tolerance)
{
    if (rgba.size() < 4) {
        return false;
    }

    return std::abs(static_cast<int>(rgba[0]) - color.r) <= tolerance
        && std::abs(static_cast<int>(rgba[1]) - color.g) <= tolerance
        && std::abs(static_cast<int>(rgba[2]) - color.b) <= tolerance
        && std::abs(static_cast<int>(rgba[3]) - color.a) <= tolerance;
}

bool pixel_close(const std::vector<std::uint8_t>& rgba, int width, int x, int y, pagecore::Color color, int tolerance)
{
    const std::size_t offset = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 4;
    if (offset + 4 > rgba.size()) {
        return false;
    }

    return std::abs(static_cast<int>(rgba[offset]) - color.r) <= tolerance
        && std::abs(static_cast<int>(rgba[offset + 1]) - color.g) <= tolerance
        && std::abs(static_cast<int>(rgba[offset + 2]) - color.b) <= tolerance
        && std::abs(static_cast<int>(rgba[offset + 3]) - color.a) <= tolerance;
}

std::uint32_t read_be32(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    require(offset + 4 <= bytes.size(), "unexpected end of binary data");
    return (static_cast<std::uint32_t>(bytes[offset]) << 24)
        | (static_cast<std::uint32_t>(bytes[offset + 1]) << 16)
        | (static_cast<std::uint32_t>(bytes[offset + 2]) << 8)
        | static_cast<std::uint32_t>(bytes[offset + 3]);
}

void require_resource_error(
    pagecore::ResourceErrorCode expected,
    const std::function<void()>& action,
    const std::string& message)
{
    try {
        action();
    } catch (const pagecore::ResourceError& error) {
        require(error.code() == expected, message + ": wrong error code");
        return;
    }

    throw std::runtime_error(message + ": expected ResourceError");
}

void require_runtime_error_contains(
    const std::function<void()>& action,
    std::string_view expected,
    const std::string& message)
{
    try {
        action();
    } catch (const std::runtime_error& error) {
        const std::string text = error.what();
        require(text.find(expected) != std::string::npos, message + ": wrong error message: " + text);
        return;
    }

    throw std::runtime_error(message + ": expected runtime_error");
}

void test_inline_script_mutates_lexbor_dom()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<!doctype html>
<html>
  <body>
    <div id="app"></div>
    <script>
      const app = document.getElementById('app');
      app.textContent = 'Hello';
      app.classList.add('ready');
      const span = document.createElement('span');
      span.setAttribute('data-x', '1');
      span.textContent = '!';
      app.appendChild(span);
    </script>
  </body>
</html>
)HTML");

    auto text = page.text_content("#app.ready");
    require(text && *text == "Hello!", "script mutation should update textContent");
    auto span = page.outer_html("#app.ready span[data-x='1']");
    require(span && span->find("<span data-x=\"1\">!</span>") != std::string::npos,
            "script-created span should be queryable and serializable");
}

void test_timers_and_events()
{
    std::vector<std::pair<std::string, std::string>> console_logs;
    pagecore::LoadOptions options;
    options.console_log = [&](std::string_view severity, std::string_view message) {
        console_logs.emplace_back(severity, message);
    };

    pagecore::Page page(options);
    page.load_html(R"HTML(
<html><body>
  <button id="b"></button>
  <script>
    const b = document.getElementById('b');
    b.addEventListener('done', () => { throw new Error('listener boom'); });
    b.addEventListener('done', (event) => b.setAttribute('data-hit', event.detail));
    b.ondone = () => { throw new Error('handler boom'); };
    setTimeout(() => b.dispatchEvent(new CustomEvent('done', { detail: 'ok' })), 10);
    setTimeout(() => { throw new Error('timer boom'); }, 11);
    setTimeout(() => b.setAttribute('data-after-timer-error', 'ok'), 12);
  </script>
</body></html>
)HTML");

    auto button = page.outer_html("#b[data-hit='ok']");
    require(button.has_value(), "setTimeout and CustomEvent should mutate DOM even when another listener throws");
    require(page.outer_html("#b[data-after-timer-error='ok']").has_value(),
            "throwing timer callbacks should not abort later timers");
    require(console_logs.size() == 3, "throwing event handlers and timer callbacks should be reported through the console log callback");
    require(console_logs[0].first == "error", "listener exception should be reported as console error");
    require(!console_logs[0].second.empty(), "listener exception log should include stack details");
    require(console_logs[1].first == "error", "event handler exception should be reported as console error");
    require(!console_logs[1].second.empty(), "event handler exception log should include stack details");
    require(console_logs[2].first == "error", "timer callback exception should be reported as console error");
    require(!console_logs[2].second.empty(), "timer callback exception log should include stack details");
}

void test_dom_shim_spec_regressions()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html><body>
<form id="f"><button id="btn" name="action" value="save">Go</button></form>
<script>
  // normalize(): merge adjacent text nodes, drop empties.
  var d = document.createElement('div');
  d.appendChild(document.createTextNode('foo'));
  d.appendChild(document.createTextNode(''));
  d.appendChild(document.createTextNode('bar'));
  document.body.appendChild(d);
  d.normalize();
  document.body.setAttribute('data-normalize', d.childNodes.length + ':' + d.textContent);

  // FormData must include the submitter button's value.
  var fd = new FormData(document.getElementById('f'), document.getElementById('btn'));
  document.body.setAttribute('data-fd', String(fd.get('action')));

  // Events on a non-Node EventTarget must not propagate to window.
  var reached = false;
  window.addEventListener('custom-et', function(){ reached = true; });
  var et = new EventTarget();
  et.dispatchEvent(new CustomEvent('custom-et', { bubbles: true }));
  document.body.setAttribute('data-et', reached ? 'leaked' : 'isolated');

  // replaceChild must detach a fragment-parented incoming node from its fragment.
  var frag = document.createDocumentFragment();
  var incoming = document.createElement('span');
  frag.appendChild(incoming);
  var box = document.createElement('div');
  var old = document.createElement('p');
  box.appendChild(old);
  document.body.appendChild(box);
  box.replaceChild(incoming, old);
  document.body.setAttribute('data-frag', String(frag.childNodes.length));

  // elementFromPoint must exist (baseline API libraries call unguarded) and
  // return null or a real Element rather than throw or hand back garbage.
  // (Real hit-testing means (1,1) now legitimately resolves to whatever
  // element is actually painted there -- see test_hit_testing for coverage
  // of the hit-testing behavior itself.)
  var efpResult = (typeof document.elementFromPoint === 'function') ? document.elementFromPoint(1, 1) : undefined;
  var efp = typeof document.elementFromPoint !== 'function'
    ? 'missing'
    : (efpResult === null || efpResult instanceof Element) ? 'ok' : 'wrong';
  document.body.setAttribute('data-efp', efp);
</script>
</body></html>
)HTML");

    require(page.outer_html("body[data-normalize='1:foobar']").has_value(),
            "normalize() must merge adjacent text nodes and drop empty ones");
    require(page.outer_html("body[data-fd='save']").has_value(),
            "FormData must include the submitter button's value (button.value reflects its attribute)");
    require(page.outer_html("body[data-et='isolated']").has_value(),
            "an event on a non-Node EventTarget must not propagate to window");
    require(page.outer_html("body[data-frag='0']").has_value(),
            "replaceChild must detach a fragment-parented incoming node from its fragment");
    require(page.outer_html("body[data-efp='ok']").has_value(),
            "document.elementFromPoint must be present and return null or a real Element (baseline API called unguarded)");
}

void test_js_console_log_callback()
{
    std::vector<std::pair<std::string, std::string>> console_logs;
    pagecore::LoadOptions options;
    options.console_log = [&](std::string_view severity, std::string_view message) {
        console_logs.emplace_back(severity, message);
    };

    pagecore::Page page(options);
    page.load_html(R"HTML(
<html><body>
  <script>
    console.info('hello', 42);
    console.warn();
  </script>
</body></html>
)HTML");

    require(console_logs.size() == 2, "console calls should be routed to the configured log callback");
    require(console_logs[0].first == "info", "console.info should preserve severity");
    require(console_logs[0].second == "hello 42", "console callback should join multiple arguments");
    require(console_logs[1].first == "warn", "console.warn should preserve severity");
    require(console_logs[1].second.empty(), "console callback should allow empty messages");
}

void test_unhandled_promise_rejection_is_logged()
{
    // Regression: an unhandled rejection (here from a top-level Promise.reject) used
    // to vanish silently for lack of a host promise-rejection tracker, unlike a
    // classic-script throw which is logged.
    std::vector<std::pair<std::string, std::string>> console_logs;
    pagecore::LoadOptions options;
    options.console_log = [&](std::string_view severity, std::string_view message) {
        console_logs.emplace_back(severity, message);
    };

    pagecore::Page page(options);
    page.load_html(R"HTML(
<html><body>
  <script>
    Promise.reject(new Error('boom-rejection'));
  </script>
</body></html>
)HTML");
    page.run_until_idle();

    const bool found = std::any_of(console_logs.begin(), console_logs.end(),
        [](const auto& entry) {
            return entry.first == "error" && entry.second.find("boom-rejection") != std::string::npos;
        });
    require(found, "an unhandled promise rejection must be surfaced as a console error");
}

void test_same_origin_policy_fails_closed_without_document_origin()
{
    // Regression: SameOriginOnly used to fail OPEN when the document had no network
    // origin (empty/data:/file: base) — network_origin("") is empty, so the guard
    // silently degraded to Allow and let a script fetch any cross-origin host.
    auto loader = std::make_shared<RecordingResourceLoader>();
    loader->add("https://other.test/x", "x", "text/plain");

    pagecore::LoadOptions options;
    options.js_resource_load_policy = pagecore::JsResourceLoadPolicy::SameOriginOnly;

    pagecore::Page page(options);
    page.set_resource_loader(loader);
    page.load_html(R"HTML(
<html><body>
  <script>
    fetch('https://other.test/x')
      .then(() => document.body.setAttribute('data-fetch','ok'))
      .catch(() => document.body.setAttribute('data-fetch','blocked'));
  </script>
</body></html>
)HTML");
    page.run_until_idle();

    require(!page.outer_html("body[data-fetch='ok']").has_value(),
            "SameOriginOnly must not allow a cross-origin fetch from a document with no "
            "network origin (fail closed)");
    require(find_request(*loader, "https://other.test/x") == nullptr,
            "the blocked cross-origin request must never reach the resource loader");
}

void test_inner_html_fragment_parsing()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html><body>
  <section id="target"></section>
  <script>
    document.getElementById('target').innerHTML = '<p class="x">A</p><p>B</p>';
  </script>
</body></html>
)HTML");

    auto html = page.outer_html("#target");
    require(html && html->find("<p class=\"x\">A</p><p>B</p>") != std::string::npos,
            "innerHTML should parse an HTML fragment through Lexbor");
}

void test_tree_operations_and_clone()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html><body>
  <ul id="list"><li id="a">A</li><li id="c">C</li></ul>
  <script>
    const list = document.getElementById('list');
    const c = document.getElementById('c');

    const b = document.createElement('li');
    b.id = 'b';
    b.textContent = 'B';
    list.insertBefore(b, c);

    const replacement = document.createElement('li');
    replacement.id = 'x';
    replacement.textContent = 'X';
    const old = list.replaceChild(replacement, document.getElementById('a'));

    const clone = replacement.cloneNode(true);
    clone.id = 'x2';
    list.appendChild(clone);
    c.remove();

    list.setAttribute('data-returned', old.id);
    // children is an HTMLCollection, so it has no Array methods.
    list.setAttribute('data-order', [...list.children].map((node) => node.textContent).join(''));
    list.setAttribute('data-contains', String(list.contains(clone)));
    list.setAttribute('data-connected', String(clone.isConnected));
  </script>
</body></html>
)HTML");

    auto list = page.outer_html("#list[data-returned='a'][data-order='XBX'][data-contains='true'][data-connected='true']");
    require(list && list->find("<li id=\"x2\">X</li>") != std::string::npos,
            "insertBefore, replaceChild, cloneNode and remove should mutate Lexbor DOM");
}

void test_dataset_attributes_and_cached_facades()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html><body>
  <div id="target" class="a" data-user-id="42" style="color: red"></div>
  <script>
    const el = document.getElementById('target');
    const sameClassList = el.classList === el.classList;
    const sameStyle = el.style === el.style;
    const sameAttributes = el.attributes === el.attributes;

    el.classList.add('b');
    el.style.backgroundColor = 'blue';
    el.dataset.userId = '7';
    el.dataset.newFlag = 'yes';
    el.setAttribute('title', 'ok');

    const attrByIndex = el.attributes[0] && el.attributes[0].name ? 'indexed' : 'missing';
    const datasetKeys = Object.keys(el.dataset).sort().join(',');
    el.setAttribute('data-check', [
      el.dataset.userId,
      el.dataset.newFlag,
      el.attributes.getNamedItem('title').value,
      attrByIndex,
      datasetKeys,
      sameClassList,
      sameStyle,
      sameAttributes
    ].join('|'));
  </script>
</body></html>
)HTML");

    auto div = page.outer_html("#target.a.b[data-user-id='7'][data-new-flag='yes'][title='ok']");
    require(div && div->find("background-color: blue;") != std::string::npos,
            "dataset, attributes, classList and style facades should reflect Lexbor attributes");
    require(div->find("data-check=\"7|yes|ok|indexed|newFlag,userId|true|true|true\"") != std::string::npos,
            "DOM facade objects should be cached and expose attribute snapshots");
}

void test_inner_html_invalidates_stale_wrappers()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html><body>
  <div id="root"><span id="old">old</span></div>
  <script>
    const root = document.getElementById('root');
    const old = document.getElementById('old');
    root.innerHTML = '<span id="fresh">fresh</span>';

    let stale = 'usable';
    try {
      old.textContent = 'bad';
    } catch (error) {
      stale = 'invalid';
    }

    root.setAttribute('data-stale', stale);
    root.setAttribute('data-fresh', root.firstElementChild.textContent);
  </script>
</body></html>
)HTML");

    auto root = page.outer_html("#root[data-stale='invalid'][data-fresh='fresh']");
    require(root && root->find("bad") == std::string::npos,
            "innerHTML should invalidate stale JS wrappers for destroyed Lexbor nodes");
}

void test_wrapper_cache_prunes_only_on_forget()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html><body><div id="host"><span id="a">A</span><span id="b">B</span></div>
<script>
  const host = document.getElementById('host');
  const a = document.getElementById('a');

  // removeChild detaches but does NOT forget the node: its wrapper stays usable
  // and must not be pruned (this is the common path kept O(1) by forget-gating).
  host.removeChild(a);
  window.__removedUsable = (a.textContent === 'A' && a.isConnected === false);

  // innerHTML replacement forgets host's current subtree (the still-attached b),
  // so b's wrapper must become invalid.
  const b = document.getElementById('b');
  host.innerHTML = '<span id="c">C</span>';
  let bInvalid = false;
  try { b.textContent = 'x'; } catch (error) { bInvalid = true; }
  window.__forgottenInvalid = bInvalid;

  // The fresh subtree is wrapped correctly after the forget-triggered prune.
  window.__freshOk = (host.firstElementChild.textContent === 'C');
</script></body></html>
)HTML");

    require(page.eval("window.__removedUsable") == "true",
            "a detached-but-not-forgotten node keeps a usable wrapper");
    require(page.eval("window.__forgottenInvalid") == "true",
            "innerHTML replacement invalidates wrappers for forgotten nodes");
    require(page.eval("window.__freshOk") == "true",
            "the replacement subtree is wrapped correctly after the prune");
}

void test_timer_wait_budget()
{
    // Real-time semantics: run_until_idle waits (wall-clock) up to wait_time
    // for pending timers; a timer far beyond the budget stays pending.
    {
        pagecore::LoadOptions options;
        options.wait_until = pagecore::WaitUntil::Load;
        options.wait_time = std::chrono::milliseconds(0);

        pagecore::Page page(options);
        page.load_html(R"HTML(
<html><body>
  <div id="timer"></div>
  <script>
    setTimeout(() => document.getElementById('timer').setAttribute('data-fired', 'yes'), 5000);
  </script>
</body></html>
)HTML");

        page.run_until_idle();
        require(!page.outer_html("#timer[data-fired='yes']").has_value(),
                "a zero wait budget must not wait for a delayed timer");
    }

    {
        pagecore::LoadOptions options;
        options.wait_until = pagecore::WaitUntil::Load;
        options.wait_time = std::chrono::milliseconds(200);

        pagecore::Page page(options);
        page.load_html(R"HTML(
<html><body>
  <div id="timer"></div>
  <script>
    setTimeout(() => document.getElementById('timer').setAttribute('data-fired', 'yes'), 20);
    setTimeout(() => document.getElementById('timer').setAttribute('data-late', 'yes'), 10000);
  </script>
</body></html>
)HTML");

        require(!page.outer_html("#timer[data-fired='yes']").has_value(),
                "wait-until=load should not run timer callbacks during load");

        page.run_until_idle();
        require(page.outer_html("#timer[data-fired='yes']").has_value(),
                "a 20ms timer should fire within a 200ms wait budget");
        require(!page.outer_html("#timer[data-late='yes']").has_value(),
                "a 10s timer must stay pending after the wait budget is exhausted");
    }
}

void test_zero_wait_runs_ready_tasks_but_keeps_timers_pending()
{
    // wait_time=0 executes tasks that are ALREADY queued (0-delay) without
    // blocking, but never waits for delayed timers.
    pagecore::LoadOptions options;
    options.wait_until = pagecore::WaitUntil::Load;
    options.wait_time = std::chrono::milliseconds(0);

    pagecore::Page page(options);
    page.load_html(R"HTML(
<html><body>
  <div id="timer" data-sync="no"></div>
  <script>
    document.getElementById('timer').setAttribute('data-sync', 'yes');
    setTimeout(() => document.getElementById('timer').setAttribute('data-fired', 'yes'), 0);
    setTimeout(() => document.getElementById('timer').setAttribute('data-late', 'yes'), 100);
    // Regression: a finite delay far beyond uint64 range must clamp
    // (browser-style INT32_MAX ms), not hit undefined behavior in the
    // double->uint64 cast. (Infinity/NaN normalize to 0 in the shim and
    // never reach the cast.)
    setTimeout(() => document.getElementById('timer').setAttribute('data-huge', 'yes'), 1e30);
  </script>
</body></html>
)HTML");

    require(page.outer_html("#timer[data-sync='yes']").has_value(),
            "wait_time=0 must still execute synchronous scripts");
    require(!page.outer_html("#timer[data-fired='yes']").has_value(),
            "wait_time=0 must not run zero-delay timer callbacks during load");

    page.run_until_idle();
    require(page.outer_html("#timer[data-fired='yes']").has_value(),
            "wait_time=0 run_until_idle must execute already-ready zero-delay tasks");
    require(!page.outer_html("#timer[data-late='yes']").has_value(),
            "wait_time=0 run_until_idle must keep delayed timers pending");
    require(!page.outer_html("#timer[data-huge='yes']").has_value(),
            "an absurdly large timer delay must clamp and stay pending, not misfire");
}

void test_run_until_idle_logs_throwing_task_hook()
{
    std::vector<std::pair<std::string, std::string>> console_logs;
    pagecore::LoadOptions options;
    options.wait_until = pagecore::WaitUntil::Load;
    options.wait_time = std::chrono::milliseconds(100);
    options.console_log = [&](std::string_view severity, std::string_view message) {
        console_logs.emplace_back(severity, message);
    };

    pagecore::Page page(options);
    page.load_html("<html><body></body></html>");
    page.eval(R"JS(
      const originalRunTask = window.__pagecore_run_task;
      let boomed = false;
      window.__pagecore_run_task = (id) => {
        if (!boomed) {
          boomed = true;
          throw new Error('task hook boom');
        }
        return originalRunTask(id);
      };
      setTimeout(() => document.body.setAttribute('data-timer', 'ran'), 0);
      setTimeout(() => document.body.setAttribute('data-timer-late', 'ran'), 1);
    )JS");

    bool threw = false;
    try {
        page.run_until_idle();
    } catch (...) {
        threw = true;
    }

    require(!threw, "run_until_idle should log a throwing task hook instead of propagating");
    require(
        std::any_of(console_logs.begin(), console_logs.end(), [](const auto& entry) {
            return entry.first == "error" && entry.second.find("task hook boom") != std::string::npos;
        }),
        "run_until_idle should report task hook exceptions through console_log");
    require(
        page.outer_html("body[data-timer-late='ran']").has_value(),
        "the event loop should keep running tasks after a throwing task hook");
}

void test_event_loop_ordering_contract()
{
    auto loader = std::make_shared<RecordingResourceLoader>();
    loader->add("https://example.test/fetch.json", R"JSON({"value":"fetch"})JSON", "application/json");
    loader->add("https://example.test/xhr.json", R"JSON({"value":"xhr"})JSON", "application/json");

    std::vector<std::pair<std::string, std::string>> console_logs;
    pagecore::LoadOptions options;
    options.wait_until = pagecore::WaitUntil::Ready;
    options.wait_time = std::chrono::milliseconds(50);
    options.stable_window = std::chrono::milliseconds(5);
    options.console_log = [&](std::string_view severity, std::string_view message) {
        console_logs.emplace_back(severity, message);
    };

    pagecore::Page page(options);
    page.set_resource_loader(loader);
    page.load_html(R"HTML(
<html><body>
  <script>
    window.__eventLoopOrder = [];
    const push = (value) => window.__eventLoopOrder.push(value);

    setTimeout(() => push('timer-before-sync-mutation'), 0);
    Promise.resolve().then(() => push('promise-before-first-task'));

    const observer = new MutationObserver(() => {
      push('observer');
      Promise.resolve().then(() => push('observer-promise'));
      throw new Error('observer boom');
    });
    observer.observe(document.body, { attributes: true });
    document.body.setAttribute('data-sync-mutation', '1');
    setTimeout(() => push('timer-after-sync-mutation'), 0);

    fetch('/fetch.json')
      .then((response) => response.json())
      .then((data) => push(data.value));

    const xhr = new XMLHttpRequest();
    xhr.open('GET', '/xhr.json');
    xhr.onload = () => push(JSON.parse(xhr.responseText).value);
    xhr.send();

    setTimeout(() => { throw new Error('timer boom'); }, 0);
    setTimeout(() => {
      push('timer-after-error');
      observer.disconnect();
      document.body.setAttribute('data-event-loop-order', window.__eventLoopOrder.join(','));
    }, 0);
  </script>
</body></html>
)HTML", "https://example.test/index.html");

    require(
        page.eval("window.__eventLoopOrder.join(',')")
            == "promise-before-first-task,observer,observer-promise,timer-before-sync-mutation,timer-after-sync-mutation,fetch,xhr,timer-after-error",
        "event loop should run checkpoint before tasks, deliver MutationObserver before next task, and treat fetch/XHR as tasks");
    require(
        page.outer_html("body[data-event-loop-order]").has_value(),
        "later timer task should run after throwing observer/timer callbacks");
    require(console_logs.size() >= 2, "throwing observer and timer callbacks should be logged");
    require(
        has_request_kind(*loader, "https://example.test/fetch.json", pagecore::ResourceKind::Other)
            && has_request_kind(*loader, "https://example.test/xhr.json", pagecore::ResourceKind::Other),
        "fetch and XHR event-loop tasks should load through ResourceLoader");
}

void test_browser_like_web_api_shims()
{
    pagecore::LoadOptions options;
    options.user_agent = "shim-test-agent";

    pagecore::Page page(options);
    page.load_html(R"HTML(
<html>
  <head><title>Old</title></head>
  <body>
    <script>
      const checks = [];
      checks.push(location.href === 'https://example.test/path/index.html');
      checks.push(document.baseURI === 'https://example.test/path/index.html');
      checks.push(document.referrer === '');
      checks.push(document.referrer.indexOf(location.origin) === -1);
      checks.push(navigator.userAgent === 'shim-test-agent');
      checks.push(window.window === window);
      checks.push(self === window);
      checks.push(top === window);
      checks.push(parent === window);
      checks.push(typeof Window === 'function');
      if (typeof Window === 'function') {
        checks.push(window instanceof Window);
        checks.push(Window.prototype instanceof EventTarget);
        checks.push(Object.prototype.toString.call(window) === '[object Window]');
        checks.push(!Object.prototype.hasOwnProperty.call(window, 'addEventListener'));
      } else {
        checks.push(false, false, false, false);
      }
      const capturedWindowAddEventListener = window.addEventListener;
      const originalEventTargetAddEventListener = EventTarget.prototype.addEventListener;
      let patchedAddEventListenerCalled = false;
      EventTarget.prototype.addEventListener = function(...args) {
        patchedAddEventListenerCalled = true;
        return originalEventTargetAddEventListener.apply(this, args);
      };
      capturedWindowAddEventListener.call(window, 'pagecore-native-listener-check', () => {
        document.body.setAttribute('data-captured-window-listener', 'ok');
      });
      window.dispatchEvent(new Event('pagecore-native-listener-check'));
      EventTarget.prototype.addEventListener = originalEventTargetAddEventListener;
      checks.push(
        !patchedAddEventListenerCalled &&
        document.body.getAttribute('data-captured-window-listener') === 'ok');
      checks.push(navigator.javaEnabled() === false);
      checks.push(screen.width === window.innerWidth);
      checks.push(screen.height === window.innerHeight);
      checks.push(screen.colorDepth === 24);
      checks.push(btoa('abc') === 'YWJj');
      checks.push(atob('YWJj') === 'abc');
      checks.push(btoa('\xff') === '/w==');
      checks.push(atob('/w==').charCodeAt(0) === 255);
      let invalidBase64Input = false;
      try {
        btoa('\u0100');
      } catch (error) {
        invalidBase64Input = error && error.name === 'InvalidCharacterError';
      }
      checks.push(invalidBase64Input);

      const iframe = document.createElement('iframe');
      iframe.src = 'https://frame.test/start';
      document.body.appendChild(iframe);
      let frameMessage = false;
      iframe.contentWindow.addEventListener('message', (event) => {
        frameMessage = event.data === 'ping' &&
          event.origin === location.origin &&
          event.source === window;
      });
      iframe.contentWindow.postMessage('ping', '*');
      checks.push(iframe.contentWindow === iframe.contentWindow);
      checks.push(iframe.contentDocument === iframe.contentWindow.document);
      checks.push(iframe.contentDocument.defaultView === iframe.contentWindow);
      checks.push(typeof Window === 'function' && iframe.contentWindow instanceof Window);
      checks.push(typeof Window === 'function' && iframe.contentWindow.Window === Window);
      checks.push(iframe.contentWindow.parent === window);
      checks.push(iframe.contentWindow.frameElement === iframe);
      checks.push(iframe.contentWindow.location.href === 'https://frame.test/start');
      checks.push(frameMessage);

      const url = new URL('../asset.png?x=1#h', location.href);
      checks.push(url.href === 'https://example.test/asset.png?x=1#h');
      checks.push(URL.canParse('/ok', location.href));

      const params = new URLSearchParams('a=1&a=2&b=hello+world');
      checks.push(params.get('a') === '1');
      checks.push(params.getAll('a').join(',') === '1,2');
      checks.push(params.get('b') === 'hello world');

      localStorage.setItem('k', 'v');
      sessionStorage.setItem('s', '1');
      checks.push(localStorage.getItem('k') === 'v');
      checks.push(sessionStorage.length === 1);
      checks.push(matchMedia('(min-width: 1px)').media === '(min-width: 1px)');
      checks.push(!('Worker' in window));
      checks.push(!('SharedWorker' in window));
      checks.push(!('serviceWorker' in navigator));
      checks.push(!('IntersectionObserver' in window));
      checks.push(!('ResizeObserver' in window));
      checks.push(!('PerformanceObserver' in window));

      document.title = 'Shim Title';
      checks.push(document.title === 'Shim Title');
      document.body.setAttribute('data-script-ready', document.readyState);

      document.addEventListener('DOMContentLoaded', () => {
        document.body.setAttribute('data-dcl', document.readyState);
      });
      window.addEventListener('load', () => {
        document.body.setAttribute('data-load', document.readyState);
      });

      document.body.setAttribute('data-api', checks.every(Boolean) ? 'ok' : 'bad');
    </script>
  </body>
</html>
)HTML", "https://example.test/path/index.html");

    auto body = page.outer_html("body[data-api='ok'][data-script-ready='loading'][data-dcl='interactive'][data-load='complete']");
    require(body.has_value(), "browser-like global APIs, base URL and lifecycle events should be available");

    auto title = page.text_content("title");
    require(title && *title == "Shim Title", "document.title should update the title element");
}

void test_media_query_list_event_target_shims()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html><body>
  <script>
    const checks = [];
    const mql = matchMedia('(min-width: 1px)');

    checks.push(typeof MediaQueryList === 'function');
    checks.push(typeof MediaQueryListEvent === 'function');
    checks.push(mql instanceof MediaQueryList);
    checks.push(mql instanceof EventTarget);
    checks.push(Object.prototype.toString.call(mql) === '[object MediaQueryList]');
    checks.push(mql.media === '(min-width: 1px)');
    checks.push(mql.matches === true);
    checks.push(matchMedia('(max-width: 10px)').matches === false);
    checks.push(matchMedia('not print').matches === true);
    checks.push(matchMedia('').media === '');
    checks.push(matchMedia('').matches === true);
    checks.push(matchMedia('::').media === 'not all');
    checks.push(matchMedia('::').matches === false);
    checks.push(matchMedia('all and (min-width: 1px)').media === '(min-width: 1px)');
    checks.push(matchMedia('all and (min-width: 1px)').matches === true);

    const event = new MediaQueryListEvent('change', {
      media: mql.media,
      matches: true,
      bubbles: true,
      cancelable: true
    });
    checks.push(event instanceof MediaQueryListEvent);
    checks.push(event instanceof Event);
    checks.push(Object.prototype.toString.call(event) === '[object MediaQueryListEvent]');
    checks.push(event.type === 'change');
    checks.push(event.media === mql.media);
    checks.push(event.matches === true);
    checks.push(event.bubbles === true);
    checks.push(event.cancelable === true);

    const seen = [];
    let legacyCount = 0;
    function listener(received) {
      seen.push(`event:${received === event}:${this === mql}:${received.matches}`);
    }
    const objectListener = {
      handleEvent(received) {
        seen.push(`object:${received.media}`);
      }
    };
    function legacyListener(received) {
      legacyCount += received instanceof MediaQueryListEvent ? 1 : 10;
    }

    mql.addEventListener('change', listener);
    mql.addEventListener('change', objectListener);
    mql.addEventListener('change', (received) => received.preventDefault(), { once: true });
    mql.addListener(legacyListener);
    mql.onchange = (received) => {
      seen.push(`onchange:${received.matches}`);
    };

    checks.push(mql.dispatchEvent(event) === false);
    checks.push(seen.includes('event:true:true:true'));
    checks.push(seen.includes('object:(min-width: 1px)'));
    checks.push(seen.includes('onchange:true'));
    checks.push(legacyCount === 1);

    mql.removeListener(legacyListener);
    mql.dispatchEvent(new MediaQueryListEvent('change', {
      media: mql.media,
      matches: false
    }));
    checks.push(legacyCount === 1);

    document.body.setAttribute('data-media-query-list', checks.every(Boolean) ? 'ok' : 'bad');
  </script>
</body></html>
)HTML", "https://example.test/index.html");

    require(
        page.outer_html("body[data-media-query-list='ok']").has_value(),
        "MediaQueryList and MediaQueryListEvent should expose minimal EventTarget-compatible browser APIs");
}

void test_navigator_user_agent_data()
{
    // Regression: navigator.userAgentData must exist and be well-formed. Sites
    // such as Google's OneGoogle bar unconditionally read it and reject with an
    // unhandled "Error: va" when getHighEntropyValues is missing, degrading the
    // page. The low-entropy hints are derived from the configured userAgent so
    // they stay consistent with navigator.userAgent.
    pagecore::LoadOptions options;
    options.user_agent =
        "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36";

    pagecore::Page page(options);
    page.load_html(R"HTML(
<html><body>
  <script>
    const checks = [];
    const uad = navigator.userAgentData;
    // The exact condition Google's OneGoogle bar tests before rejecting with va.
    const wouldReject = !uad
      || typeof uad.getHighEntropyValues !== 'function'
      || (uad.brands && typeof uad.brands.map !== 'function');
    checks.push(!wouldReject);
    checks.push(Array.isArray(uad.brands) && uad.brands.length > 0);
    checks.push(uad.brands.some((b) => b.brand === 'Google Chrome' && b.version === '120'));
    checks.push(typeof uad.mobile === 'boolean' && uad.mobile === false);
    checks.push(uad.platform === 'macOS');
    document.body.setAttribute('data-sync', checks.every(Boolean) ? 'ok' : 'bad');

    uad.getHighEntropyValues(['platform', 'platformVersion', 'architecture', 'uaFullVersion'])
      .then((values) => {
        const ok = values
          && values.platform === 'macOS'
          && typeof values.architecture === 'string'
          && typeof values.platformVersion === 'string'
          && values.uaFullVersion === '120.0.0.0';
        document.body.setAttribute('data-async', ok ? 'ok' : 'bad');
      })
      .catch(() => document.body.setAttribute('data-async', 'rejected'));
  </script>
</body></html>
)HTML", "https://example.test/index.html");

    require(page.outer_html("body[data-sync='ok'][data-async='ok']").has_value(),
            "navigator.userAgentData must be well-formed and getHighEntropyValues must resolve");
}

void test_event_constructor_ignores_prototype_accessors()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html><body>
  <script>
    Object.defineProperty(Event.prototype, 'target', {
      configurable: true,
      get() { return null; }
    });
    document.addEventListener('DOMContentLoaded', (event) => {
      document.body.setAttribute('data-event-target', event.target === document ? 'ok' : 'bad');
    });
  </script>
</body></html>
)HTML");

    require(
        page.outer_html("body[data-event-target='ok']").has_value(),
        "Event instances should own mutable dispatch fields even if Event.prototype has accessor shims");
}

void test_document_lifecycle_ignores_ready_state_overrides()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html><body>
  <script>
    Object.defineProperty(document, 'readyState', {
      configurable: true,
      get() { return 'loading'; }
    });
    document.addEventListener('DOMContentLoaded', () => {
      document.body.setAttribute('data-dcl', 'ok');
    });
    window.addEventListener('load', () => {
      document.body.setAttribute('data-load', 'ok');
    });
  </script>
</body></html>
)HTML");

    require(
        page.outer_html("body[data-dcl='ok'][data-load='ok']").has_value(),
        "lifecycle dispatch should use internal readyState even if page code shadows document.readyState");
}

void test_get_computed_style_reads_display_from_stylesheets()
{
    auto loader = std::make_shared<RecordingResourceLoader>();
    loader->add(
        "https://example.test/styles.css",
        ".wrapper { display: none; }",
        "text/css");

    pagecore::Page page;
    page.set_resource_loader(loader);
    page.load_html(R"HTML(
<html>
  <head><link rel="stylesheet" href="/styles.css"></head>
  <body>
    <div class="wrapper"></div>
    <script>
      const wrapper = document.querySelector('.wrapper');
      const before = getComputedStyle(wrapper).getPropertyValue('display');
      if (before === 'none') wrapper.style.display = 'block';
      const after = getComputedStyle(wrapper).display;
      document.body.setAttribute('data-computed-display',
        before === 'none' &&
        after === 'block' &&
        wrapper.getAttribute('style').indexOf('display: block') >= 0
          ? 'ok'
          : 'bad');
    </script>
  </body>
</html>
)HTML", "https://example.test/index.html");

    require(
        page.outer_html("body[data-computed-display='ok']").has_value(),
        "getComputedStyle should expose simple display rules from linked stylesheets");
    require(
        has_request_kind(*loader, "https://example.test/styles.css", pagecore::ResourceKind::Stylesheet),
        "getComputedStyle stylesheet lookup should request linked CSS as a stylesheet resource");
}

// Regression test: litehtml used to discard the entire contents of any
// unrecognized at-rule, including `@layer`, rather than just ignoring the
// (unimplemented) cascade-layer ordering semantics. Since Tailwind/PostCSS
// wrap effectively their whole generated stylesheet in `@layer` blocks, this
// silently dropped all layout-relevant CSS on real-world pages.
void test_layer_at_rule_rules_are_applied()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html>
  <head>
    <style>
      @layer base, components;
      @layer base {
        .box { display: none; }
      }
      @layer components {
        .box { color: rgb(0, 128, 128); }
      }
    </style>
  </head>
  <body>
    <div class="box"></div>
    <script>
      const box = document.querySelector('.box');
      const style = getComputedStyle(box);
      document.body.setAttribute('data-layer',
        style.display === 'none' && style.color === 'rgb(0, 128, 128)' ? 'ok' : 'bad');
    </script>
  </body>
</html>
)HTML");

    require(
        page.outer_html("body[data-layer='ok']").has_value(),
        "rules inside @layer blocks should still apply instead of being discarded");
}

void test_cssom_stylesheets_rules_declarations_and_cascade()
{
    auto loader = std::make_shared<RecordingResourceLoader>();
    loader->add(
        "https://example.test/base.css",
        ".target { color: red; display: none; }\n#target { color: blue; }",
        "text/css");

    pagecore::Page page;
    page.set_resource_loader(loader);
    page.load_html(R"HTML(
<html>
  <head>
    <link rel="stylesheet" href="/base.css">
    <style>.target { color: green !important; background-color: yellow; }</style>
  </head>
  <body>
    <div id="target" class="target"></div>
    <div id="inserted" class="inserted"></div>
    <script>
      const target = document.getElementById('target');
      const inserted = document.getElementById('inserted');
      const sheets = document.styleSheets;
      const linked = sheets[0];
      const inline = sheets.item(1);
      const firstRule = linked.cssRules[0];

      inline.insertRule('.inserted { display: inline; color: purple; }', inline.cssRules.length);
      const insertedDisplay = getComputedStyle(inserted).display;
      inline.deleteRule(inline.cssRules.length - 1);
      const insertedDisplayAfterDelete = getComputedStyle(inserted).display;

      target.style.setProperty('color', 'black');
      const authorImportant = getComputedStyle(target).color;
      target.style.setProperty('color', 'black', 'important');
      const inlineImportant = getComputedStyle(target).color;

      const checks = [
        sheets.length === 2,
        sheets.item(0) === linked,
        linked instanceof CSSStyleSheet,
        linked.ownerNode.tagName === 'LINK',
        linked.href === 'https://example.test/base.css',
        inline.ownerNode.tagName === 'STYLE',
        firstRule instanceof CSSStyleRule,
        firstRule.type === CSSRule.STYLE_RULE,
        firstRule.selectorText === '.target',
        firstRule.style instanceof CSSStyleDeclaration,
        firstRule.style.getPropertyValue('display') === 'none',
        inline.cssRules[0].style.getPropertyPriority('color') === 'important',
        insertedDisplay === 'inline',
        insertedDisplayAfterDelete === 'block',
        authorImportant === 'rgb(0, 128, 0)',
        inlineImportant === 'rgb(0, 0, 0)',
        target.style.getPropertyPriority('color') === 'important',
        target.style.item(0) === 'color',
        document.querySelector('style').textContent.indexOf('.target') >= 0
      ];
      document.body.setAttribute('data-cssom', checks.every(Boolean) ? 'ok' : 'bad');
    </script>
  </body>
</html>
)HTML", "https://example.test/index.html");

    require(
        page.outer_html("body[data-cssom='ok']").has_value(),
        "CSSOM stylesheets, rules, declarations and cascade should behave like browser-facing APIs");
    require(
        has_request_kind(*loader, "https://example.test/base.css", pagecore::ResourceKind::Stylesheet),
        "CSSOM should load linked stylesheets as stylesheet resources");
}

void test_cssom_dynamic_sheets_media_disabled_and_adopted()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html>
  <head>
    <style id="sheet">
      .box { display: none; color: red; }
      @media screen { .media { color: teal; } }
    </style>
  </head>
  <body>
    <div id="box" class="box media"></div>
    <div id="parent" style="color: lime; visibility: hidden;">
      <span id="child" style="display: unset; opacity: unset;"></span>
    </div>
    <script>
      const box = document.getElementById('box');
      const child = document.getElementById('child');
      const sheetElement = document.getElementById('sheet');
      const sheet = sheetElement.sheet;
      const checks = [];

      checks.push(sheet.cssRules.item(0) === sheet.cssRules[0]);
      checks.push(sheet.cssRules[1].type === CSSRule.MEDIA_RULE);
      checks.push(sheet.cssRules[1].cssRules[0].style.color === 'teal');
      checks.push(getComputedStyle(box).display === 'none');
      checks.push(getComputedStyle(box).color === 'rgb(0, 128, 128)');

      const childStyle = getComputedStyle(child);
      checks.push(childStyle.color === 'rgb(0, 255, 0)');
      checks.push(childStyle.visibility === 'hidden');
      checks.push(childStyle.display === 'inline');
      checks.push(childStyle.opacity === '1');
      checks.push(childStyle.length > 0);
      checks.push(childStyle.item(0) !== '');
      checks.push(childStyle.getPropertyPriority('color') === '');
      child.style.cssFloat = 'right';
      checks.push(child.style.getPropertyValue('float') === 'right');
      checks.push(getComputedStyle(child).cssFloat === 'right');

      // getComputedStyle() now reads litehtml's cascade, which only sees the
      // real DOM (the <style> element's literal text). sheet.disabled is a
      // CSSOM-only flag with no DOM representation, so it no longer
      // suppresses the rule here — this matches the real renderer, which
      // never honored it either (known limitation).
      sheet.disabled = true;
      checks.push(getComputedStyle(box).display === 'none');

      sheet.disabled = false;
      sheet.cssRules[0].style.display = 'inline-block';
      checks.push(getComputedStyle(box).display === 'inline-block');

      sheet.replaceSync('.box { color: navy; }');
      checks.push(sheet.cssRules.length === 1);
      checks.push(sheetElement.textContent.indexOf('navy') >= 0);
      checks.push(getComputedStyle(box).color === 'rgb(0, 0, 128)');

      // document.adoptedStyleSheets has no backing DOM node at all, so it is
      // likewise invisible to litehtml's cascade (known limitation, matches
      // the real renderer which never read adopted sheets either).
      const adopted = new CSSStyleSheet();
      adopted.replaceSync('.box { color: maroon !important; background-color: silver; }');
      document.adoptedStyleSheets = [adopted];
      checks.push(document.adoptedStyleSheets.length === 1);
      checks.push(document.styleSheets.length === 1);
      checks.push(getComputedStyle(box).color === 'rgb(0, 0, 128)');
      checks.push(getComputedStyle(box).backgroundColor === 'rgba(0, 0, 0, 0)');

      adopted.replace('.box { color: olive; }').then((resolved) => {
        checks.push(resolved === adopted);
        checks.push(getComputedStyle(box).color === 'rgb(0, 0, 128)');
        document.body.setAttribute('data-cssom-dynamic', checks.every(Boolean) ? 'ok' : 'bad');
      });
    </script>
  </body>
</html>
)HTML", "https://example.test/index.html");

    require(
        page.outer_html("body[data-cssom-dynamic='ok']").has_value(),
        "CSSOM dynamic rules, media rules, disabled sheets and adopted stylesheets should update computed style");
}

// Page::computed_style() is the C++ entry point getComputedStyle() bridges
// to (see js_runtime.cpp's bridge_computed_style). It must work standalone,
// before any layout()/render() call — mirroring a script calling
// getComputedStyle() during page load, before the page is ever painted.
void test_page_computed_style_cpp_api()
{
    pagecore::LoadOptions options;
    options.enable_js = false;
    pagecore::Page page(options);
    page.load_html(
        R"HTML(<html><body><div id="box" style="display:flex"></div></body></html>)HTML",
        "https://example.test/index.html");

    const pagecore::NodeId box = page.document().query_selector(page.document().document_node(), "#box");
    require(box != pagecore::kInvalidNodeId, "expected to find #box");

    const auto before_version = page.document().mutation_version();
    auto style = page.computed_style(box);
    require(style.has_value(), "Page::computed_style should resolve a simple inline style before any render");

    bool found_display = false;
    for (const auto& [name, value] : style->properties) {
        if (name == "display") {
            require(value == "flex", "display should resolve to 'flex' from the inline style attribute");
            found_display = true;
        }
    }
    require(found_display, "computed style should include a display property");
    require(
        page.document().mutation_version() == before_version,
        "Page::computed_style must not perturb mutation_version");

    require(
        !page.computed_style(999999).has_value(),
        "Page::computed_style should return nullopt for an unknown node id");
}

// Regression coverage for getComputedStyle() cases where the old hand-rolled
// JS cascade (replaced by litehtml in this revision) silently disagreed with
// a real CSS engine: dynamic pseudo-classes inside :not(), the inherited
// property allow-list, numeric @media evaluation, and UA-stylesheet display
// defaults beyond the hardcoded tag list.
void test_get_computed_style_matches_real_cascade_for_cases_js_engine_got_wrong()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html>
<head>
<style>
  li:not(:first-child) { color: blue; }
  @media (min-width: 5000px) { .big-screen-only { color: red; } }
</style>
</head>
<body>
  <ul style="list-style-type: square;">
    <li id="first">one</li>
    <li id="second">two</li>
  </ul>
  <li id="bare-li"></li>
  <table id="bare-table"></table>
  <div id="media-test" class="big-screen-only"></div>
  <script>
    const first = document.getElementById('first');
    const second = document.getElementById('second');
    const bareLi = document.getElementById('bare-li');
    const bareTable = document.getElementById('bare-table');
    const mediaTest = document.getElementById('media-test');

    const checks = [
      // :not(:first-child) — the JS matcher treated any dynamic pseudo-class
      // inside :not() as always-false, so :not(:first-child) always matched.
      getComputedStyle(first).color === 'rgb(0, 0, 0)',
      getComputedStyle(second).color === 'rgb(0, 0, 255)',
      // list-style-type is a real inherited CSS property; the JS cascade's
      // inheritance allow-list omitted it.
      getComputedStyle(second).listStyleType === 'square',
      // UA-stylesheet display defaults beyond the JS cascade's hardcoded tag
      // list (which fell back to 'block' for both).
      getComputedStyle(bareLi).display === 'list-item',
      getComputedStyle(bareTable).display === 'table',
      // The JS cascade's @media handling matched on the substring
      // 'min-width' rather than evaluating it against the viewport.
      getComputedStyle(mediaTest).color === 'rgb(0, 0, 0)'
    ];
    document.body.setAttribute('data-cascade-check', checks.every(Boolean) ? 'ok' : 'bad');
  </script>
</body>
</html>
)HTML", "https://example.test/index.html");

    require(
        page.outer_html("body[data-cascade-check='ok']").has_value(),
        "getComputedStyle() should match litehtml's real cascade for cases the old JS engine got wrong");
}

void test_dom_fragment_range_serializer_and_mutation_observer()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html><body>
  <div id="root"></div>
  <script>
    const root = document.getElementById('root');
    const seen = [];
    new MutationObserver((records) => {
      for (const record of records) seen.push(record.type + ':' + (record.attributeName || ''));
      if (!root.hasAttribute('data-mut')) root.setAttribute('data-mut', seen.join(','));
    }).observe(root, { attributes: true, childList: true, subtree: true });
    const filtered = [];
    new MutationObserver((records) => {
      for (const record of records) filtered.push(record.attributeName);
      root.setAttribute('data-filtered', filtered.join(','));
    }).observe(root, { attributeFilter: ['data-order'] });

    const range = document.createRange();
    range.selectNodeContents(root);
    const fragment = range.createContextualFragment('<span class="a">A</span><span>B</span>');
    root.appendChild(fragment);
    root.insertAdjacentHTML('afterbegin', '<em>E</em>');
    root.querySelector('em').insertAdjacentText('afterend', 'T');

    const xml = new XMLSerializer().serializeToString(root);
    root.setAttribute('data-order', root.textContent);
    root.setAttribute('data-xml', xml.indexOf('<span class="a">A</span>') >= 0 ? 'ok' : 'bad');
  </script>
</body></html>
)HTML");

    auto root = page.outer_html("#root[data-order='ETAB'][data-xml='ok']");
    require(root && root->find("<em>E</em>T<span class=\"a\">A</span><span>B</span>") != std::string::npos,
            "Range fragments, insertAdjacentHTML/Text and XMLSerializer should operate on Lexbor nodes");
    require(root->find("data-mut=\"childList:,childList:,childList:,attributes:data-order,attributes:data-xml\"") != std::string::npos,
            "MutationObserver should receive queued childList and attribute records");
    require(root->find("data-filtered=\"data-order\"") != std::string::npos,
            "MutationObserver attributeFilter should imply attributes and filter attribute records");
}

void test_page_enforces_dom_node_budget()
{
    // Regression for the native DOM-node budget: js_memory_limit_bytes bounds only
    // the JS heap, so without this an adversarial createElement loop exhausts native
    // (Lexbor) memory before the JS deadline fires. A low budget must stop it.
    pagecore::LoadOptions options;
    options.max_dom_nodes = 100;
    pagecore::Page page(options);
    page.load_html(R"HTML(<html><body><script>
      window.__created = 0;
      window.__hit = false;
      try {
        for (let i = 0; i < 100000; i++) { document.createElement('div'); window.__created++; }
      } catch (e) { window.__hit = true; }
    </script></body></html>)HTML");
    require(page.eval("String(window.__hit)") == "true",
            "exceeding max_dom_nodes should throw a catchable error in script");
    require(std::stoi(page.eval("String(window.__created)")) < 1000,
            "node creation must stop near the budget rather than run unbounded");

    // A generous default budget must not interfere with an ordinary page.
    pagecore::Page unbounded;
    unbounded.load_html(R"HTML(<html><body><script>
      for (let i = 0; i < 5000; i++) document.body.appendChild(document.createElement('span'));
      window.__count = document.querySelectorAll('span').length;
    </script></body></html>)HTML");
    require(unbounded.eval("String(window.__count)") == "5000",
            "a normal page well under the default budget should not be affected");
}

void test_page_enforces_aggregate_load_deadline()
{
    // Regression for the aggregate load deadline: js_timeout bounds each script, but
    // without an aggregate ceiling K scripts can consume K * js_timeout. The second
    // script must be skipped once the first exhausts the aggregate budget.
    std::vector<std::string> console;
    pagecore::LoadOptions options;
    options.max_load_time = std::chrono::milliseconds{50};
    options.js_timeout = std::chrono::milliseconds{30000};
    options.console_log = [&](std::string_view severity, std::string_view message) {
        console.emplace_back(std::string(severity) + ":" + std::string(message));
    };
    pagecore::Page page(options);
    page.load_html(R"HTML(<html><body>
      <script>window.__ran1 = true; const t = Date.now(); while (Date.now() - t < 60000) {}</script>
      <script>window.__ran2 = true;</script>
    </body></html>)HTML");
    require(page.eval("String(typeof window.__ran1)") == "boolean",
            "the first script should have started before the deadline");
    require(page.eval("String(typeof window.__ran2)") == "undefined",
            "the second script must be skipped once the aggregate load deadline is exceeded");
    const bool logged = std::any_of(console.begin(), console.end(), [](const std::string& line) {
        return line.find("Aggregate script execution deadline") != std::string::npos;
    });
    require(logged, "an aggregate-deadline console error should be reported");
}

void test_text_content_mutation_observer_records_nodes()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html><body>
  <div id="root"><span id="old">old</span><!--marker--></div>
  <script>
    const root = document.getElementById('root');
    const removedSpan = document.getElementById('old');
    const removedComment = root.childNodes[1];
    const records = [];
    new MutationObserver((batch) => {
      for (const record of batch) records.push(record);
      const childRecord = records.find((record) =>
        record.type === 'childList' &&
        record.target === root &&
        record.addedNodes.length === 1 &&
        record.addedNodes[0] === fresh &&
        record.removedNodes.length === 2 &&
        record.removedNodes[0] === removedSpan &&
        record.removedNodes[1] === removedComment);
      const textRecord = records.find((record) =>
        record.type === 'characterData' && record.target === fresh);
      const commentRecord = records.find((record) =>
        record.type === 'characterData' && record.target === comment);
      root.setAttribute('data-textcontent-records',
        childRecord && textRecord && commentRecord ? 'ok' : 'bad');
    }).observe(root, { childList: true, characterData: true, subtree: true });

    root.textContent = 'fresh';
    const fresh = root.firstChild;
    fresh.textContent = 'fresh2';
    const comment = document.createComment('before');
    root.appendChild(comment);
    comment.textContent = 'after';
  </script>
</body></html>
)HTML");

    require(
        page.outer_html("#root[data-textcontent-records='ok']").has_value(),
        "textContent mutations should report removed/added child nodes and characterData records for text and comments");
}

void test_document_write_fragment_insertion()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html><body>
  <script>
    document.write('<span id="a">A</span>', '<span id="b">B</span>');
    document.writeln('<span id="c">C</span>');
  </script>
  <div id="tail">T</div>
</body></html>
)HTML", "https://example.test/index.html");

    const auto body = page.outer_html("body");
    require(body && body->find("<span id=\"a\">A</span><span id=\"b\">B</span><span id=\"c\">C</span>") != std::string::npos,
            "document.write/writeln should parse HTML fragments and preserve write order");
    require(body->find("<span id=\"c\">C</span>\n") != std::string::npos,
            "document.writeln should append a newline after its HTML text");
}

void test_document_write_external_script_and_open_close()
{
    auto loader = std::make_shared<RecordingResourceLoader>();
    loader->add(
        "https://example.test/writer.js",
        "document.write('<main id=\"external-write\"><b>external</b></main>');",
        "text/javascript");

    pagecore::Page external_page;
    external_page.set_resource_loader(loader);
    external_page.load_html(R"HTML(
<html><body>
  <script src="/writer.js"></script>
  <div id="tail">tail</div>
</body></html>
)HTML", "https://example.test/index.html");

    const auto external_body = external_page.outer_html("body");
    require(
        external_body && external_body->find("<script src=\"/writer.js\"></script><main id=\"external-write\"><b>external</b></main>") != std::string::npos,
        "external scripts should document.write HTML after document.currentScript");
    require(
        has_request_kind(*loader, "https://example.test/writer.js", pagecore::ResourceKind::Script),
        "document.write external script test should load the writer through ResourceLoader");

    pagecore::Page open_page;
    open_page.load_html(R"HTML(
<html><body>
  <div id="old">old</div>
  <script>
    document.open();
    document.write('<section id="new">new</section>');
    document.close();
  </script>
</body></html>
)HTML", "https://example.test/open.html");

    require(open_page.outer_html("#new").has_value(), "document.open/write/close should create replacement body content");
    require(!open_page.outer_html("#old").has_value(), "document.open should clear previous body content");
}

void test_document_write_escaped_script_text_remains_text()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html><body>
  <script>
    document.write('&lt;script&gt;document.body.setAttribute("data-ran", "bad")&lt;/script&gt;');
  </script>
</body></html>
)HTML", "https://example.test/index.html");

    const auto body = page.outer_html("body");
    require(body && body->find("&lt;script&gt;document.body.setAttribute") != std::string::npos,
            "escaped script-like text should remain text when inserted through document.write");
    require(!page.outer_html("body[data-ran='bad']").has_value(),
            "escaped script-like text should not become executable HTML");
}

void test_comment_nodes_wrap_for_sibling_traversal()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html><body>
  <div id="first"></div><!--marker--><div id="second"></div>
  <script>
    const previous = document.getElementById('second').previousSibling;
    document.body.setAttribute('data-prev',
      previous instanceof Comment
        && previous.nodeType === Node.COMMENT_NODE
        && previous.nodeValue === 'marker'
        ? 'ok'
        : 'bad');
  </script>
</body></html>
)HTML", "https://example.test/comments.html");

    require(
        page.outer_html("body[data-prev='ok']").has_value(),
        "comment sibling nodes should wrap as Comment instead of being treated as Elements");
}

void test_character_data_interface()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html><body>
  <script>
    const checks = [];

    // Text and Comment derive from CharacterData, and a Comment is not a Text.
    const text = document.createTextNode('test');
    const comment = document.createComment('test');
    checks.push(text instanceof CharacterData);
    checks.push(comment instanceof CharacterData);
    checks.push(!(comment instanceof Text));
    checks.push(Object.getPrototypeOf(Text.prototype) === CharacterData.prototype);

    // The mutation API, including WebIDL unsigned-long wraparound: -1 becomes
    // 4294967295, which is past the end and must throw rather than clamp.
    text.appendData('ing');
    checks.push(text.data === 'testing');
    text.replaceData(0, 4, 'X');
    checks.push(text.data === 'Xing');
    checks.push(text.substringData(1, 2) === 'in');
    let threw = false;
    try { text.deleteData(-1, 0); } catch (error) { threw = error.name === 'IndexSizeError'; }
    checks.push(threw);

    // A number argument is data, not a node id: `new Text(42)` must stringify.
    // Telling the internal (id-based) and page (data-based) constructor paths
    // apart by argument type would silently adopt node 42 here.
    checks.push(new Text(42).data === '42');
    checks.push(new Text(undefined).data === '');
    checks.push(new Text(null).data === 'null');
    checks.push(new Comment(7).data === '7');

    // A constructed node keeps its wrapper identity once inserted.
    const constructed = new Text('tail');
    document.body.appendChild(constructed);
    checks.push(document.body.lastChild === constructed);

    document.body.setAttribute('data-ok', checks.every(Boolean) ? 'ok' : 'bad');
  </script>
</body></html>
)HTML", "https://example.test/character-data.html");

    require(
        page.outer_html("body[data-ok='ok']").has_value(),
        "CharacterData should own the data mutation API and treat a numeric constructor argument as data");
}

void test_pre_insertion_validity()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html><body>
  <div id="host"><span id="kid"></span></div>
  <div id="other"></div>
  <script>
    const checks = [];
    const nameOf = (fn) => { try { fn(); return 'no throw'; } catch (error) { return error.name; } };
    const host = document.getElementById('host');
    const kid = document.getElementById('kid');
    const other = document.getElementById('other');

    // A node may not be inserted into itself or into its own descendant.
    checks.push(nameOf(() => host.appendChild(host)) === 'HierarchyRequestError');
    checks.push(nameOf(() => kid.appendChild(host)) === 'HierarchyRequestError');

    // Only Document/DocumentFragment/Element can hold children, so a Text node is
    // never a valid parent. This is checked before the ancestor relationship.
    const text = document.createTextNode('x');
    checks.push(nameOf(() => text.appendChild(document.createElement('b'))) === 'HierarchyRequestError');
    checks.push(nameOf(() => text.appendChild(text)) === 'HierarchyRequestError');

    // A Document node itself is not insertable anywhere.
    checks.push(nameOf(() => host.appendChild(document)) === 'HierarchyRequestError');

    // The reference/removed child must actually be a child of the parent.
    checks.push(nameOf(() => host.insertBefore(document.createElement('b'), other)) === 'NotFoundError');
    checks.push(nameOf(() => host.removeChild(other)) === 'NotFoundError');
    checks.push(nameOf(() => host.replaceChild(document.createElement('b'), other)) === 'NotFoundError');

    // The document already has one element child (<html>), so it may not take
    // another, and a Text node may never be a child of a document.
    checks.push(nameOf(() => document.appendChild(document.createElement('b'))) === 'HierarchyRequestError');
    checks.push(nameOf(() => document.appendChild(document.createTextNode('x'))) === 'HierarchyRequestError');

    // Valid operations still work, and replaceChild returns the replaced node.
    const fresh = document.createElement('i');
    checks.push(host.replaceChild(fresh, kid) === kid);
    checks.push(host.firstChild === fresh);

    document.body.setAttribute('data-ok', checks.every(Boolean) ? 'ok' : 'bad');
  </script>
</body></html>
)HTML", "https://example.test/hierarchy.html");

    require(
        page.outer_html("body[data-ok='ok']").has_value(),
        "insertion should enforce DOM pre-insertion validity with HierarchyRequestError/NotFoundError");
}

void test_dom_token_list_is_an_ordered_set()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html><body>
  <div id="d" class="a a b"></div>
  <div id="e"></div>
  <script>
    const checks = [];
    const d = document.getElementById('d');
    const e = document.getElementById('e');

    // The token set is an ordered set, so duplicates collapse...
    checks.push(d.classList.length === 2);
    checks.push(d.classList[0] === 'a' && d.classList[1] === 'b');
    // ...but `value` is the verbatim attribute text, not the re-serialized set.
    checks.push(d.classList.value === 'a a b');

    // toggle(force=true) on a token that is already present is a no-op: it must not
    // rewrite (and thereby normalize) the attribute.
    d.classList.toggle('a', true);
    checks.push(d.getAttribute('class') === 'a a b');

    // Ordered-set replace: the first occurrence of either token takes the new value
    // and other occurrences are dropped, so "c b a" with c -> a becomes "a b".
    e.setAttribute('class', 'c b a');
    checks.push(e.classList.replace('c', 'a') === true);
    checks.push(e.getAttribute('class') === 'a b');

    // remove() on an element with no class attribute must not create one.
    const f = document.createElement('div');
    f.classList.remove('a');
    checks.push(!f.hasAttribute('class'));

    // Empty is a SyntaxError; whitespace is an InvalidCharacterError. Emptiness is
    // checked across all arguments first, so replace(" ", "") is a SyntaxError.
    const nameOf = (fn) => { try { fn(); return 'no throw'; } catch (error) { return error.name; } };
    checks.push(nameOf(() => d.classList.add('')) === 'SyntaxError');
    checks.push(nameOf(() => d.classList.add('a b')) === 'InvalidCharacterError');
    checks.push(nameOf(() => d.classList.replace(' ', '')) === 'SyntaxError');

    // [PutForwards=value]: assigning to classList writes the class attribute.
    e.classList = 'x y';
    checks.push(e.getAttribute('class') === 'x y');

    document.body.setAttribute('data-ok', checks.every(Boolean) ? 'ok' : 'bad');
  </script>
</body></html>
)HTML", "https://example.test/tokenlist.html");

    require(
        page.outer_html("body[data-ok='ok']").has_value(),
        "DOMTokenList should behave as an ordered set with spec-ordered validation errors");
}

void test_idl_attribute_reflection()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html><body>
  <div id="d"></div>
  <input id="i">
  <link id="l" rel="preload">
  <script>
    const checks = [];
    const div = document.getElementById('d');
    const input = document.getElementById('i');
    const link = document.getElementById('l');

    // Global attributes reflect on every HTML element.
    div.title = 'hi';
    checks.push(div.getAttribute('title') === 'hi');
    div.setAttribute('lang', 'en');
    checks.push(div.lang === 'en');

    // Enumerated: canonicalized case-insensitively, invalid falls back to "".
    div.setAttribute('dir', 'RTL');
    checks.push(div.dir === 'rtl');
    div.setAttribute('dir', 'sideways');
    checks.push(div.dir === '');

    // long, parsed by the HTML integer rules rather than Number(). Those rules stop
    // at the first non-digit and ignore the remainder, so "5%" is 5 (not NaN and not
    // an error), while a *leading* non-digit is invalid and yields the default.
    div.setAttribute('tabindex', '5%');
    checks.push(div.tabIndex === 5);
    div.setAttribute('tabindex', '%5');
    checks.push(div.tabIndex === 0);
    div.setAttribute('tabindex', ' 7');
    checks.push(div.tabIndex === 7);
    // "-0" parses to the integer zero, not to -0.
    div.setAttribute('tabindex', '-0');
    checks.push(Object.is(div.tabIndex, 0));

    // ARIA reflects as nullable strings: absent is null, and null removes.
    checks.push(div.ariaLabel === null);
    div.ariaLabel = 'label';
    checks.push(div.getAttribute('aria-label') === 'label');
    div.ariaLabel = null;
    checks.push(!div.hasAttribute('aria-label') && div.ariaLabel === null);

    // A reflected unsigned long is capped at 2147483647, not at 2^32-1.
    input.setAttribute('height', '2147483648');
    checks.push(input.height === 0);
    input.setAttribute('height', '120');
    checks.push(input.height === 120);

    // size is limited to positive values, with a default of 20.
    checks.push(input.size === 20);
    input.setAttribute('size', '0');
    checks.push(input.size === 20);

    // maxLength is a "limited long": a negative assignment throws.
    let threw = false;
    try { input.maxLength = -1; } catch (error) { threw = error.name === 'IndexSizeError'; }
    checks.push(threw);

    // formAction reads back the document URL when missing or empty.
    checks.push(input.formAction === document.URL);
    input.setAttribute('formaction', '');
    checks.push(input.formAction === document.URL);

    // crossOrigin is a nullable enum: absent is null, invalid maps to "anonymous".
    checks.push(link.crossOrigin === null);
    link.setAttribute('crossorigin', 'bogus');
    checks.push(link.crossOrigin === 'anonymous');
    link.setAttribute('as', 'SCRIPT');
    checks.push(link.as === 'script');

    document.body.setAttribute('data-ok', checks.every(Boolean) ? 'ok' : 'bad');
  </script>
</body></html>
)HTML", "https://example.test/reflection.html");

    require(
        page.outer_html("body[data-ok='ok']").has_value(),
        "content attributes should reflect as IDL properties with the per-type HTML coercion rules");
}

void test_live_collections()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html><body>
  <div id="host"><span id="one" class="c">a</span></div>
  <script>
    const checks = [];
    const host = document.getElementById('host');

    // Identity is stable, so a list captured before a mutation is the same object.
    const nodes = host.childNodes;
    const kids = host.children;
    checks.push(nodes === host.childNodes);
    checks.push(kids === host.children);
    checks.push(nodes instanceof NodeList);
    checks.push(kids instanceof HTMLCollection);

    // ...and it is live: the captured list sees a later append.
    checks.push(nodes.length === 1);
    const added = document.createElement('span');
    added.id = 'two';
    added.className = 'c';
    host.appendChild(added);
    checks.push(nodes.length === 2);
    checks.push(kids[1] === added);
    checks.push(nodes.item(2) === null);
    checks.push(!(2 in nodes));

    // getElementsBy* are live; querySelectorAll is a static snapshot.
    const live = document.getElementsByClassName('c');
    const snapshot = document.querySelectorAll('.c');
    checks.push(live.length === 2 && snapshot.length === 2);
    const third = document.createElement('span');
    third.className = 'c';
    host.appendChild(third);
    checks.push(live.length === 3);
    checks.push(snapshot.length === 2);

    // HTMLCollection exposes named access by id, but must not let an element
    // named "length" shadow the real length getter.
    checks.push(kids.namedItem('two') === added);
    checks.push(kids['one'] === document.getElementById('one'));
    checks.push(typeof kids.length === 'number');

    // Collections are iterable but carry no Array methods, exactly as in a browser.
    checks.push(typeof nodes.map === 'undefined');
    checks.push([...kids].length === 3);

    // Brand check: the getter must reject a receiver that is not a real collection.
    let threw = false;
    try { Object.create(kids).length; } catch (error) { threw = error instanceof TypeError; }
    checks.push(threw);

    document.body.setAttribute('data-ok', checks.every(Boolean) ? 'ok' : 'bad');
  </script>
</body></html>
)HTML", "https://example.test/collections.html");

    require(
        page.outer_html("body[data-ok='ok']").has_value(),
        "childNodes/children/getElementsBy* should be live collections with stable identity");
}

void test_dom_interface_globals()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html><body>
  <div id="host" data-x="1"></div>
  <script>
    const checks = [];
    const host = document.getElementById('host');

    checks.push(host.dataset instanceof DOMStringMap);
    checks.push(host.dataset.x === '1');

    checks.push(document instanceof HTMLDocument);
    checks.push(Object.prototype.toString.call(document) === '[object HTMLDocument]');
    // XMLDocument must exist and be distinct: WPT asserts a parsed document is
    // *not* an XMLDocument, which only means anything if the interface is there.
    checks.push(typeof XMLDocument === 'function');
    checks.push(!(document instanceof XMLDocument));

    const range = new StaticRange({ startContainer: host, startOffset: 0, endContainer: host, endOffset: 0 });
    checks.push(range.collapsed === true);
    checks.push(range.startContainer === host);

    checks.push(document.createEvent('FocusEvent') instanceof FocusEvent);
    checks.push(document.createEvent('hashchangeevent') instanceof HashChangeEvent);
    checks.push(document.createEvent('HTMLEvents') instanceof Event);
    let notSupported = false;
    try { document.createEvent('foo'); } catch (error) { notSupported = error.name === 'NotSupportedError'; }
    checks.push(notSupported);
    // Device-sensor and touch interfaces stay absent on purpose, so feature
    // detection is not told this engine has sensors it does not have.
    checks.push(typeof DeviceMotionEvent === 'undefined');
    checks.push(typeof TouchEvent === 'undefined');

    document.body.setAttribute('data-ok', checks.every(Boolean) ? 'ok' : 'bad');
  </script>
</body></html>
)HTML", "https://example.test/interfaces.html");

    require(
        page.outer_html("body[data-ok='ok']").has_value(),
        "DOM interface globals should be installed and createEvent should reject unmodelled interfaces");
}

void test_url_search_params_robustness()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html><body>
  <script>
    const checks = [];

    // The application/x-www-form-urlencoded parser works on bytes and must
    // never throw, unlike decodeURIComponent: a lone '%' or a truncated
    // percent-escape degrades to a literal '%' or U+FFFD instead of failing.
    checks.push(new URLSearchParams('b=%2sf%2a').toString() === 'b=%252sf*');
    checks.push(new URLSearchParams('b=%%2a').toString() === 'b=%25*');
    checks.push(new URLSearchParams('value=%C2').get('value') === '�');

    // USVString conversion: a lone surrogate cannot round-trip through UTF-8,
    // so both names and values replace it with U+FFFD.
    const surrogateParams = new URLSearchParams();
    surrogateParams.append('a\uD83Db', 'c');
    checks.push(surrogateParams.toString() === 'a%EF%BF%BDb=c');

    // Two-argument has()/delete(): the value filter is optional, and passing
    // undefined explicitly must behave as if it were omitted.
    const twoArg = new URLSearchParams('a=b&a=d');
    checks.push(twoArg.has('a', 'b') === true);
    checks.push(twoArg.has('a', 'c') === false);
    checks.push(twoArg.has('a', undefined) === true);
    twoArg.delete('a', 'b');
    checks.push(twoArg.toString() === 'a=d');

    // record<USVString, USVString> conversion: keys that only collide after
    // USVString conversion collapse to their last value but keep their first
    // position, matching Map#set semantics.
    const collidingKeys = new URLSearchParams({ '\uD835x': '1', xx: '2', '\uD83Dx': '3' });
    checks.push(collidingKeys.toString() === encodeURIComponent('�x') + '=3&xx=2');

    // A JS function is still an Object per WebIDL Type(V), so it must be read
    // as a record (own enumerable properties), not stringified via its source
    // text through the USVString fallback.
    function Tagged() {}
    Tagged.a = '1';
    Tagged.b = '2';
    checks.push(new URLSearchParams(Tagged).toString() === 'a=1&b=2');

    // A page can override an existing URLSearchParams instance's iterator;
    // the constructor must honor that override rather than special-casing
    // `instanceof URLSearchParams` and reading _entries directly.
    const overridden = new URLSearchParams();
    overridden[Symbol.iterator] = function* () { yield ['a', 'b']; };
    checks.push(new URLSearchParams(overridden).get('a') === 'b');

    // Live, index-based iteration: deleting the current entry mid-iteration
    // shifts the next one into the just-visited slot, so it must be skipped
    // rather than re-visited.
    const url = new URL('http://localhost/query?param0=0&param1=1&param2=2');
    const seen = [];
    for (const param of url.searchParams) {
      if (param[0] === 'param0') url.searchParams.delete('param0');
      else seen.push(param[0]);
    }
    checks.push(seen.length === 1 && seen[0] === 'param2');

    // Assigning url.search must update the existing searchParams object in
    // place (same identity), so a live for-of iterator over it observes the
    // new entries instead of iterating a detached snapshot.
    const identityUrl = new URL('http://a.b/c?a=1&b=2&c=3&d=4');
    const liveParams = identityUrl.searchParams;
    const collected = [];
    for (const entry of liveParams) {
      identityUrl.search = 'x=1&y=2&z=3';
      collected.push(entry[0]);
    }
    checks.push(collected.join(',') === 'a,y,z');

    document.body.setAttribute('data-ok', checks.every(Boolean) ? 'ok' : 'bad');
  </script>
</body></html>
)HTML", "https://example.test/urlsearchparams-robustness.html");

    require(
        page.outer_html("body[data-ok='ok']").has_value(),
        "URLSearchParams should parse/serialize losslessly and iterate live over its backing list");
}

void test_url_hostname_idna()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html><body>
  <script>
    const checks = [];

    // A non-ASCII hostname on a special scheme is Punycode-encoded.
    checks.push(new URL('https://Ä.example/x').hostname === 'xn--4ca.example');
    // IDNA mapping lowercases special-scheme hostnames.
    checks.push(new URL('https://EXAMPLE.COM/x').hostname === 'example.com');

    // A non-special scheme's host is opaque: no IDNA, no lowercasing.
    checks.push(new URL('foo://EXAMPLE.COM/x').hostname === 'EXAMPLE.COM');

    // An empty host on a special scheme is a hard parse failure.
    let threw = false;
    try { new URL('https:///path'); } catch (error) { threw = error.name === 'TypeError'; }
    checks.push(threw);

    // Forbidden host code points (space, control characters, ...) are
    // rejected even though IDNA mapping alone would not flag them; this is a
    // WHATWG URL Standard overlay on top of UTS46/IDNA, not part of it.
    threw = false;
    try { new URL('https://bad host.example/x'); } catch (error) { threw = error.name === 'TypeError'; }
    checks.push(threw);

    // The hostname/host setters fail silently (unlike the constructor): an
    // invalid assignment leaves the URL unchanged rather than throwing.
    const url = new URL('https://good.example/x');
    url.hostname = 'bad host with spaces';
    checks.push(url.hostname === 'good.example');
    url.host = 'also bad:1';
    checks.push(url.hostname === 'good.example' && url.port === '');

    document.body.setAttribute('data-ok', checks.every(Boolean) ? 'ok' : 'bad');
  </script>
</body></html>
)HTML", "https://example.test/hostname-idna.html");

    require(
        page.outer_html("body[data-ok='ok']").has_value(),
        "special-scheme hostnames should go through IDNA/Punycode and forbidden-code-point checks, opaque hosts should not");
}

void test_text_decoder_encoding_support()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html><body>
  <script>
    const checks = [];

    // A legacy multi-byte encoding decodes via the native Lexbor binding.
    const gbk = new TextDecoder('gbk');
    checks.push(gbk.encoding === 'gbk');
    checks.push(gbk.decode(new Uint8Array([0xc4, 0xe3, 0xba, 0xc3])) === '你好');

    // A label mapping to the WHATWG "replacement" encoding is rejected by the
    // constructor even though Lexbor resolves it to a real decoder (used
    // only for a different code path than TextDecoder, per spec).
    let threw = false;
    try { new TextDecoder('hz-gb-2312'); } catch (error) { threw = error.name === 'RangeError'; }
    checks.push(threw);
    threw = false;
    try { new TextDecoder('not-a-real-encoding'); } catch (error) { threw = error.name === 'RangeError'; }
    checks.push(threw);

    // Fatal mode: a decode error throws, and does not leave the decoder's
    // internal state corrupted for the next (unrelated) call.
    const fatalGbk = new TextDecoder('gbk', { fatal: true });
    threw = false;
    try { fatalGbk.decode(Uint8Array.from([0xfe, 0x39, 0xfe, 0x40])); } catch (error) { threw = error.name === 'TypeError'; }
    checks.push(threw);
    checks.push(fatalGbk.decode(Uint8Array.of(0x40)) === '@');

    // A BOM split across two streamed chunks is still recognized and
    // stripped once complete.
    const utf8 = new TextDecoder('utf-8');
    checks.push(utf8.decode(Uint8Array.of(0xef, 0xbb), { stream: true }) === '');
    checks.push(utf8.decode(Uint8Array.of(0xbf, 0x40)) === '@');

    // TextDecoderStream/TextEncoderStream: an astral character split across
    // two written chunks reassembles into a single decoded string.
    (async () => {
      const encodeStream = new TextEncoderStream();
      const writer = encodeStream.writable.getWriter();
      const reader = encodeStream.readable.getReader();
      const blueHeart = '\u{1F499}';
      writer.write(blueHeart[0]);
      writer.write(blueHeart[1]);
      writer.close();
      const bytes = [];
      for (;;) {
        const { value, done } = await reader.read();
        if (done) break;
        bytes.push(...value);
      }
      const decoded = new TextDecoder('utf-8').decode(new Uint8Array(bytes));
      checks.push(decoded === blueHeart);

      document.body.setAttribute('data-ok', checks.every(Boolean) ? 'ok' : 'bad');
    })();
  </script>
</body></html>
)HTML", "https://example.test/text-decoder-encoding.html");

    require(
        page.outer_html("body[data-ok='ok']").has_value(),
        "TextDecoder should support legacy multi-byte encodings via the native binding, reject replacement-mapped labels, "
        "recover cleanly from fatal errors, and stream BOM detection across chunk boundaries");
}

void test_node_move_before()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<!DOCTYPE html>
<html><body>
  <script>
    const checks = [];
    const nameOf = (fn) => { try { fn(); return 'no throw'; } catch (error) { return error.name; } };

    // moveBefore() is a ParentNode member, not a Node member: absent (not just
    // throwing) on non-parent types.
    checks.push(!('moveBefore' in document.createTextNode('x')));
    checks.push(!('moveBefore' in new Comment('c')));
    checks.push(!('moveBefore' in document.doctype));
    checks.push(typeof document.body.moveBefore === 'function');

    // WebIDL argument validation: both parameters are required (not optional)
    // Node-or-null references.
    checks.push(nameOf(() => document.body.moveBefore(null, null)) === 'TypeError');
    checks.push(nameOf(() => document.body.moveBefore(document.createTextNode('x'))) === 'TypeError');
    checks.push(nameOf(() => document.body.moveBefore(document.createTextNode('x'), {})) === 'TypeError');

    // Ordinary move: returns undefined (unlike insertBefore) and actually
    // relocates the node.
    const a = document.body.appendChild(document.createElement('div'));
    const b = a.appendChild(document.createElement('div'));
    const c = a.appendChild(document.createElement('div'));
    checks.push(a.moveBefore(c, b) === undefined);
    checks.push(a.firstChild === c && a.lastChild === b);

    // Moving a node before itself is a no-op, not an error.
    a.moveBefore(c, c);
    checks.push(a.firstChild === c && a.lastChild === b);

    // node must be an inclusive-ancestor-free, Element-or-CharacterData node:
    // a DocumentFragment or the document's own doctype cannot be moved.
    checks.push(nameOf(() => document.body.moveBefore(new DocumentFragment(), null)) === 'HierarchyRequestError');
    checks.push(nameOf(() => document.body.moveBefore(document.doctype, null)) === 'HierarchyRequestError');
    checks.push(nameOf(() => a.moveBefore(a, null)) === 'HierarchyRequestError');
    checks.push(nameOf(() => c.moveBefore(a, null)) === 'HierarchyRequestError');

    // A reference child that isn't this parent's own child is a NotFoundError,
    // not a HierarchyRequestError.
    const other = document.body.appendChild(document.createElement('div'));
    checks.push(nameOf(() => a.moveBefore(b, other)) === 'NotFoundError');

    // Pre-move validity is about a *shared* root, not literal connectedness to
    // the main document: two disconnected nodes in the very same detached
    // subtree may move between each other...
    const disconnectedRoot = document.createElement('div');
    const disconnectedBranch = disconnectedRoot.appendChild(document.createElement('div'));
    const disconnectedLeaf = disconnectedRoot.appendChild(document.createElement('p'));
    disconnectedBranch.moveBefore(disconnectedLeaf, null);
    checks.push(disconnectedBranch.firstChild === disconnectedLeaf);
    // ...but moving between two *unrelated* disconnected trees still throws,
    // and so does moving between a connected and a disconnected tree.
    const unrelatedDisconnected = document.createElement('div');
    checks.push(nameOf(() => unrelatedDisconnected.moveBefore(disconnectedLeaf, null)) === 'HierarchyRequestError');
    checks.push(nameOf(() => document.body.moveBefore(disconnectedLeaf, null)) === 'HierarchyRequestError');
    checks.push(nameOf(() => disconnectedRoot.moveBefore(document.body.firstChild, null)) === 'HierarchyRequestError');

    document.body.setAttribute('data-ok', checks.every(Boolean) ? 'ok' : 'bad');
  </script>
</body></html>
)HTML", "https://example.test/move-before.html");

    require(
        page.outer_html("body[data-ok='ok']").has_value(),
        "Node.moveBefore should be a ParentNode-only method enforcing pre-move validity "
        "(shared root, movable node type, NotFoundError reference child) and actually relocate the node");
}

void test_range_content_methods()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<!DOCTYPE html>
<html><body>
  <div id="a">Hello <b>cruel</b> world</div>
  <div id="siblings"><span id="s1">one</span><span id="s2">two</span></div>
  <script>
    const checks = [];
    const a = document.getElementById('a');

    // commonAncestorContainer used to be a stub returning startContainer; it
    // must compute the real nearest inclusive-ancestor-of-both container.
    const r = new Range();
    r.setStart(a.firstChild, 3);
    r.setEnd(a.lastChild, 3);
    checks.push(r.commonAncestorContainer === a);

    // cloneContents(): partial text at both edges, the fully-contained <b>
    // element cloned deeply in the middle; the original tree is untouched.
    const cloned = r.cloneContents();
    checks.push(cloned.childNodes.length === 3);
    checks.push(cloned.firstChild.data === 'lo ');
    checks.push(cloned.childNodes[1].outerHTML === '<b>cruel</b>');
    checks.push(cloned.lastChild.data === ' wo');
    checks.push(a.textContent === 'Hello cruel world');

    // extractContents(): same content, but removed from the original tree,
    // and the range collapses to a single point at the removal site.
    const r2 = new Range();
    r2.setStart(a.firstChild, 3);
    r2.setEnd(a.lastChild, 3);
    const extracted = r2.extractContents();
    checks.push(extracted.childNodes.length === 3 && extracted.childNodes[1].outerHTML === '<b>cruel</b>');
    checks.push(a.textContent === 'Helrld');
    checks.push(r2.collapsed && r2.startContainer === a);

    // toString(): concatenates partial boundary text with fully-contained
    // Text node data in tree order -- not simply startContainer.textContent.
    const p = document.createElement('p');
    p.append('Test div', document.createElement('br'), 'Another div');
    document.body.appendChild(p);
    const r3 = new Range();
    r3.setStart(p.firstChild, 0);
    r3.setEnd(p.lastChild, p.lastChild.data.length);
    checks.push(r3.toString() === 'Test divAnother div');

    // deleteContents(): same tree effect as extractContents(), no fragment.
    const q = document.createElement('p');
    q.textContent = 'Hello cruel world';
    document.body.appendChild(q);
    const r4 = new Range();
    r4.setStart(q.firstChild, 6);
    r4.setEnd(q.firstChild, 12);
    r4.deleteContents();
    checks.push(q.textContent === 'Hello world');

    // insertNode(): splits the start Text node at the boundary and inserts
    // before the split point.
    const ins = document.createElement('p');
    ins.textContent = 'Hello world';
    document.body.appendChild(ins);
    const r5 = new Range();
    r5.setStart(ins.firstChild, 6);
    r5.collapse(true);
    const bold = document.createElement('b');
    bold.textContent = 'NEW ';
    r5.insertNode(bold);
    checks.push(ins.innerHTML === 'Hello <b>NEW </b>world');

    // surroundContents(): wraps a fully-Text-contained range in newParent.
    const sur = document.createElement('p');
    sur.textContent = 'Hello world';
    document.body.appendChild(sur);
    const r6 = new Range();
    r6.setStart(sur.firstChild, 6);
    r6.setEnd(sur.firstChild, 11);
    r6.surroundContents(document.createElement('em'));
    checks.push(sur.innerHTML === 'Hello <em>world</em>');

    // surroundContents() throws InvalidStateError when a non-Text node is
    // only partially inside the range.
    const partial = document.createElement('p');
    partial.innerHTML = '<span>abc</span><span>def</span>';
    document.body.appendChild(partial);
    const r7 = new Range();
    r7.setStart(partial.firstChild.firstChild, 1);
    r7.setEnd(partial.lastChild.firstChild, 1);
    let threw = false;
    try { r7.surroundContents(document.createElement('em')); } catch (error) { threw = error.name === 'InvalidStateError'; }
    checks.push(threw);

    // compareBoundaryPoints/comparePoint/intersectsNode: ordering across
    // disjoint sibling subtrees.
    const siblings = document.getElementById('siblings');
    const s1 = document.getElementById('s1');
    const s2 = document.getElementById('s2');
    const rA = new Range(); rA.selectNode(s1);
    const rB = new Range(); rB.selectNode(s2);
    checks.push(rA.compareBoundaryPoints(Range.START_TO_START, rB) === -1);
    checks.push(rA.compareBoundaryPoints(Range.END_TO_START, rB) === -1);
    checks.push(rA.comparePoint(s2.firstChild, 0) === 1);
    checks.push(rA.intersectsNode(s1) === true);
    checks.push(rA.intersectsNode(s2) === false);
    checks.push(rA.isPointInRange(s1.firstChild, 1) === true);
    checks.push(rA.isPointInRange(s2.firstChild, 1) === false);

    document.body.setAttribute('data-ok', checks.every(Boolean) ? 'ok' : 'bad');
  </script>
</body></html>
)HTML", "https://example.test/range-content.html");

    require(
        page.outer_html("body[data-ok='ok']").has_value(),
        "Range's content methods (cloneContents/extractContents/deleteContents/insertNode/surroundContents), "
        "toString(), commonAncestorContainer, and the boundary-point comparisons should follow the DOM spec");
}

void test_dom_implementation_create_methods()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<!DOCTYPE html>
<html><body>
  <script>
    const checks = [];
    const nameOf = (fn) => { try { fn(); return 'no throw'; } catch (error) { return error.name; } };

    // The real, parsed document's doctype now reads its name/publicId/systemId
    // from Lexbor directly instead of the old '' placeholders.
    checks.push(document.doctype.name === 'html');
    checks.push(document.doctype.publicId === '' && document.doctype.systemId === '');

    // DOMImplementation.createDocumentType(): a standalone, unattached doctype
    // with real name/publicId/systemId and no parent yet.
    const dt = document.implementation.createDocumentType('svg:svg', 'pub-id', 'sys"id');
    checks.push(dt.nodeType === Node.DOCUMENT_TYPE_NODE);
    checks.push(dt.name === 'svg:svg' && dt.publicId === 'pub-id' && dt.systemId === 'sys"id');
    checks.push(dt.parentNode === null);

    // Name/QName production validation: InvalidCharacterError for a name that
    // fails the Name production, NamespaceError for one that fails QName
    // (multiple colons) while still matching Name.
    checks.push(nameOf(() => document.implementation.createDocumentType('1bad', '', '')) === 'InvalidCharacterError');
    checks.push(nameOf(() => document.implementation.createDocumentType('a:b:c', '', '')) === 'NamespaceError');
    checks.push(nameOf(() => document.implementation.createDocumentType('', '', '')) === 'InvalidCharacterError');

    // DOMImplementation.createDocument(): an XMLDocument distinct from
    // Document, holding just the doctype when qualifiedName is empty/null.
    const xmlDoctype = document.implementation.createDocumentType('qorflesnorf', 'abcde', 'x"\'y');
    const xmlDoc = document.implementation.createDocument(null, null, xmlDoctype);
    checks.push(xmlDoc instanceof XMLDocument);
    checks.push(xmlDoc.nodeType === Node.DOCUMENT_NODE);
    checks.push(xmlDoc.contentType === 'application/xml');
    checks.push(xmlDoc.doctype === xmlDoctype && xmlDoctype.parentNode === xmlDoc);
    checks.push(xmlDoc.documentElement === null);

    // contentType varies by namespace, per spec.
    checks.push(document.implementation.createDocument('http://www.w3.org/1999/xhtml', 'html', null).contentType === 'application/xhtml+xml');
    checks.push(document.implementation.createDocument('http://www.w3.org/2000/svg', 'svg', null).contentType === 'image/svg+xml');

    // createDocument's own root-element creation validates qualifiedName too.
    checks.push(nameOf(() => document.implementation.createDocument(null, 'a:b:c', null)) === 'NamespaceError');
    // Third argument must be a DocumentType (or null/omitted).
    checks.push(nameOf(() => document.implementation.createDocument(null, null, {})) === 'TypeError');

    // The XMLDocument returned is a real, working document-like container:
    // create*/appendChild/documentElement all work.
    const el = xmlDoc.createElement('everyone-hates-hyphenated-element-names');
    const textNode = xmlDoc.createTextNode('hello');
    el.appendChild(textNode);
    xmlDoc.appendChild(el);
    checks.push(xmlDoc.documentElement === el && el.parentNode === xmlDoc);
    checks.push(el.textContent === 'hello');
    checks.push(xmlDoc.createDocumentFragment() instanceof DocumentFragment);

    // createProcessingInstruction(): works on any document (HTML or XML);
    // target is exposed via nodeName too, matching Lexbor's generic node-name
    // lookup for PROCESSING_INSTRUCTION_NODE.
    const pi = xmlDoc.createProcessingInstruction('somePI', 'chirp chirp');
    checks.push(pi.nodeType === Node.PROCESSING_INSTRUCTION_NODE);
    checks.push(pi.target === 'somePI' && pi.nodeName === 'somePI' && pi.data === 'chirp chirp');
    checks.push(pi instanceof ProcessingInstruction && pi instanceof CharacterData);
    checks.push(nameOf(() => new ProcessingInstruction()) === 'TypeError');

    const piOnRealDocument = document.createProcessingInstruction('target2', 'data2');
    checks.push(piOnRealDocument.target === 'target2' && piOnRealDocument.nodeType === Node.PROCESSING_INSTRUCTION_NODE);

    // "?>" inside PI data is InvalidCharacterError, on any document.
    checks.push(nameOf(() => xmlDoc.createProcessingInstruction('t', 'a?>b')) === 'InvalidCharacterError');
    checks.push(nameOf(() => document.createProcessingInstruction('1bad', 'x')) === 'InvalidCharacterError');

    // createCDATASection(): NotSupportedError on HTML documents (the real
    // document and createHTMLDocument()'s result), works on an XMLDocument.
    checks.push(nameOf(() => document.createCDATASection('x')) === 'NotSupportedError');
    const foreignDoc = document.implementation.createHTMLDocument('');
    checks.push(nameOf(() => foreignDoc.createCDATASection('x')) === 'NotSupportedError');

    const cdata = xmlDoc.createCDATASection('1234');
    checks.push(cdata.nodeType === Node.CDATA_SECTION_NODE);
    checks.push(cdata.data === '1234' && cdata.nodeName === '#cdata-section');
    checks.push(cdata instanceof CDATASection && cdata instanceof Text);
    checks.push(nameOf(() => new CDATASection()) === 'TypeError');
    checks.push(nameOf(() => xmlDoc.createCDATASection('a]]>b')) === 'InvalidCharacterError');

    // A CDATA section behaves like ordinary CharacterData once inserted --
    // this is exactly the pattern WPT's dom/ranges/common.js fixture uses
    // (appending CDATA sections created off a throwaway `new Document()` into
    // a real HTML paragraph) to build a text-like node with three CDATA/text
    // children for Range tests. Left detached rather than appended to
    // document.body: Lexbor's own HTML serializer has no case for
    // LXB_DOM_NODE_TYPE_CDATA_SECTION (falls through to its generic error
    // path), so a CDATA descendant would break this test's own
    // page.outer_html() pass/fail signal below -- a known, narrow Lexbor
    // limitation, not a bug in this feature (see docs/browser-api-support.md).
    const p = document.createElement('p');
    p.appendChild(cdata);
    p.appendChild(xmlDoc.createCDATASection('5678'));
    p.append('9012');
    checks.push(p.textContent === '123456789012');
    checks.push(p.childNodes.length === 3);

    // The bare `new Document()` constructor: "return a new document that is
    // an XML document" -- simulated the same way createDocument() is.
    const bareDoc = new Document();
    checks.push(bareDoc instanceof XMLDocument);
    checks.push(bareDoc.createCDATASection('xyz').data === 'xyz');

    document.body.setAttribute('data-ok', checks.every(Boolean) ? 'ok' : 'bad');
  </script>
</body></html>
)HTML", "https://example.test/dom-implementation-create.html");

    require(
        page.outer_html("body[data-ok='ok']").has_value(),
        "DOMImplementation.createDocumentType()/createDocument() and "
        "Document.createProcessingInstruction()/createCDATASection() should follow the DOM spec "
        "(Name/QName validation, XMLDocument container, HTML-document NotSupportedError gate)");
}

void test_detached_html_document_appendchild_and_doctype()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<!DOCTYPE html>
<html><body>
  <script>
    const checks = [];
    const nameOf = (fn) => { try { fn(); return 'no throw'; } catch (error) { return error.name; } };

    // createHTMLDocument() creates its own implicit "html" doctype, per spec
    // -- it did not exist at all before this fix.
    const foreignDoc = document.implementation.createHTMLDocument('title');
    checks.push(foreignDoc.doctype !== null);
    checks.push(foreignDoc.doctype.name === 'html');
    checks.push(foreignDoc.doctype.publicId === '' && foreignDoc.doctype.systemId === '');
    checks.push(foreignDoc.doctype.parentNode === foreignDoc);
    checks.push(foreignDoc.doctype.nodeType === Node.DOCUMENT_TYPE_NODE);

    // childNodes/firstChild/lastChild put the doctype before documentElement.
    checks.push(foreignDoc.childNodes.length === 2);
    checks.push(foreignDoc.childNodes[0] === foreignDoc.doctype);
    checks.push(foreignDoc.childNodes[1] === foreignDoc.documentElement);
    checks.push(foreignDoc.firstChild === foreignDoc.doctype);
    checks.push(foreignDoc.lastChild === foreignDoc.documentElement);

    // appendChild()/removeChild() on the document object itself -- this is
    // exactly what WPT's dom/ranges/common.js fixture does
    // (foreignDoc.appendChild(foreignComment)) and previously threw
    // "foreignDoc.appendChild is not a function".
    const foreignComment = foreignDoc.createComment('hello');
    checks.push(foreignDoc.appendChild(foreignComment) === foreignComment);
    checks.push(foreignComment.parentNode === foreignDoc);
    checks.push(foreignDoc.lastChild === foreignComment);
    checks.push(foreignDoc.childNodes.length === 3);

    checks.push(foreignDoc.removeChild(foreignComment) === foreignComment);
    checks.push(foreignComment.parentNode === null);
    checks.push(foreignDoc.childNodes.length === 2);
    checks.push(nameOf(() => foreignDoc.removeChild(foreignComment)) === 'NotFoundError');

    // Document-level pre-insertion validity still applies: a second doctype
    // or a second element child is a HierarchyRequestError, reusing the same
    // validity gate the real document/XMLDocument already enforce.
    const anotherDoctype = document.implementation.createDocumentType('x', '', '');
    checks.push(nameOf(() => foreignDoc.appendChild(anotherDoctype)) === 'HierarchyRequestError');
    checks.push(nameOf(() => foreignDoc.appendChild(document.createElement('div'))) === 'HierarchyRequestError');

    document.body.setAttribute('data-ok', checks.every(Boolean) ? 'ok' : 'bad');
  </script>
</body></html>
)HTML", "https://example.test/detached-document-appendchild.html");

    require(
        page.outer_html("body[data-ok='ok']").has_value(),
        "createHTMLDocument()'s result should have an implicit doctype and a working "
        "appendChild()/removeChild() enforcing the same Document pre-insertion validity as a real document");
}

void test_create_comment_nodes_are_not_visible_text()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html><body>
  <div id="placeholder-host"><script id="placeholder-source">document.body.setAttribute('data-ran', 'bad');</script></div>
  <script>
    const host = document.getElementById('placeholder-host');
    const source = document.getElementById('placeholder-source');
    const placeholder = document.createComment(source.outerHTML);
    source.parentNode.replaceChild(placeholder, source);
    document.body.setAttribute('data-comment-node',
      placeholder instanceof Comment
        && placeholder.nodeType === Node.COMMENT_NODE
        && placeholder.nodeValue.indexOf('<script') === 0
        ? 'ok'
        : 'bad');
    document.body.setAttribute('data-body-text',
      host.textContent.indexOf('<script') === -1 ? 'ok' : 'bad');
  </script>
</body></html>
)HTML", "https://example.test/comment-placeholder.html");

    const auto body = page.outer_html("body");
    require(page.outer_html("body[data-comment-node='ok']").has_value(),
            "document.createComment should create real Comment nodes");
    require(page.outer_html("body[data-body-text='ok']").has_value(),
            "comment contents should not contribute to element textContent");
    require(body && body->find("<!--<script id=\"placeholder-source\">") != std::string::npos,
            "comment placeholders should serialize as comments, not escaped text");
    require(body && body->find("&lt;script id=\"placeholder-source\"") == std::string::npos,
            "comment placeholders should not serialize as visible escaped HTML text");
}

void test_event_options_bubbling_and_wpt_driver_shim()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html><body>
  <button id="button"></button>
  <script>
    const button = document.getElementById('button');
    const hits = [];

    const controller = new AbortController();
    button.addEventListener('click', () => hits.push('aborted'), { signal: controller.signal });
    controller.abort();

    button.addEventListener('click', { handleEvent(event) {
      hits.push(event.composedPath().indexOf(window) >= 0 ? 'path' : 'missing');
    }}, { once: true });
    window.addEventListener('click', () => hits.push('window'), { once: true });

    button.click();
    button.click();

    test_driver_internal = {};
    test_driver_internal.click(button).then(() => {
      button.setAttribute('data-driver', 'ok');
    });

    button.setAttribute('data-hits', hits.join('|'));
  </script>
</body></html>
)HTML");

    auto button = page.outer_html("#button[data-hits='path|window'][data-driver='ok']");
    require(button.has_value(), "event options, bubbling path and WPT test_driver click shim should work");
}

void test_wpt_completion_callback_registration_waits_for_harness_initialization()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html><body>
  <script>
    (function(globalScope) {
      function add_completion_callback(callback) {
        tests.all_done_callbacks.push(callback);
      }

      globalScope.add_completion_callback = add_completion_callback;

      var tests = {
        all_done_callbacks: [],
        status: { status: 0, message: null }
      };

      globalScope.__pagecore_wpt_registered_callback_count = tests.all_done_callbacks.length;
      setTimeout(function() {
        globalScope.__pagecore_wpt_registered_callback_count = tests.all_done_callbacks.length;
        tests.all_done_callbacks.forEach(function(callback) {
          callback([{ name: 'deferred upstream registration', status: 0 }], tests.status);
        });
      }, 0);
    })(window);
  </script>
</body></html>
)HTML", "https://web-platform.test/upstream-harness-order.html");

    require(
        page.eval("String(globalThis.__pagecore_wpt_registered_callback_count)") == "1",
        "WPT hook should register after an upstream-style harness initializes its internal tests object");
    require(
        page.eval("globalThis.__pagecore_wpt_json || ''").find("\"deferred upstream registration\"") != std::string::npos,
        "WPT hook should receive completion from an upstream-style harness");
}

// P0.1: test_driver_internal.click() must dispatch a real pointerdown/mousedown/
// pointerup/mouseup/click sequence (not just a single synthetic click), with
// the given coordinates and button, and focus a focusable target -- the
// real vendored testdriver.js's click() computes coordinates itself and
// always passes them through, so explicit coords here match its contract.
void test_wpt_driver_click_dispatches_real_event_sequence()
{
    pagecore::Page page;
    page.load_html(R"HTML(<html><body>
  <input id="target" type="text">
  <script>
    const target = document.getElementById('target');
    window.__events = [];
    for (const type of ['pointerdown', 'mousedown', 'pointerup', 'mouseup', 'click']) {
      target.addEventListener(type, (event) => {
        window.__events.push(`${type}:${event.clientX},${event.clientY},${event.button}`);
      });
    }

    test_driver_internal = {};
    test_driver_internal.click(target, { x: 12, y: 34 }).then(() => {
      window.__done = true;
      window.__focused = document.activeElement === target;
    });
  </script>
</body></html>)HTML");

    require(page.eval("String(window.__done)") == "true", "test_driver_internal.click() promise should resolve");
    require(page.eval("String(window.__focused)") == "true",
            "test_driver_internal.click() should focus a focusable target");
    require(page.eval("String(window.__events.join('|'))")
                == "pointerdown:12,34,0|mousedown:12,34,0|pointerup:12,34,0|mouseup:12,34,0|click:12,34,0",
            "test_driver_internal.click() should dispatch pointerdown, mousedown, pointerup, mouseup and click "
            "in order with the given coordinates and button");
}

// P0.1: test_driver_internal.send_keys() must focus the element and, per
// character, dispatch keydown -> keypress -> input (with the character
// appended to .value) -> keyup -- matching the one in-scope corpus file's
// plain-character usage (no WebDriver special-key codepoints).
void test_wpt_driver_send_keys_dispatches_keydown_input_keyup()
{
    pagecore::Page page;
    page.load_html(R"HTML(<html><body>
  <input id="target" type="text">
  <script>
    const target = document.getElementById('target');
    window.__events = [];
    for (const type of ['keydown', 'keypress', 'input', 'keyup']) {
      target.addEventListener(type, (event) => {
        window.__events.push(type + (event.key ? ':' + event.key : ''));
      });
    }

    test_driver_internal = {};
    test_driver_internal.send_keys(target, 'ab').then(() => {
      window.__done = true;
      window.__focused = document.activeElement === target;
    });
  </script>
</body></html>)HTML");

    require(page.eval("String(window.__done)") == "true", "test_driver_internal.send_keys() promise should resolve");
    require(page.eval("String(window.__focused)") == "true",
            "test_driver_internal.send_keys() should focus the target element");
    require(page.eval("String(document.getElementById('target').value)") == "ab",
            "test_driver_internal.send_keys() should type the given characters into the target's value");
    require(page.eval("String(window.__events.join('|'))")
                == "keydown:a|keypress:a|input|keyup:a|keydown:b|keypress:b|input|keyup:b",
            "test_driver_internal.send_keys() should dispatch keydown, keypress, input and keyup per character in order");
}

void test_custom_elements_registry_shim()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html><body>
  <typesense-minibar id="ce"><form></form></typesense-minibar>
  <script>
    window.customElements.define('typesense-minibar', class extends HTMLElement {
      static get observedAttributes () { return ['data-mode']; }
      connectedCallback () {
        this.setAttribute('data-connected', this.querySelector('form') ? 'yes' : 'no');
      }
      attributeChangedCallback (name, oldValue, newValue) {
        this.setAttribute('data-attr', name + ':' + oldValue + ':' + newValue);
      }
    });

    const element = document.getElementById('ce');
    element.setAttribute('data-mode', 'on');
    customElements.whenDefined('typesense-minibar').then((ctor) => {
      element.setAttribute('data-defined', customElements.get('typesense-minibar') === ctor ? 'yes' : 'no');
    });
  </script>
</body></html>
)HTML");

    auto element = page.outer_html("#ce[data-connected='yes'][data-attr='data-mode:null:on'][data-defined='yes']");
    require(element.has_value(), "customElements registry should define, upgrade and resolve custom element constructors");
}

void test_shadow_root_and_element_internals_shims()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html><body>
  <shadow-host id="host"></shadow-host>
  <script>
    customElements.define('shadow-host', class extends HTMLElement {
      connectedCallback () {
        const root = this.attachShadow({ mode: 'open', delegatesFocus: true });
        const child = document.createElement('span');
        child.id = 'inside';
        child.className = 'shadow-child';
        root.appendChild(child);
        root.prepend(document.createElement('b'));
        const prepended = root.firstChild && root.firstChild.localName === 'b';
        root.replaceChildren(child);

        const sheet = new CSSStyleSheet();
        sheet.replaceSync('.shadow-child { color: rgb(1, 2, 3); }');
        root.adoptedStyleSheets = [sheet];
        child.innerText = 'Shadow text';

        const slot = document.createElement('slot');

        let observedShadowRoot = false;
        new MutationObserver(() => { observedShadowRoot = true; }).observe(root, { attributeFilter: ['data-shadow'] });

        const internals = this.attachInternals();
        this.setAttribute('data-shadow',
          root instanceof ShadowRoot &&
          root.host === this &&
          root.mode === 'open' &&
          root.delegatesFocus === true &&
          this.shadowRoot === root &&
          root.querySelector('#inside') === child &&
          root.adoptedStyleSheets[0] === sheet &&
          // Shadow content is real litehtml-rendered DOM (see
          // attach_shadow_root), so getComputedStyle() resolves a real value
          // here — just not the adoptedStyleSheets rule, which (like on the
          // document) never reaches the cascade.
          getComputedStyle(child).color === 'rgb(0, 0, 0)' &&
          child.innerText === 'Shadow text' &&
          child.checkVisibility() === true &&
          // No positioned ancestor inside the shadow tree, so offsetParent
          // escalates to the host — same fallback real browsers use.
          child.offsetParent === this &&
          slot.assignedNodes().length === 0 &&
          slot.assignedElements().length === 0 &&
          prepended &&
          root.firstChild === child &&
          root.contains(child) &&
          child.parentNode === root &&
          child.getRootNode() === root &&
          child.getRootNode({ composed: true }) === document &&
          internals instanceof ElementInternals &&
          internals.shadowRoot === root &&
          observedShadowRoot === false
            ? 'ok'
            : 'bad');
      }
    });
  </script>
</body></html>
)HTML");

    require(
        page.outer_html("#host[data-shadow='ok']").has_value(),
        "attachShadow, ShadowRoot, getRootNode and ElementInternals should support browser-like custom elements");
}

void test_shadow_root_builds_tree_from_inner_html()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html><body>
  <slider-host id="host"><div class="slide">A</div><div class="slide">B</div></slider-host>
  <script>
    // Web components such as Swiper build their shadow tree by assigning a
    // template string to shadowRoot.innerHTML; the fragment must parse it.
    customElements.define('slider-host', class extends HTMLElement {
      connectedCallback () {
        const root = this.attachShadow({ mode: 'open' });
        root.innerHTML = '<div class="wrap"><div class="track"><slot></slot></div></div>';
        const track = root.querySelector('.track');
        let childrenSpread = -1;
        try { childrenSpread = [...track.children].length; } catch (e) { childrenSpread = -2; }
        this.setAttribute('data-slider',
          root.childNodes.length === 1 &&
          root.querySelector('.wrap') !== null &&
          track !== null &&
          track.children[0] instanceof HTMLSlotElement &&
          childrenSpread === 1 &&
          root.innerHTML.indexOf('class="track"') !== -1
            ? 'ok'
            : 'bad');
      }
    });
  </script>
</body></html>
)HTML");

    require(
        page.outer_html("#host[data-slider='ok']").has_value(),
        "shadowRoot.innerHTML should parse a template into a queryable shadow tree");
}

void test_shadow_dom_hides_markers_and_container_from_js()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html><body>
  <shadow-host id="host"></shadow-host>
  <script>
    customElements.define('shadow-host', class extends HTMLElement {
      connectedCallback () {
        const root = this.attachShadow({ mode: 'open' });
        const child = document.createElement('span');
        child.textContent = 'shadow text';
        root.appendChild(child);

        const style = getComputedStyle(child);

        this.setAttribute('data-check',
          this.outerHTML.indexOf('data-pc-shadow') === -1 &&
          this.outerHTML.indexOf('pc-shadowroot') === -1 &&
          this.innerHTML.indexOf('data-pc-shadow') === -1 &&
          this.innerHTML.indexOf('pc-shadowroot') === -1 &&
          !this.hasAttribute('data-pc-shadow-host') &&
          this.getAttribute('data-pc-shadow-host') === null &&
          this.getAttributeNames().every((name) => !name.startsWith('data-pc-shadow')) &&
          // Only the two marker attributes are hidden from query selectors;
          // reaching into shadow content itself is still allowed (no full
          // encapsulation — see attach_shadow_root's documented scope).
          document.querySelector('[data-pc-shadow-root]') === null &&
          document.querySelector('[data-pc-shadow-host]') === null &&
          document.querySelector('span').textContent === 'shadow text' &&
          style.display !== ''
            ? 'ok'
            : 'bad');
      }
    });
  </script>
</body></html>
)HTML");

    require(
        page.outer_html("#host[data-check='ok']").has_value(),
        "shadow bookkeeping markers and the container must stay invisible to "
        "outerHTML/innerHTML/getAttribute*/querySelector, while getComputedStyle "
        "still resolves real values for shadow content");
}

void test_custom_elements_with_private_fields_construct_instances()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html><body>
  <private-upgrade id="existing"></private-upgrade>
  <script>
    class PrivateUpgrade extends HTMLElement {
      #delegate;

      constructor () {
        super();
        this.#delegate = { localName: this.localName };
      }

      get delegate () { return this.#delegate; }

      connectedCallback () {
        this.setAttribute('data-connected', this.delegate.localName);
      }
    }

    customElements.define('private-upgrade', PrivateUpgrade);

    const existing = document.getElementById('existing');
    const created = document.createElement('private-upgrade');
    document.body.appendChild(created);

    document.body.setAttribute('data-private-custom-elements',
      existing.delegate.localName === 'private-upgrade' &&
      created.delegate.localName === 'private-upgrade' &&
      Object.getPrototypeOf(created.delegate) === Object.prototype &&
      existing.getAttribute('data-connected') === 'private-upgrade' &&
      created.getAttribute('data-connected') === 'private-upgrade'
        ? 'ok'
        : 'bad');
  </script>
</body></html>
)HTML");

    require(
        page.outer_html("body[data-private-custom-elements='ok']").has_value(),
        "custom elements should be constructed so private fields and instance properties are initialized");
}

void test_external_script_via_resource_loader()
{
    auto loader = std::make_shared<pagecore::MemoryResourceLoader>();
    loader->add("https://example.test/app.js",
                "document.body.setAttribute('data-external', 'yes');");

    pagecore::Page page;
    page.set_resource_loader(loader);
    page.load_html(R"HTML(
<html><body><script src="/app.js"></script></body></html>
)HTML", "https://example.test/index.html");

    auto body = page.outer_html("body[data-external='yes']");
    require(body.has_value(), "external script should load through ResourceLoader");
}

void test_current_script_and_reflected_url_attributes()
{
    auto loader = std::make_shared<pagecore::MemoryResourceLoader>();
    loader->add(
        "https://cdn.test/assets/runtime.js",
        R"JS(
const current = document.currentScript;
if (!current || current.tagName.toUpperCase() !== 'SCRIPT' || !current.src) {
  throw new Error('Automatic publicPath is not supported in this browser');
}
document.body.setAttribute('data-current-script', current.src);
document.body.setAttribute('data-public-path', current.src.replace(/\/[^\/]+$/, '/'));

const created = document.createElement('script');
created.src = '/assets/app.js';
document.body.appendChild(created);
document.body.setAttribute('data-created-src', created.src);

const link = document.createElement('a');
link.href = 'docs/page.html';
document.body.appendChild(link);
document.body.setAttribute('data-link-href', link.href);
)JS",
        "text/javascript");

    pagecore::Page page;
    page.set_resource_loader(loader);
    page.load_html(R"HTML(
<html><body><script src="https://cdn.test/assets/runtime.js"></script></body></html>
)HTML", "https://example.test/path/index.html");

    require(
        page.outer_html("body[data-current-script='https://cdn.test/assets/runtime.js'][data-public-path='https://cdn.test/assets/']").has_value(),
        "external script execution should expose document.currentScript with an absolute src");
    require(
        page.outer_html("body[data-created-src='https://example.test/assets/app.js'][data-link-href='https://example.test/path/docs/page.html']").has_value(),
        "reflected URL attributes should resolve against the document base URL");
}

void test_module_script_imports_relative_dependencies()
{
    auto loader = std::make_shared<RecordingResourceLoader>();
    loader->add(
        "https://example.test/app/modules/main.js",
        R"JS(
import { label, value } from './dep.js';
document.body.setAttribute('data-module', label + ':' + value);
document.body.setAttribute('data-current-script-module', document.currentScript === null ? 'null' : 'set');
document.body.setAttribute('data-import-meta', import.meta.url);
)JS",
        "text/javascript");
    loader->add(
        "https://example.test/app/modules/dep.js",
        "export const label = 'ok'; export const value = 42;",
        "text/javascript");

    pagecore::Page page;
    page.set_resource_loader(loader);
    page.load_html(R"HTML(
<html><body><script type="module" src="modules/main.js"></script></body></html>
)HTML", "https://example.test/app/index.html");

    require(
        page.outer_html("body[data-module='ok:42'][data-current-script-module='null']").has_value(),
        "module script should execute with imported bindings and null document.currentScript");
    require(
        page.eval("document.body.getAttribute('data-import-meta')") == "https://example.test/app/modules/main.js",
        "external module should expose import.meta.url");
    require(
        has_request_kind(*loader, "https://example.test/app/modules/main.js", pagecore::ResourceKind::Script),
        "external module script should be requested as script resource");
    require(
        has_request_kind(*loader, "https://example.test/app/modules/dep.js", pagecore::ResourceKind::Script),
        "module dependency should be loaded through ResourceLoader as script resource");
}

void test_inline_module_uses_document_url_for_relative_imports()
{
    auto loader = std::make_shared<RecordingResourceLoader>();
    loader->add(
        "https://example.test/app/dep.js",
        "export const value = 'inline-ok';",
        "text/javascript");

    pagecore::Page page;
    page.set_resource_loader(loader);
    page.load_html(R"HTML(
<html><body>
  <script type="module">
    import { value } from './dep.js';
    document.body.setAttribute('data-inline-module', value);
    document.body.setAttribute('data-inline-import-meta', import.meta.url);
  </script>
</body></html>
)HTML", "https://example.test/app/page.html");

    require(
        page.outer_html("body[data-inline-module='inline-ok']").has_value(),
        "inline module should resolve relative imports against document URL");
    require(
        page.eval("document.body.getAttribute('data-inline-import-meta')")
            == "https://example.test/app/page.html#inline-module-0",
        "inline module should expose a stable document-based import.meta.url");
    require(
        has_request_kind(*loader, "https://example.test/app/dep.js", pagecore::ResourceKind::Script),
        "inline module dependency should be loaded through ResourceLoader as script resource");
}

void test_html_element_specific_constructors()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html><body>
  <script>
    const button = document.createElement('button');
    const div = document.createElement('div');
    const image = document.createElement('img');
    const script = document.createElement('script');
    const unknown = document.createElement('blink');
    document.body.append(button, div, image, script, unknown);

    document.body.setAttribute('data-constructors',
      button instanceof HTMLButtonElement &&
      button instanceof HTMLElement &&
      !(div instanceof HTMLButtonElement) &&
      div instanceof HTMLDivElement &&
      image instanceof HTMLImageElement &&
      script instanceof HTMLScriptElement &&
      document.body instanceof HTMLBodyElement &&
      unknown instanceof HTMLUnknownElement
        ? 'ok'
        : 'bad');
  </script>
</body></html>
)HTML");

    require(
        page.outer_html("body[data-constructors='ok']").has_value(),
        "HTML element wrappers should expose tag-specific constructors and instanceof behavior");
}

void test_create_element_ns_and_template_content_clone()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html><body>
  <script>
    const svgNS = 'http://www.w3.org/2000/svg';
    const template = document.createElement('template');
    const handle = document.createElement('span');
    const svg = document.createElementNS(svgNS, 'svg');
    const path = document.createElementNS(svgNS, 'path');
    handle.classList.add('handle');
    path.setAttribute('d', 'M0 0h1v1z');
    template.content.appendChild(handle);
    handle.appendChild(svg);
    svg.appendChild(path);

    const clone = template.content.cloneNode(true);
    const clonedPath = clone.querySelector('path');
    document.body.appendChild(clone);

    document.body.setAttribute('data-template-svg',
      svg instanceof SVGSVGElement &&
      path instanceof SVGPathElement &&
      svg.namespaceURI === svgNS &&
      path.namespaceURI === svgNS &&
      clonedPath instanceof SVGPathElement &&
      document.querySelector('.handle svg path') instanceof SVGPathElement
        ? 'ok'
        : 'bad');
  </script>
</body></html>
)HTML");

    require(
        page.outer_html("body[data-template-svg='ok']").has_value(),
        "createElementNS and template.content.cloneNode should preserve SVG wrappers");
}

void test_global_event_listener_aliases_bind_to_window()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html><body>
  <script>
    addEventListener('load', () => {
      document.body.setAttribute('data-window-load-listener', 'ok');
    });
  </script>
</body></html>
)HTML");

    require(
        page.outer_html("body[data-window-load-listener='ok']").has_value(),
        "global addEventListener alias should bind to window");
}

void test_document_domain_and_cookie_jar()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html><body>
  <script>
    document.cookie = 'session=abc; path=/; domain=.example.test; samesite=lax';
    document.cookie = 'theme=light; path=/';
    document.cookie = 'session=gone; expires=Thu, 01 Jan 1970 00:00:00 GMT';
    document.body.setAttribute('data-domain', document.domain);
    document.body.setAttribute('data-cookie', document.cookie);
  </script>
</body></html>
)HTML", "https://sub.example.test/index.html");

    require(
        page.outer_html("body[data-domain='sub.example.test'][data-cookie='theme=light']").has_value(),
        "document.domain and document.cookie should provide basic browser-compatible state");
}

void test_document_location_aliases_window_location()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html><body>
  <script>
    document.body.setAttribute('data-location',
      document.location === window.location &&
      document.defaultView === window &&
      document.location.search === '?utm_source=test'
        ? 'ok'
        : 'bad');
  </script>
</body></html>
)HTML", "https://example.test/index.html?utm_source=test");

    require(
        page.outer_html("body[data-location='ok']").has_value(),
        "document.location should alias window.location");
}

void test_dom_implementation_create_html_document()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html><body>
  <script>
    const detached = document.implementation.createHTMLDocument('Detached');
    const base = detached.createElement('base');
    base.href = document.location.href;
    detached.head.appendChild(base);
    detached.body.innerHTML = '<form></form><form></form><main id="m"><span class="x">ok</span></main>';

    const parsed = new DOMParser().parseFromString('<section id="parsed">text</section>', 'text/html');

    document.body.setAttribute('data-dom-implementation',
      document.implementation instanceof DOMImplementation &&
      detached.title === 'Detached' &&
      detached.body.childNodes.length === 3 &&
      detached.getElementById('m').querySelector('.x').textContent === 'ok' &&
      detached.head.querySelector('base').href === 'https://example.test/page.html' &&
      parsed.body.querySelector('#parsed').textContent === 'text'
        ? 'ok'
        : 'bad');
  </script>
</body></html>
)HTML", "https://example.test/page.html");

    require(
        page.outer_html("body[data-dom-implementation='ok']").has_value(),
        "DOMImplementation.createHTMLDocument should provide a detached HTML document for parser libraries");
}

void test_message_channel_and_crypto_shims()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html><body>
  <script>
    const values = new Uint8Array(8);
    const returned = crypto.getRandomValues(values);
    const uuid = crypto.randomUUID();
    const channel = new MessageChannel();
    channel.port1.onmessage = (event) => {
      document.body.setAttribute('data-message-channel',
        event.data === 'ready' &&
        returned === values &&
        values.length === 8 &&
        /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/.test(uuid)
          ? 'ok'
          : 'bad');
    };
    channel.port2.postMessage('ready');
  </script>
</body></html>
)HTML");

    require(
        page.outer_html("body[data-message-channel='ok']").has_value(),
        "MessageChannel and crypto shims should provide browser-compatible basics");
}

void test_text_encoder_decoder_utf8_shims()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html><body>
  <script>
    const source = 'hello \u043f\u0440\u0438\u0432\u0435\u0442 \u{1F680}';
    const encoder = new TextEncoder();
    const encoded = encoder.encode(source);
    const decoded = new TextDecoder().decode(encoded);
    const target = new Uint8Array(4);
    const result = encoder.encodeInto('a\u{1F680}z', target);
    document.body.setAttribute('data-text-codec',
      encoder.encoding === 'utf-8' &&
      decoded === source &&
      result.read === 1 &&
      result.written === 1 &&
      target[0] === 97
        ? 'ok'
        : 'bad');
  </script>
</body></html>
)HTML");

    require(
        page.outer_html("body[data-text-codec='ok']").has_value(),
        "TextEncoder and TextDecoder should handle UTF-8 basics");
}

void test_escaped_colon_class_selector_fallback()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html><body>
  <div id="open" popover class=":popover-open"></div>
  <div id="closed" popover></div>
  <script>
    const open = document.querySelector('.\\:popover-open');
    const scopedOpen = document.querySelectorAll(':where([popover].\\:popover-open)');
    const closed = document.querySelectorAll(':where([popover]:not(.\\:popover-open))');
    document.body.setAttribute('data-escaped-selector',
      open && open.id === 'open' &&
      scopedOpen.length === 1 &&
      scopedOpen[0].id === 'open' &&
      closed.length === 1 &&
      closed[0].id === 'closed' &&
      open.matches('[popover].\\:popover-open')
        ? 'ok'
        : 'bad');
  </script>
</body></html>
)HTML");

    require(
        page.outer_html("body[data-escaped-selector='ok']").has_value(),
        "selector fallback should support escaped colon class selectors");
}

void test_target_pseudo_class_selector_fallback()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html><body>
  <section id="target"></section>
  <section id="other"></section>
  <script>
    const target = document.querySelector(':target');
    const all = document.querySelectorAll('section:target');
    document.body.setAttribute('data-target-selector',
      target && target.id === 'target' &&
      all.length === 1 &&
      all[0].matches(':target') &&
      !document.getElementById('other').matches(':target')
        ? 'ok'
        : 'bad');
  </script>
</body></html>
)HTML", "https://example.test/index.html#target");

    require(
        page.outer_html("body[data-target-selector='ok']").has_value(),
        ":target selector fallback should match the location hash element");
}

void test_request_response_fetch_object_shims()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html><body>
  <script>
    const request = new Request('/api/data', {
      method: 'post',
      headers: { 'x-test': 'yes' },
      body: 'payload'
    });
    const cloned = request.clone();
    const response = Response.json({ ok: true }, { status: 201 });
    response.json().then((data) => {
      document.body.setAttribute('data-fetch-objects',
        request.url === 'https://example.test/api/data' &&
        request.method === 'POST' &&
        cloned.headers.get('x-test') === 'yes' &&
        response.ok &&
        response.status === 201 &&
        response.headers.get('content-type') === 'application/json' &&
        data.ok === true
          ? 'ok'
          : 'bad');
    });
  </script>
</body></html>
)HTML", "https://example.test/page.html");

    require(
        page.outer_html("body[data-fetch-objects='ok']").has_value(),
        "Request and Response shims should expose basic Fetch API object behavior");
}

void test_xhr_and_fetch_load_through_resource_loader()
{
    auto loader = std::make_shared<RecordingResourceLoader>();
    loader->add(
        "https://api.test/data.json",
        R"JSON({"name":"xhr-ok"})JSON",
        "application/json");
    loader->add(
        "https://example.test/fetch.json",
        R"JSON({"name":"fetch-ok"})JSON",
        "application/json");

    pagecore::Page page;
    page.set_resource_loader(loader);
    page.load_html(R"HTML(
<html><body>
  <script>
    const xhr = new XMLHttpRequest();
    xhr.open('POST', 'https://api.test/data.json');
    xhr.setRequestHeader('Content-Type', 'application/json; charset=utf-8');
    xhr.onload = () => {
      const data = JSON.parse(xhr.responseText);
      document.body.setAttribute('data-xhr',
        xhr.status === 200 &&
        xhr.readyState === XMLHttpRequest.DONE &&
        xhr.getResponseHeader('content-type') === 'application/json' &&
        data.name === 'xhr-ok'
          ? 'ok'
          : 'bad');
    };
    xhr.send(JSON.stringify({ source: 'xhr' }));

    fetch('/fetch.json', {
      method: 'POST',
      headers: { 'Content-Type': 'text/plain' },
      body: 'fetch-body'
    }).then((response) => response.json()).then((data) => {
      document.body.setAttribute('data-fetch-loader', data.name);
    });
  </script>
</body></html>
)HTML", "https://example.test/page.html");

    require(
        page.outer_html("body[data-xhr='ok'][data-fetch-loader='fetch-ok']").has_value(),
        "XMLHttpRequest and fetch should load resources through ResourceLoader");
    const auto* xhr_request = find_request(*loader, "https://api.test/data.json");
    require(xhr_request != nullptr, "XMLHttpRequest request should be recorded");
    require(xhr_request->method == "POST", "XMLHttpRequest should pass the request method to ResourceLoader");
    require(xhr_request->body == R"JSON({"source":"xhr"})JSON",
            "XMLHttpRequest should pass the request body to ResourceLoader");
    require(has_header(*xhr_request, "content-type", "application/json; charset=utf-8"),
            "XMLHttpRequest should pass request headers to ResourceLoader");

    const auto* fetch_request = find_request(*loader, "https://example.test/fetch.json");
    require(fetch_request != nullptr, "fetch request should be recorded");
    require(fetch_request->method == "POST", "fetch should pass the request method to ResourceLoader");
    require(fetch_request->body == "fetch-body", "fetch should pass the request body to ResourceLoader");
    require(has_header(*fetch_request, "content-type", "text/plain"),
            "fetch should pass request headers to ResourceLoader");
}

void test_shared_cookie_jar_document_scripts_fetch_and_xhr()
{
    auto loader = std::make_shared<RecordingResourceLoader>();
    loader->add(
        "https://example.test/index.html",
        R"HTML(
<html><head><script src="/static.js"></script></head><body>
  <script>
    document.cookie = 'client=js; path=/';
    document.cookie = 'secret=public; path=/';
    document.cookie = 'static=gone; path=/; expires=Thu, 01 Jan 1970 00:00:00 GMT';

    fetch('/api', { credentials: 'same-origin' }).then((response) => {
      document.body.setAttribute('data-fetch-headers',
        response.status === 202 &&
        response.statusText === 'Accepted' &&
        response.headers.get('x-api') === 'yes' &&
        !response.headers.has('set-cookie')
          ? 'ok'
          : 'bad');
      return response.text();
    }).then(() => {
      document.body.setAttribute('data-after-fetch-cookie', document.cookie);
      const xhr = new XMLHttpRequest();
      xhr.open('GET', '/xhr');
      xhr.onload = () => {
        document.body.setAttribute('data-xhr',
          xhr.status === 200 &&
          xhr.statusText === 'OK' &&
          xhr.responseURL === 'https://example.test/xhr' &&
          xhr.getResponseHeader('x-xhr') === 'yes' &&
          xhr.getAllResponseHeaders().toLowerCase().indexOf('set-cookie') === -1
            ? 'ok'
            : 'bad');
        document.body.setAttribute('data-final-cookie', document.cookie);
      };
      xhr.send();

      // Insert the dynamic script only after the fetch has completed: requests
      // snapshot cookies when the transfer STARTS (like a browser), so a script
      // inserted while /api is still in flight would not see its Set-Cookie.
      const dynamic = document.createElement('script');
      dynamic.src = '/dynamic.js';
      document.head.appendChild(dynamic);
    });
  </script>
</body></html>
)HTML",
        "text/html",
        {
            {"Set-Cookie", "top=nav; Path=/"},
            {"Set-Cookie", "secret=hidden; Path=/; HttpOnly"},
        });
    loader->add(
        "https://example.test/static.js",
        "document.body.setAttribute('data-static-cookie', "
        "document.cookie.includes('top=nav') && !document.cookie.includes('secret=') ? 'ok' : 'bad');",
        "text/javascript",
        {{"Set-Cookie", "static=one; Path=/; HttpOnly"}});
    loader->add(
        "https://example.test/dynamic.js",
        "document.body.setAttribute('data-dynamic', document.cookie.includes('api=ok') ? 'ok' : 'bad');",
        "text/javascript",
        {{"Set-Cookie", "dynamic=two; Path=/"}});
    loader->add(
        "https://example.test/api",
        "api-ok",
        "text/plain",
        {
            {"X-Api", "yes"},
            {"Set-Cookie", "api=ok; Path=/"},
        },
        202,
        "Accepted");
    loader->add(
        "https://example.test/xhr",
        "xhr-ok",
        "text/plain",
        {
            {"X-Xhr", "yes"},
            {"Set-Cookie", "xhr=ok; Path=/"},
        });

    pagecore::Page page;
    page.set_resource_loader(loader);
    page.load_url("https://example.test/index.html");

    require(
        page.outer_html("body[data-static-cookie='ok'][data-fetch-headers='ok'][data-xhr='ok'][data-dynamic='ok']").has_value(),
        "shared CookieJar should be visible through document.cookie and Fetch/XHR response metadata should hide Set-Cookie");

    const auto body = page.outer_html("body");
    require(body && body->find("top=nav") != std::string::npos,
            "document.cookie should expose regular cookies from the shared jar");
    require(body && body->find("client=js") != std::string::npos,
            "document.cookie writes should update the shared jar");
    require(body && body->find("api=ok") != std::string::npos && body->find("xhr=ok") != std::string::npos,
            "Set-Cookie from fetch/XHR responses should update the shared jar");
    require(body && body->find("secret=hidden") == std::string::npos && body->find("static=one") == std::string::npos,
            "HttpOnly cookies should be sent on requests but hidden from document.cookie");

    const auto* static_request = find_request(*loader, "https://example.test/static.js");
    require(static_request != nullptr && header_contains(*static_request, "cookie", "top=nav")
                && header_contains(*static_request, "cookie", "secret=hidden"),
            "static scripts should receive cookies from top-level Set-Cookie");
    require(static_request != nullptr && static_request->referrer == "https://example.test/index.html",
            "static script requests should carry the document referrer");

    const auto* api_request = find_request(*loader, "https://example.test/api");
    require(api_request != nullptr && header_contains(*api_request, "cookie", "client=js")
                && header_contains(*api_request, "cookie", "static=one")
                && header_contains(*api_request, "cookie", "secret=hidden")
                && !header_contains(*api_request, "cookie", "secret=public"),
            "fetch should send document.cookie writes but document.cookie must not overwrite or delete HttpOnly cookies");
    require(api_request != nullptr && api_request->referrer == "https://example.test/index.html",
            "fetch requests should carry the document referrer by default");

    const auto* dynamic_request = find_request(*loader, "https://example.test/dynamic.js");
    require(dynamic_request != nullptr && header_contains(*dynamic_request, "cookie", "client=js")
                && header_contains(*dynamic_request, "cookie", "api=ok"),
            "dynamic scripts should receive cookies updated by earlier fetch responses");
    require(dynamic_request != nullptr && dynamic_request->referrer == "https://example.test/index.html",
            "dynamic script requests should carry the document referrer");

    const auto* xhr_request = find_request(*loader, "https://example.test/xhr");
    require(xhr_request != nullptr && header_contains(*xhr_request, "cookie", "api=ok"),
            "XMLHttpRequest should send cookies from the shared jar");
    require(xhr_request != nullptr && xhr_request->referrer == "https://example.test/index.html",
            "XMLHttpRequest requests should carry the document referrer by default");
}

void test_fetch_xhr_credentials_control_cookie_injection()
{
    auto loader = std::make_shared<RecordingResourceLoader>();
    loader->add("https://example.test/omit", "omit", "text/plain");
    loader->add(
        "https://other.test/set",
        "set",
        "text/plain",
        {{"Set-Cookie", "third=1; Domain=other.test; Path=/; SameSite=None; Secure"}});
    loader->add("https://other.test/same", "same", "text/plain");
    loader->add("https://other.test/include", "include", "text/plain");
    loader->add("https://other.test/xhr-same", "xhr-same", "text/plain");
    loader->add("https://other.test/xhr-include", "xhr-include", "text/plain");
    loader->add("https://example.test:8443/port", "port", "text/plain");
    loader->add(
        "https://example.test:8443/set-port",
        "set-port",
        "text/plain",
        {{"Set-Cookie", "crossport=bad; Domain=example.test; Path=/"}});
    loader->add("https://example.test/after-port", "after-port", "text/plain");

    pagecore::Page page;
    page.set_resource_loader(loader);
    page.load_html(R"HTML(
<html><body>
  <script>
    document.cookie = 'sid=1; path=/';
    fetch('/omit', { credentials: 'omit' })
      .then(() => fetch('https://other.test/set', { credentials: 'include' }))
      .then(() => fetch('https://other.test/same'))
      .then(() => fetch('https://other.test/include', { credentials: 'include' }))
      .then(() => fetch('https://example.test:8443/port'))
      .then(() => fetch('https://example.test:8443/set-port'))
      .then(() => fetch('/after-port'))
      .then(() => new Promise((resolve) => {
        const xhrSame = new XMLHttpRequest();
        xhrSame.open('GET', 'https://other.test/xhr-same');
        xhrSame.onload = resolve;
        xhrSame.send();
      }))
      .then(() => new Promise((resolve) => {
        const xhrInclude = new XMLHttpRequest();
        xhrInclude.open('GET', 'https://other.test/xhr-include');
        xhrInclude.withCredentials = true;
        xhrInclude.onload = resolve;
        xhrInclude.send();
      }))
      .then(() => document.body.setAttribute('data-credentials', 'ok'));
  </script>
</body></html>
)HTML", "https://example.test/index.html");

    require(page.outer_html("body[data-credentials='ok']").has_value(),
            "credentials test chain should complete");

    const auto* omit_request = find_request(*loader, "https://example.test/omit");
    require(omit_request != nullptr && !header_contains(*omit_request, "cookie", "sid=1"),
            "fetch credentials=omit should not send same-origin cookies");

    const auto* same_request = find_request(*loader, "https://other.test/same");
    require(same_request != nullptr && !header_contains(*same_request, "cookie", "third=1"),
            "fetch default same-origin credentials should not send cross-origin cookies");

    const auto* include_request = find_request(*loader, "https://other.test/include");
    require(include_request != nullptr && header_contains(*include_request, "cookie", "third=1"),
            "fetch credentials=include should send matching cross-origin cookies");

    const auto* port_request = find_request(*loader, "https://example.test:8443/port");
    require(port_request != nullptr && !header_contains(*port_request, "cookie", "sid=1"),
            "fetch default same-origin credentials should treat different ports as different origins");

    const auto* after_port_request = find_request(*loader, "https://example.test/after-port");
    require(after_port_request != nullptr && !header_contains(*after_port_request, "cookie", "crossport=bad"),
            "fetch default same-origin credentials should not accept Set-Cookie from a different port origin");

    const auto* xhr_same_request = find_request(*loader, "https://other.test/xhr-same");
    require(xhr_same_request != nullptr && !header_contains(*xhr_same_request, "cookie", "third=1"),
            "XMLHttpRequest without withCredentials should use same-origin credentials");

    const auto* xhr_include_request = find_request(*loader, "https://other.test/xhr-include");
    require(xhr_include_request != nullptr && header_contains(*xhr_include_request, "cookie", "third=1"),
            "XMLHttpRequest withCredentials should include matching cross-origin cookies");
}

void test_cookie_attributes_path_domain_expires_secure_samesite()
{
    auto loader = std::make_shared<RecordingResourceLoader>();
    loader->add("https://example.test/app/check", "app", "text/plain");
    loader->add("https://example.test/api/check", "api", "text/plain");
    loader->add("https://sub.example.test/app/check", "sub", "text/plain");
    loader->add("http://example.test/app/insecure", "insecure", "text/plain");
    loader->add(
        "https://third.test/set",
        "set",
        "text/plain",
        {
            {"Set-Cookie", "thirdStrict=1; Domain=third.test; Path=/; SameSite=Strict; Secure"},
            {"Set-Cookie", "thirdLax=1; Domain=third.test; Path=/; SameSite=Lax; Secure"},
            {"Set-Cookie", "thirdDefault=1; Domain=third.test; Path=/; Secure"},
            {"Set-Cookie", "thirdNone=1; Domain=third.test; Path=/; SameSite=None; Secure"},
            {"Set-Cookie", "thirdBadNone=1; Domain=third.test; Path=/; SameSite=None"},
        });
    loader->add("https://third.test/again", "again", "text/plain");

    pagecore::Page page;
    page.set_resource_loader(loader);
    page.load_html(R"HTML(
<html><body>
  <script>
    document.cookie = 'pathok=1; Path=/app';
    document.cookie = 'pathmiss=1; Path=/private';
    document.cookie = 'domainok=1; Domain=.example.test; Path=/app; SameSite=Strict';
    document.cookie = 'domainbad=1; Domain=evil.test; Path=/app';
    document.cookie = 'secureok=1; Path=/app; SameSite=None; Secure';
    document.cookie = 'badnone=1; Path=/app; SameSite=None';
    document.cookie = 'expired=gone; Path=/app; Expires=Thu, 01 Jan 1970 00:00:00 GMT';
    document.cookie = 'maxgone=gone; Path=/app; Max-Age=0';
    fetch('/app/check', { credentials: 'include' })
      .then(() => fetch('/api/check', { credentials: 'include' }))
      .then(() => fetch('https://sub.example.test/app/check', { credentials: 'include' }))
      .then(() => fetch('http://example.test/app/insecure', { credentials: 'include' }))
      .then(() => fetch('https://third.test/set', { credentials: 'include' }))
      .then(() => fetch('https://third.test/again', { credentials: 'include' }))
      .then(() => document.body.setAttribute('data-cookie-attrs', 'ok'));
  </script>
</body></html>
)HTML", "https://example.test/start/index.html");

    require(page.outer_html("body[data-cookie-attrs='ok']").has_value(),
            "cookie attribute test chain should complete");

    const auto* app_request = find_request(*loader, "https://example.test/app/check");
    require(app_request != nullptr && header_contains(*app_request, "cookie", "pathok=1")
                && header_contains(*app_request, "cookie", "domainok=1")
                && header_contains(*app_request, "cookie", "secureok=1"),
            "matching Path/Domain/Secure cookies should be sent to same-site HTTPS paths");
    require(app_request != nullptr && !header_contains(*app_request, "cookie", "pathmiss=1")
                && !header_contains(*app_request, "cookie", "domainbad=1")
                && !header_contains(*app_request, "cookie", "badnone=1")
                && !header_contains(*app_request, "cookie", "expired=gone")
                && !header_contains(*app_request, "cookie", "maxgone=gone"),
            "non-matching, rejected, and expired cookies should not be sent");

    const auto* api_request = find_request(*loader, "https://example.test/api/check");
    require(api_request != nullptr && !header_contains(*api_request, "cookie", "pathok=1")
                && !header_contains(*api_request, "cookie", "domainok=1"),
            "Path-scoped cookies should not be sent outside their path prefix");

    const auto* subdomain_request = find_request(*loader, "https://sub.example.test/app/check");
    require(subdomain_request != nullptr && header_contains(*subdomain_request, "cookie", "domainok=1")
                && !header_contains(*subdomain_request, "cookie", "pathok=1"),
            "Domain cookies should reach same-site subdomains while host-only cookies should not");

    const auto* insecure_request = find_request(*loader, "http://example.test/app/insecure");
    require(insecure_request != nullptr && !header_contains(*insecure_request, "cookie", "secureok=1")
                && !header_contains(*insecure_request, "cookie", "pathok=1"),
            "Secure and default-Lax cookies should not be sent to cross-scheme subresource requests");

    const auto* third_again_request = find_request(*loader, "https://third.test/again");
    require(third_again_request != nullptr && header_contains(*third_again_request, "cookie", "thirdNone=1")
                && !header_contains(*third_again_request, "cookie", "thirdStrict=1")
                && !header_contains(*third_again_request, "cookie", "thirdLax=1")
                && !header_contains(*third_again_request, "cookie", "thirdDefault=1")
                && !header_contains(*third_again_request, "cookie", "thirdBadNone=1"),
            "cross-site subresource requests should only send SameSite=None cookies that satisfy Secure");
}

void test_base64_codec_roundtrip_and_edge_cases()
{
    using pagecore::Base64DecodeMode;

    // RFC 4648 vectors and round-trip.
    require(pagecore::base64_encode("") == "", "encode empty string");
    require(pagecore::base64_encode("f") == "Zg==", "encode 'f'");
    require(pagecore::base64_encode("fo") == "Zm8=", "encode 'fo'");
    require(pagecore::base64_encode("foo") == "Zm9v", "encode 'foo'");
    require(pagecore::base64_decode("Zm9vYg==") == "foob", "decode with padding");
    const std::string data = std::string("Hello, PageCore!\x00\x01\xfe\xff", 20);
    require(pagecore::base64_decode(pagecore::base64_encode(data)) == data, "base64 round-trip preserves bytes");

    auto rejects = [](std::string_view input, Base64DecodeMode mode) {
        try {
            (void) pagecore::base64_decode(input, mode);
            return false;
        } catch (...) {
            return true;
        }
    };

    // Strict rejects non-alphabet chars, embedded whitespace and %4==1 lengths.
    require(rejects("****", Base64DecodeMode::Strict), "strict rejects non-alphabet input");
    require(rejects("Zm9v Yg==", Base64DecodeMode::Strict), "strict rejects embedded whitespace");
    require(rejects("Zm9vY", Base64DecodeMode::Strict), "strict rejects length %% 4 == 1");

    // Forgiving (WHATWG data-URL) strips whitespace and re-pads missing padding,
    // but still rejects a %4==1 length and non-alphabet characters.
    require(pagecore::base64_decode("Zm9v Yg==", Base64DecodeMode::Forgiving) == "foob",
            "forgiving strips ASCII whitespace");
    require(pagecore::base64_decode("Zm9vYg", Base64DecodeMode::Forgiving) == "foob",
            "forgiving re-pads a missing '=='");
    require(rejects("Zm9vY", Base64DecodeMode::Forgiving), "forgiving still rejects length %% 4 == 1");
    require(rejects("****", Base64DecodeMode::Forgiving), "forgiving still rejects non-alphabet input");
}

void test_script_type_classification()
{
    using pagecore::is_javascript_script_type;
    using pagecore::is_module_script_type;

    // A missing or empty type attribute is a classic JavaScript script.
    require(is_javascript_script_type(std::nullopt), "missing type is JavaScript");
    require(is_javascript_script_type(std::optional<std::string>("")), "empty type is JavaScript");
    require(is_javascript_script_type(std::optional<std::string>("   ")), "whitespace-only type is JavaScript");

    // Every whitelisted MIME type counts as JavaScript, case-insensitively.
    for (const char* type : {"text/javascript", "application/javascript", "application/ecmascript",
                             "text/ecmascript", "application/x-javascript", "text/jscript"}) {
        require(is_javascript_script_type(std::optional<std::string>(type)),
                std::string("standard type is JavaScript: ") + type);
    }
    require(is_javascript_script_type(std::optional<std::string>("TEXT/JavaScript")),
            "type matching is case-insensitive");

    // Parameters after ';' (e.g. a charset) are stripped before matching, and
    // surrounding whitespace is trimmed first.
    require(is_javascript_script_type(std::optional<std::string>("text/javascript;charset=utf-8")),
            "charset parameter is ignored");
    require(is_javascript_script_type(std::optional<std::string>("  text/javascript;charset=utf-8  ")),
            "outer whitespace and charset are ignored");
    // Only the leading/trailing whitespace of the whole value is trimmed; a space
    // left in front of the ';' is not, so this does not match the whitelist.
    require(!is_javascript_script_type(std::optional<std::string>("text/javascript ; charset=utf-8")),
            "whitespace before ';' is not trimmed away");

    // Modules are JavaScript, and are the only type reported as a module.
    require(is_javascript_script_type(std::optional<std::string>("module")), "module is JavaScript");
    require(is_module_script_type(std::optional<std::string>("module")), "module type is a module");
    require(is_module_script_type(std::optional<std::string>("  MODULE  ")),
            "module detection trims and lowercases");
    require(!is_module_script_type(std::nullopt), "missing type is not a module");
    require(!is_module_script_type(std::optional<std::string>("text/javascript")),
            "classic JavaScript is not a module");

    // Unknown/non-JavaScript types are neither executable classic scripts nor modules.
    require(!is_javascript_script_type(std::optional<std::string>("text/plain")),
            "text/plain is not JavaScript");
    require(!is_javascript_script_type(std::optional<std::string>("application/json")),
            "application/json is not JavaScript");
    require(!is_module_script_type(std::optional<std::string>("text/plain")),
            "text/plain is not a module");
}

void test_css_scan_url_target()
{
    using pagecore::parse_css_url_target;

    // The primitive is called with `pos` just after the '(' and returns the
    // unescaped target, advancing `pos` past the closing ')'.
    auto parse = [](std::string_view after_paren) {
        std::size_t pos = 0;
        std::string target = parse_css_url_target(after_paren, pos);
        return std::pair<std::string, std::size_t>{target, pos};
    };

    // Unquoted target, consumed up to and past ')'.
    {
        auto [target, pos] = parse("foo.png)");
        require(target == "foo.png", "unquoted target");
        require(pos == 8, "pos advances past ')'");
    }
    // Quoted targets (both quote styles) preserve inner spaces.
    require(parse("\"a b.png\")").first == "a b.png", "double-quoted target keeps spaces");
    require(parse("'c.png')").first == "c.png", "single-quoted target");
    // Leading and trailing whitespace around an unquoted target is stripped.
    require(parse("   d.png  )").first == "d.png", "unquoted target is trimmed");
    // Backslash escapes are unescaped inside quoted targets.
    require(parse("\"a\\)b\\\"c\")").first == "a)b\"c", "quoted escapes are unescaped");
    // Empty targets (bare and quoted) return an empty string but still advance pos.
    {
        auto [target, pos] = parse(")");
        require(target.empty(), "bare empty target");
        require(pos == 1, "pos advances past ')' even for empty target");
    }
    require(parse("\"\")").first.empty(), "quoted empty target");
}

void test_css_scan_extract_urls()
{
    using pagecore::extract_css_urls;
    using pagecore::CssUrlRef;
    using pagecore::ResourceKind;

    // Plain property url() targets are extracted as images.
    {
        const auto urls = extract_css_urls("div { background: url(img.png); }");
        require(urls.size() == 1, "one url extracted");
        require(urls[0].url == "img.png", "image url target");
        require(urls[0].kind == ResourceKind::Image, "background url is an image");
    }

    // @import url() targets are classified as stylesheets.
    {
        const auto urls = extract_css_urls("@import url(\"theme.css\");");
        require(urls.size() == 1, "one @import url");
        require(urls[0].url == "theme.css", "@import target");
        require(urls[0].kind == ResourceKind::Stylesheet, "@import url is a stylesheet");
    }

    // src: declarations (web fonts) are skipped by the image prefetch scanner.
    {
        const auto urls = extract_css_urls("@font-face { src: url(font.woff2); }");
        require(urls.empty(), "src: url is skipped");
    }

    // A url() inside a comment is ignored; only the live declaration is kept.
    {
        const auto urls = extract_css_urls("/* url(ignored.png) */ a { background: url(real.png); }");
        require(urls.size() == 1, "commented url ignored");
        require(urls[0].url == "real.png", "live url kept");
    }

    // Quoted targets honour backslash escapes.
    {
        const auto urls = extract_css_urls("a { background: url(\"a\\)b.png\"); }");
        require(urls.size() == 1, "one escaped url");
        require(urls[0].url == "a)b.png", "backslash escape unescaped in quoted target");
    }

    // Protocol-relative targets are extracted verbatim (resolution is the caller's job).
    {
        const auto urls = extract_css_urls("a { background: url(//cdn.test/a.png); }");
        require(urls.size() == 1, "one protocol-relative url");
        require(urls[0].url == "//cdn.test/a.png", "protocol-relative target kept raw");
    }

    // Multi-value declarations yield every target in order.
    {
        const auto urls = extract_css_urls("a { background: url(a.png), url(b.png); }");
        require(urls.size() == 2, "two urls in a multi-value declaration");
        require(urls[0].url == "a.png" && urls[1].url == "b.png", "urls kept in order");
    }

    // `url` as part of an identifier (e.g. a custom property) is not a function call.
    {
        const auto urls = extract_css_urls("a { --myurl: 1; }");
        require(urls.empty(), "identifier ending in 'url' is not a url() token");
    }
}

void test_css_scan_parse_percentage()
{
    using pagecore::parse_css_percentage;

    require(parse_css_percentage("50%") == 50.0f, "plain percentage");
    require(parse_css_percentage("  33.5%  ") == 33.5f, "surrounding whitespace trimmed");
    require(parse_css_percentage("50% !important") == 50.0f, "!important stripped");
    require(!parse_css_percentage("50px").has_value(), "px is not a percentage");
    require(!parse_css_percentage("100").has_value(), "missing unit is not a percentage");
    require(!parse_css_percentage("").has_value(), "empty value has no percentage");
    require(!parse_css_percentage("auto").has_value(), "keyword is not a percentage");
}

void test_css_declarations_require_tree_rebuild()
{
    using pagecore::css_declarations_require_tree_rebuild;

    // Own-box metrics never force a rebuild.
    require(!css_declarations_require_tree_rebuild("width:10px;height:20px"), "size changes are patchable");
    require(!css_declarations_require_tree_rebuild("color:red;background:blue"), "color changes are patchable");
    require(!css_declarations_require_tree_rebuild(""), "empty declarations are patchable");
    require(!css_declarations_require_tree_rebuild("margin:5px;padding:3px"), "box spacing is patchable");

    // Structure-affecting properties force a rebuild.
    require(css_declarations_require_tree_rebuild("display:none"), "display forces a rebuild");
    require(css_declarations_require_tree_rebuild("width:10px;position:absolute"), "position forces a rebuild");
    require(css_declarations_require_tree_rebuild("float:left"), "float forces a rebuild");
    require(css_declarations_require_tree_rebuild("direction:rtl"), "direction forces a rebuild");
    require(css_declarations_require_tree_rebuild("list-style-type:none"), "list-style-* forces a rebuild");
    require(css_declarations_require_tree_rebuild("content:'x'"), "content forces a rebuild");

    // A property value that merely contains a keyword must not false-positive.
    require(!css_declarations_require_tree_rebuild("background:url(display.png)"),
            "a value mentioning 'display' must not be mistaken for the display property");
    require(!css_declarations_require_tree_rebuild("font-family:'position sans'"),
            "a quoted value must not be mistaken for a structure property");

    // A structural property obscured by a CSS comment must still be detected:
    // litehtml strips comments before applying inline styles, so the gate must too.
    require(css_declarations_require_tree_rebuild("color:red;/*x*/display:none"),
            "a comment before a structural property must not hide it from the gate");
    require(css_declarations_require_tree_rebuild("display/**/:none"),
            "a comment between the property name and colon must not hide it");
    require(!css_declarations_require_tree_rebuild("color:red/*display:none*/"),
            "a structural keyword inside a comment must be ignored");
    // A CSS escape in the name can hide a structural property; be conservative.
    require(css_declarations_require_tree_rebuild("\\64 isplay:none"),
            "an escaped property name must conservatively force a rebuild");

    // inline_style_property_value stays consistent with the shared tokenizer.
    require(pagecore::inline_style_property_value("width:10px;height:20px", "height")
                == std::optional<std::string>("20px"),
            "inline_style_property_value reads a declared property");
    require(pagecore::inline_style_property_value("color:red;/*c*/width:12px", "width")
                == std::optional<std::string>("12px"),
            "inline_style_property_value skips comments between declarations");
    require(pagecore::inline_style_property_value("width:5px !important", "width")
                == std::optional<std::string>("5px"),
            "inline_style_property_value strips !important");
    require(!pagecore::inline_style_property_value("width:5px", "height").has_value(),
            "inline_style_property_value returns nullopt for an absent property");
}

void test_css_scan_attribute_selectors()
{
    using pagecore::collect_css_attribute_selectors;
    using pagecore::css_attribute_selector_names;
    using pagecore::CssAttributeSelectorUsage;

    auto names_of = [](std::string_view css) {
        CssAttributeSelectorUsage usage;
        collect_css_attribute_selectors(css, usage);
        auto names = css_attribute_selector_names(usage);
        std::sort(names.begin(), names.end());
        return std::pair<std::vector<std::string>, bool>{names, usage.wildcard};
    };

    {
        auto [names, wildcard] = names_of("a[data-x=\"y\"] b[data-z] { color: red; }");
        require(!wildcard, "concrete selectors do not set wildcard");
        require(names == (std::vector<std::string>{"data-x", "data-z"}), "collects both attribute names");
    }
    {
        auto [names, wildcard] = names_of("[DATA-Foo] {}");
        require(!wildcard, "single named selector is not wildcard");
        require(names == (std::vector<std::string>{"data-foo"}), "attribute name is lowercased");
    }
    {
        auto [names, wildcard] = names_of("[svg|href] {}");
        require(!wildcard, "namespaced selector is not wildcard");
        require(names == (std::vector<std::string>{"href"}), "namespace prefix stripped to local name");
    }
    {
        auto [names, wildcard] = names_of("[  ] {}");
        require(wildcard, "empty attribute selector forces wildcard");
    }
    {
        auto [names, wildcard] = names_of("a[foo");
        require(wildcard, "unterminated attribute selector forces wildcard");
    }
    {
        auto [names, wildcard] = names_of("[foo\\=bar] {}");
        require(wildcard, "escaped attribute name forces wildcard");
    }
}

void test_css_scan_default_computed_style()
{
    using pagecore::default_computed_style_property_value;

    auto display = [](std::string_view tag) {
        return default_computed_style_property_value("display", tag).value_or("<none>");
    };
    require(display("div") == "block", "div defaults to block");
    require(display("span") == "inline", "span defaults to inline");
    require(display("li") == "list-item", "li defaults to list-item");
    require(display("table") == "table", "table default display");
    require(display("td") == "table-cell", "td default display");
    require(display("input") == "inline-block", "form control defaults to inline-block");
    require(display("script") == "none", "script defaults to display:none");
    require(display("UNKNOWNTAG") == "inline", "unknown tag defaults to inline");

    // Non-display properties with simple tag-independent defaults.
    require(default_computed_style_property_value("position", "div") == "static", "position default");
    require(default_computed_style_property_value("margin-left", "div") == "0px", "margin default");
    require(default_computed_style_property_value("width", "div") == "auto", "width default");
    require(default_computed_style_property_value("max-width", "div") == "none", "max-width default");
    // Property matching is case-insensitive.
    require(default_computed_style_property_value("DISPLAY", "DIV") == "block", "case-insensitive property/tag");
    // Properties without a simple default return nullopt.
    require(!default_computed_style_property_value("color", "div").has_value(), "color has no simple default");
}

void test_page_activity_tracker_counters_and_stability()
{
    pagecore::PageActivityTracker tracker;
    tracker.reset(1);
    require(tracker.network_idle(), "a freshly reset tracker is network-idle");
    require(tracker.mutation_observers_drained(), "no mutation observers pending initially");
    require(!tracker.snapshot().load_fired, "load has not fired initially");

    tracker.begin(pagecore::PageActivityKind::XhrFetch);
    tracker.begin(pagecore::PageActivityKind::XhrFetch);
    require(tracker.snapshot().pending_xhr_fetch == 2, "two XHR/fetch activities pending");
    require(!tracker.network_idle(), "pending XHR keeps the page non-idle");
    tracker.end(pagecore::PageActivityKind::XhrFetch);
    tracker.end(pagecore::PageActivityKind::XhrFetch);
    require(tracker.network_idle(), "network-idle once all XHR activities end");

    // Underflow guard: an unmatched end() must not wrap the unsigned counter.
    tracker.end(pagecore::PageActivityKind::XhrFetch);
    require(tracker.snapshot().pending_xhr_fetch == 0, "unmatched end() must not underflow the counter");
    require(tracker.network_idle(), "still network-idle after a spurious end()");

    // Relevant timers also gate network idleness.
    tracker.set_relevant_timers(1);
    require(!tracker.network_idle(), "a pending relevant timer keeps the page non-idle");
    tracker.set_relevant_timers(0);
    require(tracker.network_idle(), "network-idle once relevant timers drain");

    // dom_stable requires the clock to advance past the last mutation by the window.
    tracker.mark_mutation(2);
    require(!tracker.dom_stable(std::chrono::milliseconds(100)), "not DOM-stable immediately after a mutation");
    tracker.set_clock(std::chrono::milliseconds(200));
    require(tracker.dom_stable(std::chrono::milliseconds(100)), "DOM-stable once the quiet window elapses");

    // set_clock is monotonic (never moves backward), so a stale timestamp is ignored.
    tracker.set_clock(std::chrono::milliseconds(50));
    require(tracker.snapshot().clock == std::chrono::milliseconds(200), "set_clock must be monotonic (non-decreasing)");

    tracker.mark_load_fired();
    require(tracker.snapshot().load_fired, "load_fired reflected after mark_load_fired()");
}

void test_cookie_public_suffix_and_injection_rejected()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html><body>
  <script>
    document.cookie = 'super=1; Domain=example';
    document.cookie = 'tld=1; Domain=com';
    document.cookie = 'good=1';
    document.cookie = 'inject=a\r\nSet-Cookie: evil=1';
    document.cookie = '__Host-ok=1; Path=/; Secure';
    document.cookie = '__Host-bad=1; Path=/app; Secure';
    document.cookie = '__Secure-bad=1';
    document.cookie = 'maxwins=1; Max-Age=0; Expires=Tue, 01 Jan 2030 00:00:00 GMT';
    document.cookie = 'maxkeep=1; Expires=Thu, 01 Jan 1970 00:00:00 GMT; Max-Age=100000';
    document.cookie = 'maxbig=1; Max-Age=10000000000';
    document.cookie = '__HoSt-ci=1; Path=/; Secure';
    document.cookie = '__SeCuRe-ci=1';
    var c = document.cookie;
    var mark = function(name, needle){ document.body.setAttribute('data-' + name, c.indexOf(needle) >= 0 ? '1' : '0'); };
    mark('good', 'good=1');
    mark('super', 'super=1');
    mark('tld', 'tld=1');
    mark('inject', 'inject=');
    mark('evil', 'evil=1');
    mark('hostok', '__Host-ok=1');
    mark('hostbad', '__Host-bad=1');
    mark('securebad', '__Secure-bad=1');
    mark('maxwins', 'maxwins=1');
    mark('maxkeep', 'maxkeep=1');
    mark('maxbig', 'maxbig=1');
    mark('hostci', '__HoSt-ci=1');
    mark('secureci', '__SeCuRe-ci=1');
  </script>
</body></html>
)HTML", "https://shop.example/start/index.html");

    const auto marked = [&](const char* name, const char* value) {
        return page.outer_html(std::string("body[data-") + name + "='" + value + "']").has_value();
    };
    require(marked("good", "1"), "host-only cookie should be accepted");
    require(marked("super", "0"), "cookie scoped to a public suffix (Domain=example) must be rejected");
    require(marked("tld", "0"), "cookie scoped to a bare TLD (Domain=com) must be rejected");
    require(marked("inject", "0") && marked("evil", "0"),
            "cookie name/value with CR/LF control characters must be rejected (header-injection defense)");
    require(marked("hostok", "1"), "valid __Host- cookie should be accepted");
    require(marked("hostbad", "0"), "__Host- cookie with a non-root path must be rejected");
    require(marked("securebad", "0"), "__Secure- cookie without the Secure attribute must be rejected");
    require(marked("maxwins", "0"), "Max-Age=0 must take precedence over a later future Expires");
    require(marked("maxkeep", "1"), "Max-Age must take precedence over an earlier past Expires");
    require(marked("maxbig", "1"),
            "a huge Max-Age must be clamped, not overflow the clock and wrap the expiry into the past");
    require(marked("hostci", "1"),
            "the __Host- prefix must be matched case-insensitively (accepted when its rules are met)");
    require(marked("secureci", "0"),
            "the __Secure- prefix must be matched case-insensitively (rejected without Secure)");
}

void test_cookie_domain_rejected_on_ip_host()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html><body>
  <script>
    document.cookie = 'ipdomain=1; Domain=2.3.4';
    document.cookie = 'iphost=1';
    var c = document.cookie;
    var mark = function(name, needle){ document.body.setAttribute('data-' + name, c.indexOf(needle) >= 0 ? '1' : '0'); };
    mark('ipdomain', 'ipdomain=1');
    mark('iphost', 'iphost=1');
  </script>
</body></html>
)HTML", "http://1.2.3.4/start/index.html");

    const auto marked = [&](const char* name, const char* value) {
        return page.outer_html(std::string("body[data-") + name + "='" + value + "']").has_value();
    };
    require(marked("iphost", "1"), "a host-only cookie on an IP-literal host should be accepted");
    require(marked("ipdomain", "0"),
            "a Domain attribute that is not the IP host itself must be rejected so the cookie "
            "cannot leak across unrelated IP hosts (RFC 6265 §5.3)");
}

void test_public_suffix_list_covers_modern_hosting_platforms()
{
    auto loader = std::make_shared<RecordingResourceLoader>();
    loader->add(
        "https://a.vercel.app/set-super",
        "set-super",
        "text/plain",
        {{"Set-Cookie", "super=1; Domain=vercel.app; Path=/"}});
    loader->add("https://a.vercel.app/read", "read-a", "text/plain");
    loader->add(
        "https://b.vercel.app/set-strict",
        "set-strict",
        "text/plain",
        {{"Set-Cookie", "bStrict=1; Path=/; SameSite=Strict; Secure"}});
    loader->add("https://b.vercel.app/read", "read-b", "text/plain");

    pagecore::Page page;
    page.set_resource_loader(loader);
    page.load_html(R"HTML(
<html><body>
  <script>
    fetch('/set-super', { credentials: 'include' })
      .then(() => fetch('/read', { credentials: 'include' }))
      .then(() => fetch('https://b.vercel.app/set-strict', { credentials: 'include' }))
      .then(() => fetch('https://b.vercel.app/read', { credentials: 'include' }))
      .then(() => document.body.setAttribute('data-done', 'ok'));
  </script>
</body></html>
)HTML", "https://a.vercel.app/start/index.html");

    require(page.outer_html("body[data-done='ok']").has_value(),
            "public suffix list chain should complete");

    const auto* read_a = find_request(*loader, "https://a.vercel.app/read");
    require(read_a != nullptr && !header_contains(*read_a, "cookie", "super=1"),
            "Set-Cookie scoped to the public suffix vercel.app must be rejected (supercookie defense)");

    const auto* read_b = find_request(*loader, "https://b.vercel.app/read");
    require(read_b != nullptr && !header_contains(*read_b, "cookie", "bStrict=1"),
            "a.vercel.app and b.vercel.app are independent registrants under one private PSL suffix; "
            "a SameSite=Strict cookie set by b.vercel.app must not leak into a cross-site request from a.vercel.app");
}

void test_cookie_secure_not_overwritten_by_insecure()
{
    auto loader = std::make_shared<RecordingResourceLoader>();
    loader->add("https://example.test/secure-set", "s", "text/plain",
        {{"Set-Cookie", "sess=secure; Path=/; Secure"}});
    loader->add("http://example.test/attack", "a", "text/plain",
        {{"Set-Cookie", "sess=hijack; Path=/"}});
    loader->add("https://example.test/read", "r", "text/plain");

    pagecore::Page page;
    page.set_resource_loader(loader);
    page.load_html(R"HTML(
<html><body>
  <script>
    fetch('https://example.test/secure-set', { credentials: 'include' })
      .then(() => fetch('http://example.test/attack', { credentials: 'include' }))
      .then(() => fetch('https://example.test/read', { credentials: 'include' }))
      .then(() => document.body.setAttribute('data-done', 'ok'));
  </script>
</body></html>
)HTML", "https://example.test/start/index.html");

    require(page.outer_html("body[data-done='ok']").has_value(),
            "secure-cookie overwrite chain should complete");
    const auto* read = find_request(*loader, "https://example.test/read");
    require(read != nullptr && header_contains(*read, "cookie", "sess=secure"),
            "a Secure cookie set over HTTPS must survive an insecure overwrite attempt");
    require(read != nullptr && !header_contains(*read, "cookie", "sess=hijack"),
            "an insecure origin must not overwrite a Secure cookie of the same name");
}

void test_cookie_jar_growth_is_bounded()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html><body>
  <script>
    for (var i = 0; i < 80; i++) { document.cookie = 'k' + i + '=1'; }
    var parts = document.cookie ? document.cookie.split('; ') : [];
    var kcount = parts.filter(function(p){ return p.indexOf('k') === 0; }).length;
    document.body.setAttribute('data-kcount', String(kcount));
  </script>
</body></html>
)HTML", "https://cap.example/index.html");

    require(page.outer_html("body[data-kcount='50']").has_value(),
            "per-domain cookie count must be bounded to the cap (50) via oldest-first eviction");
}

void test_failed_external_script_does_not_abort_page()
{
    auto loader = std::make_shared<RecordingResourceLoader>();
    // https://dead.example/missing.js is intentionally NOT registered, so the
    // loader reports a transport failure (NotFound) for that external script.
    pagecore::Page page;
    page.set_resource_loader(loader);
    page.load_html(R"HTML(
<html><body>
  <script src="https://dead.example/missing.js"></script>
  <script>document.body.setAttribute('data-inline', 'ran');</script>
</body></html>
)HTML", "https://example.test/index.html");

    require(page.outer_html("body[data-inline='ran']").has_value(),
            "a failed external <script src> must not abort the page load or skip later inline scripts");
}

void test_nomodule_suppresses_only_classic_scripts()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html><body>
  <script nomodule>document.body.setAttribute('data-classic', 'ran');</script>
  <script type="module" nomodule>document.body.setAttribute('data-module', 'ran');</script>
</body></html>
)HTML", "https://example.test/index.html");

    require(!page.outer_html("body[data-classic='ran']").has_value(),
            "nomodule must suppress a classic script");
    require(page.outer_html("body[data-module='ran']").has_value(),
            "nomodule must NOT suppress a module script (per the HTML spec)");
}

void test_scripts_inside_template_do_not_execute()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html><body>
  <template><script>document.body.setAttribute('data-template-script', 'ran');</script></template>
  <script>document.body.setAttribute('data-top', 'ran');</script>
</body></html>
)HTML", "https://example.test/index.html");

    require(page.outer_html("body[data-top='ran']").has_value(), "top-level script should run");
    require(!page.outer_html("body[data-template-script='ran']").has_value(),
            "a script inside inert <template> content must not execute");
}

void test_page_readiness_wait_until_load_skips_timers()
{
    pagecore::LoadOptions options;
    options.wait_until = pagecore::WaitUntil::Load;
    options.wait_time = std::chrono::milliseconds(50);
    options.stable_window = std::chrono::milliseconds(5);

    pagecore::Page page(options);
    page.load_html(R"HTML(
<html><body>
  <script>
    setTimeout(() => document.body.setAttribute('data-timer', 'ran'), 0);
  </script>
</body></html>
)HTML");

    require(
        !page.outer_html("body[data-timer='ran']").has_value(),
        "wait-until=load should not run timer callbacks during load");

    const bool ready = page.run_until_ready(pagecore::PageReadinessOptions{
        pagecore::WaitUntil::Ready,
        std::chrono::milliseconds(50),
        std::chrono::milliseconds(5),
    });
    require(ready, "run_until_ready should complete after the pending timer drains");
    require(
        page.outer_html("body[data-timer='ran']").has_value(),
        "explicit run_until_ready should run pending relevant timer callbacks");
}

void test_page_readiness_ready_waits_for_timer_fetch_and_dom_stable()
{
    auto loader = std::make_shared<RecordingResourceLoader>();
    loader->add("https://example.test/data.json", R"JSON({"value":"ok"})JSON", "application/json");

    pagecore::LoadOptions options;
    options.wait_until = pagecore::WaitUntil::Ready;
    options.wait_time = std::chrono::milliseconds(50);
    options.stable_window = std::chrono::milliseconds(10);

    pagecore::Page page(options);
    page.set_resource_loader(loader);
    page.load_html(R"HTML(
<html><body>
  <script>
    setTimeout(() => {
      fetch('/data.json')
        .then((response) => response.json())
        .then((data) => document.body.setAttribute('data-ready-fetch', data.value));
    }, 0);
  </script>
</body></html>
)HTML", "https://example.test/index.html");

    require(
        page.outer_html("body[data-ready-fetch='ok']").has_value(),
        "wait-until=ready should wait for timer-scheduled fetch and the resulting DOM mutation");
    require(
        has_request_kind(*loader, "https://example.test/data.json", pagecore::ResourceKind::Other),
            "timer-scheduled fetch should load through ResourceLoader before readiness completes");
}

void test_page_readiness_dom_stable_runs_pending_page_tasks()
{
    auto loader = std::make_shared<RecordingResourceLoader>();
    loader->add("https://example.test/data.json", R"JSON({"value":"ok"})JSON", "application/json");

    pagecore::LoadOptions options;
    options.wait_until = pagecore::WaitUntil::Load;
    options.wait_time = std::chrono::milliseconds(0);

    pagecore::Page page(options);
    page.set_resource_loader(loader);
    page.load_html("<html><body></body></html>", "https://example.test/index.html");
    page.eval(R"JS(
      fetch('/data.json')
        .then((response) => response.json())
        .then((data) => document.body.setAttribute('data-dom-stable-fetch', data.value));
    )JS");

    const bool ready = page.run_until_ready(pagecore::PageReadinessOptions{
        pagecore::WaitUntil::DomStable,
        std::chrono::milliseconds(50),
        std::chrono::milliseconds(0),
    });

    require(ready, "wait-until=dom-stable should complete after pending page tasks run");
    require(
        page.outer_html("body[data-dom-stable-fetch='ok']").has_value(),
        "wait-until=dom-stable should not finish before a queued fetch task mutates the DOM");
}

void test_page_readiness_image_and_stylesheet_load_events()
{
    auto loader = std::make_shared<RecordingResourceLoader>();
    loader->add("https://example.test/pixel.png", "png-bytes", "image/png");
    loader->add("https://example.test/style.css", "body { color: rgb(1, 2, 3); }", "text/css");

    pagecore::LoadOptions options;
    options.wait_until = pagecore::WaitUntil::Ready;
    options.wait_time = std::chrono::milliseconds(50);
    options.stable_window = std::chrono::milliseconds(5);

    pagecore::Page page(options);
    page.set_resource_loader(loader);
    page.load_html(R"HTML(
<html><head></head><body>
  <script>
    const img = new Image();
    img.onload = () => document.body.setAttribute('data-img-load', img.complete ? 'ok' : 'bad');
    img.onerror = () => document.body.setAttribute('data-img-load', 'error');
    img.src = '/pixel.png';

    const link = document.createElement('link');
    link.rel = 'stylesheet';
    link.href = '/style.css';
    link.onload = () => document.body.setAttribute('data-link-load', 'ok');
    link.onerror = () => document.body.setAttribute('data-link-load', 'error');
    document.head.appendChild(link);
  </script>
</body></html>
)HTML", "https://example.test/index.html");

    require(
        page.outer_html("body[data-img-load='ok'][data-link-load='ok']").has_value(),
        "wait-until=ready should wait for image and stylesheet load events");
    require(
        has_request_kind(*loader, "https://example.test/pixel.png", pagecore::ResourceKind::Image),
        "image load lifecycle should request image resources");
    require(
        has_request_kind(*loader, "https://example.test/style.css", pagecore::ResourceKind::Stylesheet),
        "stylesheet load lifecycle should request stylesheet resources");
}

void test_js_resource_policy_block_all_keeps_parser_scripts()
{
    pagecore::LoadOptions options;
    options.wait_time = std::chrono::milliseconds(5);
    options.js_resource_load_policy = pagecore::JsResourceLoadPolicy::BlockAll;

    auto loader = std::make_shared<RecordingResourceLoader>();
    loader->add(
        "https://example.test/boot.js",
        R"JS(
document.body.setAttribute('data-parser-script', 'ran');

fetch('/api.json').then(
  () => document.body.setAttribute('data-fetch', 'loaded'),
  () => document.body.setAttribute('data-fetch', 'blocked')
);

const xhr = new XMLHttpRequest();
xhr.open('GET', '/xhr.json');
xhr.onload = () => document.body.setAttribute('data-xhr', 'loaded');
xhr.onerror = () => document.body.setAttribute('data-xhr', 'blocked');
xhr.send();

const script = document.createElement('script');
script.src = '/dynamic.js';
script.onload = () => document.body.setAttribute('data-dynamic-script', 'loaded');
script.onerror = () => document.body.setAttribute('data-dynamic-script', 'blocked');
document.body.appendChild(script);
)JS",
        "text/javascript");
    loader->add("https://example.test/api.json", "{}", "application/json");
    loader->add("https://example.test/xhr.json", "{}", "application/json");
    loader->add(
        "https://example.test/dynamic.js",
        "document.body.setAttribute('data-dynamic-script', 'loaded');",
        "text/javascript");

    pagecore::Page page(options);
    page.set_resource_loader(loader);
    page.load_html(
        "<html><body><script src='/boot.js'></script></body></html>",
        "https://example.test/index.html");

    require(
        page.outer_html("body[data-parser-script='ran']").has_value(),
        "parser-discovered scripts should still load when JS resource loads are blocked");
    require(page.outer_html("body[data-fetch='blocked']").has_value(), "fetch should reject when policy blocks JS loads");
    require(page.outer_html("body[data-xhr='blocked']").has_value(), "XHR should fire error when policy blocks JS loads");
    require(
        page.outer_html("body[data-dynamic-script='blocked']").has_value(),
        "dynamic external scripts should dispatch error when policy blocks JS loads");
    require(
        find_request(*loader, "https://example.test/boot.js") != nullptr,
        "parser script should reach the resource loader");
    require(
        find_request(*loader, "https://example.test/api.json") == nullptr
            && find_request(*loader, "https://example.test/xhr.json") == nullptr
            && find_request(*loader, "https://example.test/dynamic.js") == nullptr,
        "blocked JS-initiated loads should not reach the resource loader");
}

void test_js_resource_policy_same_origin_blocks_cross_origin_loads()
{
    pagecore::LoadOptions options;
    options.wait_time = std::chrono::milliseconds(5);
    options.js_resource_load_policy = pagecore::JsResourceLoadPolicy::SameOriginOnly;

    auto loader = std::make_shared<RecordingResourceLoader>();
    loader->add(
        "https://example.test/boot.js",
        R"JS(
fetch('/same.json').then(
  () => document.body.setAttribute('data-same-origin', 'loaded'),
  () => document.body.setAttribute('data-same-origin', 'blocked')
);
fetch('https://cdn.test/third.json').then(
  () => document.body.setAttribute('data-cross-origin', 'loaded'),
  () => document.body.setAttribute('data-cross-origin', 'blocked')
);
)JS",
        "text/javascript");
    loader->add("https://example.test/same.json", "{}", "application/json");
    loader->add("https://cdn.test/third.json", "{}", "application/json");

    pagecore::Page page(options);
    page.set_resource_loader(loader);
    page.load_html(
        "<html><body><script src='/boot.js'></script></body></html>",
        "https://example.test/index.html");

    require(
        page.outer_html("body[data-same-origin='loaded']").has_value(),
        "same-origin JS resource loads should be allowed");
    require(
        page.outer_html("body[data-cross-origin='blocked']").has_value(),
        "cross-origin JS resource loads should be blocked");
    require(
        find_request(*loader, "https://example.test/same.json") != nullptr,
        "same-origin JS resource load should reach the resource loader");
    require(
        find_request(*loader, "https://cdn.test/third.json") == nullptr,
        "cross-origin JS resource load should not reach the resource loader");
}

void test_js_resource_budgets_limit_count_bytes_and_time()
{
    {
        pagecore::LoadOptions options;
        options.max_js_resource_loads = 1;

        auto loader = std::make_shared<RecordingResourceLoader>();
        loader->add("https://example.test/a.json", "A", "application/json");
        loader->add("https://example.test/b.json", "B", "application/json");

        pagecore::Page page(options);
        page.set_resource_loader(loader);
        page.load_html(R"HTML(
<html><body>
  <script>
    fetch('/a.json').then(
      () => document.body.setAttribute('data-a', 'loaded'),
      () => document.body.setAttribute('data-a', 'blocked')
    );
    fetch('/b.json').then(
      () => document.body.setAttribute('data-b', 'loaded'),
      () => document.body.setAttribute('data-b', 'blocked')
    );
  </script>
</body></html>
)HTML", "https://example.test/index.html");

        require(page.outer_html("body[data-a='loaded'][data-b='blocked']").has_value(),
                "max_js_resource_loads should allow the first JS load and block the second");
        require(find_request(*loader, "https://example.test/a.json") != nullptr,
                "first JS resource load should reach the loader");
        require(find_request(*loader, "https://example.test/b.json") == nullptr,
                "JS resource loads blocked by count budget should not reach the loader");
    }

    {
        pagecore::LoadOptions options;
        options.max_js_resource_bytes = 2;

        auto loader = std::make_shared<RecordingResourceLoader>();
        loader->add("https://example.test/a.json", "OK", "application/json");
        loader->add("https://example.test/b.json", "B", "application/json");

        pagecore::Page page(options);
        page.set_resource_loader(loader);
        // The second fetch starts only after the first completes: byte budgets
        // are recorded at completion, so concurrently STARTED transfers are
        // bounded by the count budget and connection caps, not by bytes.
        page.load_html(R"HTML(
<html><body>
  <script>
    fetch('/a.json').then(
      () => document.body.setAttribute('data-a', 'loaded'),
      () => document.body.setAttribute('data-a', 'blocked')
    ).then(() => fetch('/b.json').then(
      () => document.body.setAttribute('data-b', 'loaded'),
      () => document.body.setAttribute('data-b', 'blocked')
    ));
  </script>
</body></html>
)HTML", "https://example.test/index.html");

        require(page.outer_html("body[data-a='loaded'][data-b='blocked']").has_value(),
                "max_js_resource_bytes should block later JS loads after the byte budget is spent");
        require(find_request(*loader, "https://example.test/a.json") != nullptr,
                "first JS resource load should reach the loader before byte budget is spent");
        require(find_request(*loader, "https://example.test/b.json") == nullptr,
                "JS resource loads blocked by byte budget should not reach the loader");
    }

    {
        std::vector<pagecore::PerfEvent> events;
        pagecore::LoadOptions options;
        options.max_js_resource_time = std::chrono::milliseconds(0);
        options.perf_trace = [&](const pagecore::PerfEvent& event) {
            events.push_back(event);
        };

        auto loader = std::make_shared<RecordingResourceLoader>();
        loader->add("https://example.test/a.json", "A", "application/json");

        pagecore::Page page(options);
        page.set_resource_loader(loader);
        page.load_html(R"HTML(
<html><body>
  <script>
    fetch('/a.json').then(
      () => document.body.setAttribute('data-a', 'loaded'),
      () => document.body.setAttribute('data-a', 'blocked')
    );
  </script>
</body></html>
)HTML", "https://example.test/index.html");

        require(page.outer_html("body[data-a='blocked']").has_value(),
                "max_js_resource_time=0 should block JS loads before they start");
        require(find_request(*loader, "https://example.test/a.json") == nullptr,
                "JS resource loads blocked by time budget should not reach the loader");
        const auto blocked = std::find_if(events.begin(), events.end(), [](const pagecore::PerfEvent& event) {
            return event.phase == pagecore::PerfPhase::ResourceLoad
                && event.name == "js_load_resource_blocked"
                && event.reason == "budget:max_js_resource_time_ms";
        });
        require(blocked != events.end(), "budget-blocked JS resource loads should report a trace reason");
    }
}

void test_xhr_event_handler_exceptions_are_reported()
{
    auto loader = std::make_shared<pagecore::MemoryResourceLoader>();
    loader->add("https://api.test/data.txt", "ok", "text/plain");

    pagecore::LoadOptions options;
    options.console_log = [](std::string_view, std::string_view) {};

    pagecore::Page page(options);
    page.set_resource_loader(loader);

    page.load_html(R"HTML(
<html><body>
  <script>
    const xhr = new XMLHttpRequest();
    xhr.open('GET', 'https://api.test/data.txt');
    xhr.addEventListener('load', () => {
      throw new Error('xhr listener boom');
    });
    xhr.addEventListener('load', () => {
      document.body.setAttribute('data-xhr-after-error', 'ok');
    });
    xhr.addEventListener('error', () => {
      document.body.setAttribute('data-xhr-error', 'network');
    });
    xhr.send();
  </script>
</body></html>
)HTML", "https://example.test/page.html");

    require(
        page.outer_html("body[data-xhr-after-error='ok']").has_value(),
        "XMLHttpRequest listener exceptions should be reported without aborting later listeners");
    require(
        !page.outer_html("body[data-xhr-error='network']").has_value(),
        "XMLHttpRequest listener exceptions should not be converted to network errors");
}

void test_non_javascript_script_types_are_not_executed()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html><body>
  <script type="speculationrules">
    {"prefetch":[{"source":"document","where":{"href_matches":"/*"}}]}
  </script>
  <script type="importmap">
    {"imports":{"x":"/x.js"}}
  </script>
  <script type="application/ld+json">
    {"@context":"https://schema.org"}
  </script>
  <script>
    document.body.setAttribute('data-js', 'ok');
  </script>
</body></html>
)HTML");

    require(page.outer_html("body[data-js='ok']").has_value(),
            "non-JavaScript script types should be skipped instead of parsed by QuickJS");
}

void test_static_classic_async_defer_scripts_follow_spec_order()
{
    auto loader = std::make_shared<RecordingResourceLoader>();
    loader->add(
        "https://example.test/async.js",
        "window.__staticOrder = (window.__staticOrder || '') + 'A';",
        "text/javascript");
    loader->add(
        "https://example.test/defer.js",
        "window.__staticOrder = (window.__staticOrder || '') + 'D';",
        "text/javascript");

    pagecore::Page page;
    page.set_resource_loader(loader);
    page.load_html(R"HTML(
<html><body>
  <script>window.__staticOrder = '1';</script>
  <script async src="/async.js"></script>
  <script defer src="/defer.js"></script>
  <script>
    window.__staticOrder += '4';
    document.addEventListener('DOMContentLoaded', () => {
      document.body.setAttribute('data-static-order-at-dcl', window.__staticOrder);
    });
  </script>
</body></html>
)HTML", "https://example.test/index.html");

    // Spec semantics: inline scripts run during parsing ('1','4'), the async
    // script runs when its (memory-loader) completion task lands, the deferred
    // script runs after parsing and before DOMContentLoaded. With the
    // deterministic task queue the async completion (queued first) precedes
    // the deferred execution: "14AD".
    require(page.eval("window.__staticOrder") == "14AD",
            "async runs off parse order; defer runs after parsing (spec order)");
    require(
        page.outer_html("body[data-static-order-at-dcl='14AD']").has_value(),
        "DOMContentLoaded should fire after deferred scripts");
    require(has_request_kind(*loader, "https://example.test/async.js", pagecore::ResourceKind::Script),
            "static async classic script should still load as a script resource");
    require(has_request_kind(*loader, "https://example.test/defer.js", pagecore::ResourceKind::Script),
            "static defer classic script should still load as a script resource");
}

void test_defer_scripts_run_in_order_before_dcl_and_async_holds_load()
{
    auto loader = std::make_shared<RecordingResourceLoader>();
    loader->add(
        "https://example.test/defer-one.js",
        "window.__deferOrder = (window.__deferOrder || '') + '1';"
        "window.__readyAtDefer1 = document.readyState;",
        "text/javascript");
    loader->add(
        "https://example.test/defer-two.js",
        "window.__deferOrder = (window.__deferOrder || '') + '2';",
        "text/javascript");
    loader->add(
        "https://example.test/async-late.js",
        "window.__asyncRan = document.readyState;",
        "text/javascript");

    pagecore::Page page;
    page.set_resource_loader(loader);
    page.load_html(R"HTML(
<html><body>
  <script defer src="/defer-one.js"></script>
  <script async src="/async-late.js"></script>
  <script defer src="/defer-two.js"></script>
  <script>
    window.__deferAtParse = window.__deferOrder || '';
    document.addEventListener('DOMContentLoaded', () => {
      document.body.setAttribute('data-defer-at-dcl', window.__deferOrder || '');
    });
    window.addEventListener('load', () => {
      document.body.setAttribute('data-async-at-load', window.__asyncRan || 'missing');
    });
  </script>
</body></html>
)HTML", "https://example.test/index.html");

    require(page.eval("window.__deferAtParse") == "",
            "deferred scripts must not run during parsing");
    require(page.eval("window.__deferOrder") == "12",
            "deferred scripts should run in document order");
    require(page.outer_html("body[data-defer-at-dcl='12']").has_value(),
            "DOMContentLoaded should fire after all deferred scripts ran");
    require(page.eval("window.__readyAtDefer1") == "loading",
            "deferred scripts should run before readyState leaves 'loading'");
    // The async script fetched+executed before the load event fired.
    const auto async_at_load = page.eval("document.body.getAttribute('data-async-at-load')");
    require(async_at_load == "loading" || async_at_load == "interactive",
            "the window load event should wait for parser-inserted async scripts");
}

void test_dynamic_script_insertion_executes_classic_scripts()
{
    auto loader = std::make_shared<RecordingResourceLoader>();
    loader->add(
        "https://example.test/masked.js",
        R"JS(
document.body.setAttribute('data-masked-static', 'bad');
)JS",
        "text/javascript");
    loader->add(
        "https://example.test/dynamic.js",
        R"JS(
window.__dynamicScriptOrder = (window.__dynamicScriptOrder || []);
window.__dynamicScriptOrder.push('body');
document.body.setAttribute('data-dynamic-external', document.currentScript.src);
)JS",
        "text/javascript");
    loader->add(
        "https://example.test/late-handler.js",
        R"JS(
document.body.setAttribute('data-late-handler-script', 'ok');
)JS",
        "text/javascript");
    loader->add(
        "https://example.test/ordered-one.js",
        "window.__dynamicOrdered = (window.__dynamicOrdered || '') + '1';",
        "text/javascript");
    loader->add(
        "https://example.test/ordered-two.js",
        "window.__dynamicOrdered = (window.__dynamicOrdered || '') + '2'; document.body.setAttribute('data-dynamic-ordered', window.__dynamicOrdered);",
        "text/javascript");

    pagecore::Page page;
    page.set_resource_loader(loader);
    page.load_html(R"HTML(
<html><body>
  <script src="/masked.js" type="loader-token-text/javascript"></script>
  <script type="loader-token-application/javascript">
    document.body.setAttribute('data-masked-inline', 'bad');
  </script>
  <script>
    const external = document.createElement('script');
    external.src = '/dynamic.js';
    external.onload = () => {
      window.__dynamicScriptOrder.push('load');
      document.body.setAttribute('data-dynamic-load', 'ok');
      document.body.setAttribute('data-dynamic-script-order', window.__dynamicScriptOrder.join(','));
    };
    document.head.appendChild(external);

    const lateHandler = document.createElement('script');
    lateHandler.src = '/late-handler.js';
    document.head.appendChild(lateHandler);
    lateHandler.onload = () => document.body.setAttribute('data-late-handler-load', 'ok');

    const inline = document.createElement('script');
    inline.text = "document.body.setAttribute('data-dynamic-inline', document.currentScript && document.currentScript.localName);";
    document.body.appendChild(inline);

    const orderedOne = document.createElement('script');
    document.body.setAttribute('data-dynamic-default-async', String(orderedOne.async));
    orderedOne.async = false;
    document.body.setAttribute('data-dynamic-explicit-async', String(orderedOne.async));
    orderedOne.src = '/ordered-one.js';
    document.head.appendChild(orderedOne);

    const orderedTwo = document.createElement('script');
    orderedTwo.async = false;
    orderedTwo.src = '/ordered-two.js';
    document.head.appendChild(orderedTwo);

    const holder = document.createElement('div');
    holder.innerHTML = '<script>document.body.setAttribute("data-innerhtml-script", "bad");</' + 'script>';
    document.body.appendChild(holder);
  </script>
</body></html>
)HTML", "https://example.test/index.html");

    require(
        page.outer_html("body[data-dynamic-external='https://example.test/dynamic.js'][data-dynamic-load='ok'][data-dynamic-inline='script'][data-late-handler-script='ok'][data-late-handler-load='ok']")
            .has_value(),
        "classic scripts inserted through DOM APIs should execute with currentScript and load events");
    require(
        page.outer_html("body[data-dynamic-script-order='body,load']").has_value(),
        "dynamic script load event should fire after the script body runs");
    require(
        !page.outer_html("body[data-masked-static='bad']").has_value(),
        "non-standard static script types should not be fetched or executed by PageCore itself");
    require(
        !page.outer_html("body[data-masked-inline='bad']").has_value(),
        "non-standard inline script types should not execute during static script collection");
    require(
        !has_request_kind(
            *loader,
            "https://example.test/masked.js",
            pagecore::ResourceKind::Script),
        "non-standard static external script types should not be requested before a loader opts in");
    require(
        has_request_kind(*loader, "https://example.test/dynamic.js", pagecore::ResourceKind::Script),
        "dynamic external scripts should load as script resources");
    require(
        has_request_kind(*loader, "https://example.test/late-handler.js", pagecore::ResourceKind::Script),
        "external dynamic scripts should start asynchronously enough for post-insertion onload handlers");
    require(
        page.outer_html("body[data-dynamic-default-async='true'][data-dynamic-explicit-async='false'][data-dynamic-ordered='12']")
            .has_value(),
        "DOM-created classic scripts should default to async and async=false scripts should execute in insertion order");
    require(
        has_request_kind(*loader, "https://example.test/ordered-one.js", pagecore::ResourceKind::Script)
            && has_request_kind(*loader, "https://example.test/ordered-two.js", pagecore::ResourceKind::Script),
        "ordered dynamic classic scripts should load as script resources");
    require(
        !page.outer_html("body[data-innerhtml-script='bad']").has_value(),
        "scripts created by innerHTML should remain inert when their container is inserted");
}

void test_dynamic_module_scripts_are_not_executed()
{
    auto loader = std::make_shared<RecordingResourceLoader>();
    loader->add(
        "https://example.test/dynamic-module.js",
        "document.body.setAttribute('data-dynamic-module-external', 'bad');",
        "text/javascript");

    pagecore::Page page;
    page.set_resource_loader(loader);
    page.load_html(R"HTML(
<html><body>
  <script>
    const externalModule = document.createElement('script');
    externalModule.type = 'module';
    externalModule.src = '/dynamic-module.js';
    externalModule.onload = () => document.body.setAttribute('data-dynamic-module-load', 'bad');
    externalModule.onerror = () => document.body.setAttribute('data-dynamic-module-error', 'bad');
    document.head.appendChild(externalModule);

    const inlineModule = document.createElement('script');
    inlineModule.type = 'module';
    inlineModule.text = "document.body.setAttribute('data-dynamic-module-inline', 'bad');";
    document.body.appendChild(inlineModule);
  </script>
</body></html>
)HTML", "https://example.test/index.html");

    require(!has_request_kind(*loader, "https://example.test/dynamic-module.js", pagecore::ResourceKind::Script),
            "dynamic module scripts should not be fetched by the classic dynamic script path");
    require(
        !page.outer_html("body[data-dynamic-module-external='bad']").has_value()
            && !page.outer_html("body[data-dynamic-module-inline='bad']").has_value()
            && !page.outer_html("body[data-dynamic-module-load='bad']").has_value()
            && !page.outer_html("body[data-dynamic-module-error='bad']").has_value(),
        "dynamic module scripts are intentionally not executed yet");
}

void test_resource_request_kind_and_cache()
{
    auto raw_loader = std::make_shared<RecordingResourceLoader>();
    raw_loader->add(
        "https://example.test/app.js",
        "document.body.setAttribute('data-script-kind', 'ok');",
        "text/javascript");

    auto cached_loader = std::make_shared<pagecore::CachingResourceLoader>(raw_loader);

    pagecore::Page page;
    page.set_resource_loader(cached_loader);
    page.load_html(R"HTML(
<html><body><script src="/app.js"></script></body></html>
)HTML", "https://example.test/index.html");

    auto body = page.outer_html("body[data-script-kind='ok']");
    require(body.has_value(), "external script should execute through cached loader");
    require(raw_loader->requests.size() == 1, "script should be loaded once through inner loader");
    require(raw_loader->requests[0].kind == pagecore::ResourceKind::Script, "script request should be kind=Script");
    require(raw_loader->requests[0].referrer == "https://example.test/index.html", "script request should carry referrer");

    auto response = cached_loader->load(pagecore::ResourceRequest{
        "https://example.test/app.js",
        pagecore::ResourceKind::Script,
        "https://example.test/index.html",
        "https://example.test/index.html",
    });
    require(response.from_cache, "second script load should be served from cache");
    require(raw_loader->requests.size() == 1, "cache hit should not call inner loader again");
}

void test_caching_resource_loader_is_lru()
{
    auto inner = std::make_shared<RecordingResourceLoader>();
    inner->add("https://example.test/a", "A", "text/plain");
    inner->add("https://example.test/b", "B", "text/plain");
    inner->add("https://example.test/c", "C", "text/plain");

    pagecore::CachingResourceLoader cache(inner, 2);
    const auto get = [&](const char* url) {
        return cache.load(pagecore::ResourceRequest{url, pagecore::ResourceKind::Other});
    };

    (void) get("https://example.test/a"); // miss -> {a}
    (void) get("https://example.test/b"); // miss -> {a, b}
    (void) get("https://example.test/a"); // hit  -> a becomes most-recently-used
    (void) get("https://example.test/c"); // miss -> evicts least-recently-used (b), keeps {a, c}

    const auto a_again = get("https://example.test/a");
    require(a_again.from_cache,
            "a recently-used entry must survive eviction (LRU, not FIFO): 'a' was touched after insertion");

    const auto b_again = get("https://example.test/b");
    require(!b_again.from_cache,
            "the least-recently-used entry ('b') must have been the one evicted when 'c' was inserted");
}

void test_resource_url_resolution_normalizes_dot_segments()
{
    require(
        pagecore::resolve_url("https://example.test/app/page.html", "./dep.js")
            == "https://example.test/app/dep.js",
        "relative URL resolution should remove current-directory segments");
    require(
        pagecore::resolve_url("https://example.test/app/nested/page.html", "../asset.css")
            == "https://example.test/app/asset.css",
        "relative URL resolution should remove parent-directory segments");
    require(
        pagecore::resolve_url("https://example.test", "style.css")
            == "https://example.test/style.css",
        "relative URL resolution should handle origin URLs without a trailing slash");
    require(
        pagecore::resolve_url("https://example.test/app/page.html", "/styles/../main.css")
            == "https://example.test/main.css",
        "absolute-path URL resolution should normalize dot segments");
    require(
        pagecore::resolve_url("https://example.test/app/page.html", "//cdn.example.test/assets/pixel.png")
            == "https://cdn.example.test/assets/pixel.png",
        "protocol-relative URL resolution should inherit the base scheme");
    require(
        pagecore::resolve_url("https://example.test/app/page.html", "cdn.example.test/assets/pixel.png")
            == "https://cdn.example.test/assets/pixel.png",
        "host-like URL resolution should avoid treating the hostname as a relative path segment");
    require(
        pagecore::resolve_url("https://example.test/app/page.html", "data:image/svg+xml,%3Csvg%3E%3C/svg%3E")
            == "data:image/svg+xml,%3Csvg%3E%3C/svg%3E",
        "opaque data URLs should not be path-normalized");
}

void test_resource_loader_decodes_data_urls()
{
    pagecore::CurlResourceLoader curl_loader("pagecore-test");
    const auto svg = curl_loader.load(pagecore::ResourceRequest{
        "data:image/svg+xml,%3Csvg%3E%3C/svg%3E",
        pagecore::ResourceKind::Image,
    });
    require(svg.status == 200, "data URL should load with a successful status");
    require(svg.mime_type == "image/svg+xml", "data URL should expose the media type");
    require(svg.body == "<svg></svg>", "data URL should percent-decode its payload");

    pagecore::MemoryResourceLoader memory_loader;
    const auto png = memory_loader.load(pagecore::ResourceRequest{
        "data:image/png;base64,QUJDRA==",
        pagecore::ResourceKind::Image,
    });
    require(png.status == 200, "memory loader should handle inline data URLs directly");
    require(png.mime_type == "image/png", "base64 data URL should expose the image media type");
    require(png.body == "ABCD", "base64 data URL should decode its payload");

    const auto relaxed = memory_loader.load(pagecore::ResourceRequest{
        "data:text/plain;base64,QU JD\nRA",
        pagecore::ResourceKind::Other,
    });
    require(relaxed.body == "ABCD", "base64 data URL decode should allow whitespace and missing padding");

    pagecore::ResourcePolicy tiny;
    tiny.max_response_bytes = 3;
    pagecore::MemoryResourceLoader tiny_loader(tiny);
    require_resource_error(
        pagecore::ResourceErrorCode::TooLarge,
        [&] {
            (void) tiny_loader.load(pagecore::ResourceRequest{
                "data:text/plain;base64,QUJDRA==",
                pagecore::ResourceKind::Other,
            });
        },
        "base64 data URL decode should preserve max_response_bytes enforcement");
}

void test_resource_policy_errors()
{
    pagecore::ResourcePolicy no_network;
    no_network.allow_network = false;
    pagecore::CurlResourceLoader curl_loader("pagecore-test", no_network);
    require_resource_error(
        pagecore::ResourceErrorCode::NetworkDisabled,
        [&] {
            (void) curl_loader.load(pagecore::ResourceRequest{
                "https://example.test/app.js",
                pagecore::ResourceKind::Script,
            });
        },
        "network policy should reject HTTP requests before transport");

    pagecore::ResourcePolicy same_origin;
    same_origin.same_origin_only = true;
    pagecore::MemoryResourceLoader same_origin_loader(same_origin);
    same_origin_loader.add("https://cdn.test/app.js", "x", "text/javascript");
    require_resource_error(
        pagecore::ResourceErrorCode::SameOriginViolation,
        [&] {
            (void) same_origin_loader.load(pagecore::ResourceRequest{
                "https://cdn.test/app.js",
                pagecore::ResourceKind::Script,
                "https://example.test/index.html",
                "https://example.test/index.html",
            });
        },
        "same-origin policy should reject cross-origin resources");

    pagecore::ResourcePolicy tiny;
    tiny.max_response_bytes = 3;
    pagecore::MemoryResourceLoader tiny_loader(tiny);
    tiny_loader.add("https://example.test/big.css", "1234", "text/css");
    require_resource_error(
        pagecore::ResourceErrorCode::TooLarge,
        [&] {
            (void) tiny_loader.load(pagecore::ResourceRequest{
                "https://example.test/big.css",
                pagecore::ResourceKind::Stylesheet,
            });
        },
        "max response size should reject oversized resources");
}

void test_resource_policy_blocks_private_hosts()
{
    pagecore::CurlResourceLoader loader("pagecore-test"); // default: block_private_hosts = true
    const char* blocked_urls[] = {
        "http://127.0.0.1/x",
        "http://localhost/x",
        "http://169.254.169.254/latest/meta-data/",
        "http://[::1]/x",
        "http://10.0.0.1/x",
        "http://192.168.1.1/x",
        "http://172.16.0.1/x",
        // Legacy numeric IPv4 encodings that resolve to 127.0.0.1 and must be
        // rejected by the literal pre-flight (SSRF bypass, portable to Windows).
        "http://2130706433/",     // decimal 127.0.0.1
        "http://0x7f000001/",     // hex 127.0.0.1
        "http://0177.0.0.1/",     // octal-leading 127.0.0.1
        "http://127.1/",          // short form 127.0.0.1
        // FQDN trailing-dot forms resolve identically and must not slip past.
        "http://localhost./x",
        "http://127.0.0.1./x",
        // IPv6 encodings that embed a private/loopback IPv4 address.
        "http://[::127.0.0.1]/",       // IPv4-compatible ::127.0.0.1
        "http://[2002:7f00:1::]/",     // 6to4 embedding 127.0.0.1
        "http://[64:ff9b::7f00:1]/",   // NAT64 well-known prefix embedding 127.0.0.1
    };
    for (const char* url : blocked_urls) {
        require_resource_error(
            pagecore::ResourceErrorCode::BlockedHost,
            [&] { (void) loader.load(pagecore::ResourceRequest{url, pagecore::ResourceKind::Script}); },
            std::string("private/loopback/link-local host should be blocked: ") + url);
    }

    pagecore::ResourcePolicy custom;
    custom.block_private_hosts = false;
    custom.blocked_hosts = {"evil.test"};
    pagecore::CurlResourceLoader custom_loader("pagecore-test", custom);
    require_resource_error(
        pagecore::ResourceErrorCode::BlockedHost,
        [&] {
            (void) custom_loader.load(pagecore::ResourceRequest{
                "http://evil.test/x", pagecore::ResourceKind::Script});
        },
        "explicitly blocked host should be rejected");
}

void test_resource_scheme_not_allowed()
{
    pagecore::CurlResourceLoader loader("pagecore-test");
    require_resource_error(
        pagecore::ResourceErrorCode::SchemeNotAllowed,
        [&] {
            (void) loader.load(pagecore::ResourceRequest{
                "ftp://example.test/x", pagecore::ResourceKind::Other});
        },
        "non-allowlisted scheme should be rejected");
}

void test_resource_file_sandbox()
{
    const std::filesystem::path root = std::filesystem::path(PAGECORE_BINARY_DIR) / "pagecore_sandbox_test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "sub");
    const std::filesystem::path inside = root / "sub" / "inside.txt";
    const std::filesystem::path outside = std::filesystem::path(PAGECORE_BINARY_DIR) / "pagecore_sandbox_outside.txt";
    {
        std::ofstream(inside, std::ios::binary) << "INSIDE";
        std::ofstream(outside, std::ios::binary) << "OUTSIDE";
    }

    pagecore::ResourcePolicy policy;
    policy.file_root = root.string();
    pagecore::CurlResourceLoader loader("pagecore-test", policy);

    const auto ok = loader.load(pagecore::ResourceRequest{"file://" + inside.string(), pagecore::ResourceKind::Other});
    require(ok.body == "INSIDE", "file inside sandbox root should be readable");

    require_resource_error(
        pagecore::ResourceErrorCode::FileDisabled,
        [&] {
            (void) loader.load(pagecore::ResourceRequest{
                "file://" + outside.string(), pagecore::ResourceKind::Other});
        },
        "file outside sandbox root should be rejected");

    require_resource_error(
        pagecore::ResourceErrorCode::FileDisabled,
        [&] {
            (void) loader.load(pagecore::ResourceRequest{
                "file://" + (root / "sub" / ".." / ".." / "pagecore_sandbox_outside.txt").string(),
                pagecore::ResourceKind::Other});
        },
        "traversal out of sandbox root should be rejected");

    require_resource_error(
        pagecore::ResourceErrorCode::NotFound,
        [&] {
            (void) loader.load(pagecore::ResourceRequest{
                "file://" + root.string(), pagecore::ResourceKind::Other});
        },
        "non-regular file (directory) should be rejected");

    pagecore::ResourcePolicy tiny = policy;
    tiny.max_response_bytes = 3;
    pagecore::CurlResourceLoader tiny_loader("pagecore-test", tiny);
    require_resource_error(
        pagecore::ResourceErrorCode::TooLarge,
        [&] {
            (void) tiny_loader.load(pagecore::ResourceRequest{
                "file://" + inside.string(), pagecore::ResourceKind::Other});
        },
        "file exceeding max_response_bytes should be rejected before slurping");

    std::filesystem::remove_all(root);
    std::filesystem::remove(outside);
}

void test_resource_relative_file_url()
{
    // Regression: "file://sub/f.txt" must resolve to the relative path "sub/f.txt"
    // (PageCore builds file URLs as file:// + path), not be misread as host=sub
    // yielding "/f.txt".
    const std::filesystem::path saved = std::filesystem::current_path();
    const std::filesystem::path root = std::filesystem::path(PAGECORE_BINARY_DIR) / "pagecore_relfile_test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "sub");
    std::ofstream(root / "sub" / "f.txt", std::ios::binary) << "REL";

    std::filesystem::current_path(root);
    try {
        pagecore::CurlResourceLoader loader("pagecore-test");
        const auto resp = loader.load(pagecore::ResourceRequest{
            "file://sub/f.txt", pagecore::ResourceKind::Other});
        require(resp.body == "REL",
                "relative file:// path must resolve relative to the working directory");
    } catch (...) {
        std::filesystem::current_path(saved);
        std::filesystem::remove_all(root);
        throw;
    }
    std::filesystem::current_path(saved);
    std::filesystem::remove_all(root);
}

void test_resource_blocks_file_from_network_origin()
{
    const std::filesystem::path file = std::filesystem::path(PAGECORE_BINARY_DIR) / "pagecore_local_secret.txt";
    std::ofstream(file, std::ios::binary) << "SECRET";
    const std::string url = "file://" + file.string();

    pagecore::CurlResourceLoader loader("pagecore-test"); // allow_file_from_network = false
    require_resource_error(
        pagecore::ResourceErrorCode::FileDisabled,
        [&] {
            (void) loader.load(pagecore::ResourceRequest{
                url, pagecore::ResourceKind::Other, "https://evil.test/page.html", "https://evil.test/page.html"});
        },
        "remote page must not read local files via file://");

    const auto top_level = loader.load(pagecore::ResourceRequest{url, pagecore::ResourceKind::Document});
    require(top_level.body == "SECRET", "top-level file:// load (no network initiator) should succeed");

    pagecore::ResourcePolicy permissive;
    permissive.allow_file_from_network = true;
    pagecore::CurlResourceLoader permissive_loader("pagecore-test", permissive);
    const auto allowed = permissive_loader.load(pagecore::ResourceRequest{
        url, pagecore::ResourceKind::Other, "https://evil.test/page.html", "https://evil.test/page.html"});
    require(allowed.body == "SECRET", "allow_file_from_network should re-enable file:// from network origin");

    std::filesystem::remove(file);
}

#if !defined(_WIN32)
void test_curl_loader_preserves_set_cookie_headers_across_redirects()
{
    const BoundTestServer bound = bind_loopback_test_server(2, "redirect cookie");
    const int server_fd = bound.fd;
    const int port = bound.port;

    std::atomic<int> accepted{0};
    std::thread server([server_fd, port, &accepted] {
        for (int i = 0; i < 2; ++i) {
            const int client = ::accept(server_fd, nullptr, nullptr);
            if (client < 0) {
                continue;
            }
            ++accepted;
            char buffer[2048];
            (void) ::recv(client, buffer, sizeof(buffer), 0);
            std::string response;
            if (i == 0) {
                response =
                    "HTTP/1.1 302 Found\r\n"
                    "Location: http://127.0.0.1:" + std::to_string(port) + "/final\r\n"
                    "Set-Cookie: hop=redirect; Path=/\r\n"
                    "Content-Length: 0\r\n"
                    "Connection: close\r\n"
                    "\r\n";
            } else {
                response =
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: text/plain\r\n"
                    "Set-Cookie: final=ok; Path=/\r\n"
                    "Content-Length: 2\r\n"
                    "Connection: close\r\n"
                    "\r\n"
                    "ok";
            }
            (void) ::send(client, response.data(), response.size(), 0);
            ::close(client);
        }
        ::close(server_fd);
    });

    try {
        // block_private_hosts is disabled here only so the loopback fixture is
        // reachable at all. The connect-time socket guard's redirect-to-internal /
        // DNS-rebinding defense cannot be exercised hermetically over loopback: the
        // first hop would itself have to be 127.0.0.1 (rejected by the literal
        // pre-flight before any redirect) and CI has no non-private first hop that
        // could 302 to an internal address. The literal-IP arm of that guard is
        // covered directly by test_resource_policy_blocks_private_hosts (including
        // IPv6-embedded and trailing-dot forms); the post-DNS redirect arm relies on
        // the same is_blocked_sockaddr check and is not asserted here.
        pagecore::ResourcePolicy policy;
        policy.block_private_hosts = false;
        pagecore::CurlResourceLoader loader("pagecore-test", policy);
        const auto response = loader.load(pagecore::ResourceRequest{
            "http://127.0.0.1:" + std::to_string(port) + "/start",
            pagecore::ResourceKind::Document,
        });

        require(response.status == 200 && response.body == "ok", "redirect cookie fixture should return final response");
        require(response.redirect_count == 1, "redirect count should report followed redirects");
        require(response.url == "http://127.0.0.1:" + std::to_string(port) + "/final",
                "final response URL should be exposed after redirect");

        bool saw_redirect_cookie = false;
        bool saw_final_cookie = false;
        for (const auto& [name, value] : response.headers) {
            if (header_name_equals(name, "set-cookie") && value.find("hop=redirect") != std::string::npos) {
                saw_redirect_cookie = true;
            }
            if (header_name_equals(name, "set-cookie") && value.find("final=ok") != std::string::npos) {
                saw_final_cookie = true;
            }
        }
        require(saw_redirect_cookie && saw_final_cookie,
                "ResourceResponse headers should preserve Set-Cookie from redirect and final responses");
        require(response.set_cookie_headers.size() == 2,
                "ResourceResponse should preserve hop-aware Set-Cookie metadata across redirects");
        require(response.set_cookie_headers[0].first == "http://127.0.0.1:" + std::to_string(port) + "/start",
                "redirect Set-Cookie should be associated with the redirect response URL");
        require(response.set_cookie_headers[1].first == "http://127.0.0.1:" + std::to_string(port) + "/final",
                "final Set-Cookie should be associated with the final response URL");
    } catch (...) {
        if (server.joinable()) {
            server.join();
        }
        throw;
    }

    if (server.joinable()) {
        server.join();
    }
    require(accepted == 2, "redirect cookie fixture should serve redirect and final requests");
}

void test_curl_loader_rejects_oversized_response_headers()
{
    // Regression for the response-header size cap: the body path is bounded by
    // max_response_bytes, but without a header cap a server can move an unbounded
    // payload into response headers and exhaust memory. The load must be rejected
    // rather than accumulate the headers. ~1.5 MiB of header lines is sent; the
    // protection is enforced by our cumulative header cap (the authoritative bound
    // for Set-Cookie accumulated across redirect hops, which libcurl resets per
    // hop) and/or by libcurl's own per-response header limit — either way the
    // oversized response must not succeed.
    const BoundTestServer bound = bind_loopback_test_server(2, "oversized headers");
    const int server_fd = bound.fd;
    const int port = bound.port;

    std::thread server([server_fd] {
        const int client = ::accept(server_fd, nullptr, nullptr);
        if (client >= 0) {
            char buffer[2048];
            (void) ::recv(client, buffer, sizeof(buffer), 0);
            std::string response = "HTTP/1.1 200 OK\r\n";
            const std::string pad(200, 'x');
            for (int i = 0; i < 7000; ++i) {
                response += "X-Fill-" + std::to_string(i) + ": " + pad + "\r\n";
            }
            response += "Content-Length: 2\r\n\r\nok";
            // Flush until all bytes are sent or curl aborts the transfer and closes
            // the socket (send() then fails), so enough header bytes reach the cap.
            std::size_t sent = 0;
            while (sent < response.size()) {
                const ssize_t n = ::send(client, response.data() + sent, response.size() - sent, 0);
                if (n <= 0) {
                    break;
                }
                sent += static_cast<std::size_t>(n);
            }
            ::close(client);
        }
        ::close(server_fd);
    });

    try {
        pagecore::ResourcePolicy policy;
        policy.block_private_hosts = false;
        pagecore::CurlResourceLoader loader("pagecore-test", policy);
        bool rejected = false;
        try {
            (void) loader.load(pagecore::ResourceRequest{
                "http://127.0.0.1:" + std::to_string(port) + "/big-headers",
                pagecore::ResourceKind::Document});
        } catch (const pagecore::ResourceError&) {
            rejected = true;
        }
        require(rejected, "a response whose headers exceed the size cap must be rejected");
    } catch (...) {
        if (server.joinable()) {
            server.join();
        }
        throw;
    }
    if (server.joinable()) {
        server.join();
    }
}

void test_curl_loader_rejects_cross_scheme_redirect()
{
    // A 3xx redirect to a disallowed scheme (file://) must not be followed: the
    // request/redirect protocol set is pinned to http(s), closing a redirect-to-
    // file SSRF/local-read. The load must not yield the local file's contents.
    const BoundTestServer bound = bind_loopback_test_server(2, "cross-scheme redirect");
    const int server_fd = bound.fd;
    const int port = bound.port;

    std::thread server([server_fd] {
        const int client = ::accept(server_fd, nullptr, nullptr);
        if (client >= 0) {
            char buffer[2048];
            (void) ::recv(client, buffer, sizeof(buffer), 0);
            const std::string response =
                "HTTP/1.1 302 Found\r\n"
                "Location: file:///etc/passwd\r\n"
                "Content-Length: 0\r\n"
                "Connection: close\r\n"
                "\r\n";
            (void) ::send(client, response.data(), response.size(), 0);
            ::close(client);
        }
        ::close(server_fd);
    });

    try {
        pagecore::ResourcePolicy policy;
        policy.block_private_hosts = false;
        pagecore::CurlResourceLoader loader("pagecore-test", policy);
        bool threw = false;
        std::string body;
        std::string url;
        try {
            const auto response = loader.load(pagecore::ResourceRequest{
                "http://127.0.0.1:" + std::to_string(port) + "/redir",
                pagecore::ResourceKind::Document});
            body = response.body;
            url = response.url;
        } catch (const pagecore::ResourceError&) {
            threw = true;
        }
        require(threw || (url.rfind("file:", 0) != 0 && body.find("root:") == std::string::npos),
                "redirect to a disallowed scheme (file://) must not be followed");
    } catch (...) {
        if (server.joinable()) {
            server.join();
        }
        throw;
    }
    if (server.joinable()) {
        server.join();
    }
}

void test_curl_loader_sends_request_cookie_across_same_host_redirect()
{
    // Regression for the cross-origin cookie leak fix: an attached Cookie header is
    // routed through libcurl's cookie engine (host-scoped), so it must still reach
    // the same host on a redirect hop. The engine also prevents the cookie from
    // being replayed to a *different* host, but that path cannot be exercised over
    // loopback (both hops share host 127.0.0.1), so it is covered by the engine's
    // documented host-scoping rather than asserted here.
    const BoundTestServer bound = bind_loopback_test_server(2, "request cookie redirect");
    const int server_fd = bound.fd;
    const int port = bound.port;

    std::vector<std::string> captured;
    std::mutex captured_mutex;
    std::thread server([server_fd, port, &captured, &captured_mutex] {
        for (int i = 0; i < 2; ++i) {
            const int client = ::accept(server_fd, nullptr, nullptr);
            if (client < 0) {
                continue;
            }
            char buffer[2048];
            const long received = ::recv(client, buffer, sizeof(buffer) - 1, 0);
            {
                std::lock_guard<std::mutex> lock(captured_mutex);
                captured.emplace_back(buffer, received > 0 ? static_cast<std::size_t>(received) : 0);
            }
            std::string response;
            if (i == 0) {
                response =
                    "HTTP/1.1 302 Found\r\n"
                    "Location: http://127.0.0.1:" + std::to_string(port) + "/final\r\n"
                    "Content-Length: 0\r\n"
                    "Connection: close\r\n\r\n";
            } else {
                response =
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: text/plain\r\n"
                    "Content-Length: 2\r\n"
                    "Connection: close\r\n\r\nok";
            }
            (void) ::send(client, response.data(), response.size(), 0);
            ::close(client);
        }
        ::close(server_fd);
    });

    try {
        pagecore::ResourcePolicy policy;
        policy.block_private_hosts = false;
        pagecore::CurlResourceLoader loader("pagecore-test", policy);
        pagecore::ResourceRequest request{
            "http://127.0.0.1:" + std::to_string(port) + "/start",
            pagecore::ResourceKind::Document};
        request.headers.emplace_back("Cookie", "sid=secret");
        const auto response = loader.load(request);
        require(response.status == 200 && response.body == "ok", "same-host redirect should complete");
    } catch (...) {
        if (server.joinable()) {
            server.join();
        }
        throw;
    }
    if (server.joinable()) {
        server.join();
    }

    require(captured.size() == 2, "server should receive both the initial and redirected request");
    require(captured[0].find("Cookie: sid=secret") != std::string::npos,
            "the attached cookie should be sent on the initial request");
    require(captured[1].find("Cookie: sid=secret") != std::string::npos,
            "the attached cookie should still be sent to the same host on the redirect hop");
}

void test_curl_loader_sends_user_agent_and_sanitized_referer_on_network_paths()
{
    const BoundTestServer bound = bind_loopback_test_server(6, "header capture");
    const int server_fd = bound.fd;
    const int port = bound.port;

    std::vector<std::string> received_requests;
    std::thread server([server_fd, &received_requests] {
        for (int i = 0; i < 6; ++i) {
            const int client = ::accept(server_fd, nullptr, nullptr);
            if (client < 0) {
                continue;
            }
            std::string request;
            std::array<char, 2048> buffer{};
            while (request.find("\r\n\r\n") == std::string::npos) {
                const ssize_t n = ::recv(client, buffer.data(), buffer.size(), 0);
                if (n <= 0) {
                    break;
                }
                request.append(buffer.data(), static_cast<std::size_t>(n));
            }
            received_requests.push_back(std::move(request));
            const std::string response =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/plain\r\n"
                "Content-Length: 2\r\n"
                "Connection: close\r\n"
                "\r\n"
                "ok";
            (void) ::send(client, response.data(), response.size(), 0);
            ::close(client);
        }
        ::close(server_fd);
    });

    try {
        pagecore::ResourcePolicy policy;
        policy.block_private_hosts = false;
        pagecore::CurlResourceLoader loader("pagecore-agent-test", policy);
        const std::string origin = "https://origin.test/page.html";
        const std::string base = "http://127.0.0.1:" + std::to_string(port);

        const auto single = loader.load(pagecore::ResourceRequest{
            base + "/single",
            pagecore::ResourceKind::Other,
            "https://user:pass@origin.test/page.html?x=1#access_token=secret",
            origin,
        });
        require(single.status == 200 && single.body == "ok", "serial curl request should complete");

        const auto batch = loader.load_all({
            pagecore::ResourceRequest{
                base + "/batch-a",
                pagecore::ResourceKind::Script,
                "file:///Users/test/local.html#secret",
                origin},
            pagecore::ResourceRequest{
                base + "/batch-b",
                pagecore::ResourceKind::Image,
                "http://origin.test/asset.html#fragment",
                origin},
            pagecore::ResourceRequest{
                base + "/batch-c",
                pagecore::ResourceKind::Other,
                "https://origin.test/path\nInjected: bad",
                origin},
            pagecore::ResourceRequest{
                base + "/batch-d",
                pagecore::ResourceKind::Other,
                origin,
                origin,
                "GET",
                {},
                {{"Referer", "file:///tmp/local.html#secret"}}},
            pagecore::ResourceRequest{
                base + "/batch-e",
                pagecore::ResourceKind::Other,
                origin,
                origin,
                "GET",
                {},
                {{"Referer", "https://user:pass@explicit.test/page.html#token"}}},
        });
        require(batch.size() == 5
                    && batch[0].status == 200
                    && batch[1].status == 200
                    && batch[2].status == 200
                    && batch[3].status == 200
                    && batch[4].status == 200,
                "multi curl requests should complete");
    } catch (...) {
        if (server.joinable()) {
            server.join();
        }
        throw;
    }

    if (server.joinable()) {
        server.join();
    }

    auto trim = [](std::string_view value) {
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
            value.remove_prefix(1);
        }
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
            value.remove_suffix(1);
        }
        return std::string(value);
    };
    auto raw_header = [&](std::string_view request, std::string_view name) {
        std::size_t line_start = 0;
        while (line_start < request.size()) {
            const std::size_t line_end = request.find('\n', line_start);
            std::string_view line = request.substr(
                line_start,
                line_end == std::string_view::npos ? std::string_view::npos : line_end - line_start);
            while (!line.empty() && line.back() == '\r') {
                line.remove_suffix(1);
            }
            if (line.empty()) {
                break;
            }
            const std::size_t colon = line.find(':');
            if (colon != std::string_view::npos && header_name_equals(trim(line.substr(0, colon)), name)) {
                return trim(line.substr(colon + 1));
            }
            if (line_end == std::string_view::npos) {
                break;
            }
            line_start = line_end + 1;
        }
        return std::string{};
    };
    auto request_target = [](std::string_view request) {
        const std::size_t method_end = request.find(' ');
        if (method_end == std::string_view::npos) {
            return std::string{};
        }
        const std::size_t target_end = request.find(' ', method_end + 1);
        if (target_end == std::string_view::npos) {
            return std::string{};
        }
        return std::string(request.substr(method_end + 1, target_end - method_end - 1));
    };

    require(received_requests.size() == 6, "header capture fixture should receive serial and batch requests");
    for (const auto& request : received_requests) {
        require(raw_header(request, "User-Agent") == "pagecore-agent-test",
                "CurlResourceLoader should send configured User-Agent on network requests");
        const std::string target = request_target(request);
        const std::string referer = raw_header(request, "Referer");
        if (target == "/single") {
            require(referer == "https://origin.test/",
                    "cross-origin HTTP Referer should be downgraded to origin-only (strip userinfo/path/query/fragment)");
        } else if (target == "/batch-a") {
            require(referer.empty(), "file:// referrers should not be sent to network resources");
        } else if (target == "/batch-b") {
            require(referer == "http://origin.test/",
                    "cross-origin HTTP Referer should be downgraded to origin-only in batch requests");
        } else if (target == "/batch-c") {
            require(referer.empty(), "control characters should suppress HTTP Referer emission");
        } else if (target == "/batch-d") {
            require(referer.empty(), "explicit file:// Referer headers should not be emitted");
        } else if (target == "/batch-e") {
            require(referer == "https://explicit.test/",
                    "explicit cross-origin Referer headers should be downgraded to origin-only");
        } else {
            require(false, "header capture fixture should only receive known paths");
        }
    }
}
#endif

// Pumps the event loop (uv + one task per turn) until `done` holds or the
// wall-clock budget expires. Returns whether `done` was reached.
bool pump_event_loop_until(
    pagecore::EventLoop& loop,
    const std::function<bool()>& done,
    std::chrono::milliseconds budget)
{
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (!done()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        loop.poll();
        if (loop.run_one_task()) {
            continue;
        }
        loop.wait_for_activity(std::chrono::milliseconds(20));
    }
    return true;
}

void test_async_loader_data_and_file_urls()
{
    pagecore::EventLoop loop;
    pagecore::ResourcePolicy policy;
    auto loader = std::make_shared<pagecore::CurlResourceLoader>("pagecore-async-test", policy);
    pagecore::CurlMultiAsyncLoader async_loader(loop, loader, "pagecore-async-test");

    std::optional<pagecore::AsyncLoadResult> data_result;
    async_loader.start(
        pagecore::ResourceRequest{"data:text/plain,hello-async"},
        [&](pagecore::AsyncLoadResult result) { data_result = std::move(result); });
    require(!data_result.has_value(),
            "async data: completion must be delivered as a queued task, not re-entrantly");

    const std::string file_path = std::string(PAGECORE_BINARY_DIR) + "/async_loader_test.txt";
    {
        std::ofstream out(file_path, std::ios::binary | std::ios::trunc);
        out << "file-async-body";
    }
    std::optional<pagecore::AsyncLoadResult> file_result;
    async_loader.start(
        pagecore::ResourceRequest{"file://" + file_path},
        [&](pagecore::AsyncLoadResult result) { file_result = std::move(result); });

    std::optional<pagecore::AsyncLoadResult> error_result;
    async_loader.start(
        pagecore::ResourceRequest{"file://" + file_path + ".does-not-exist"},
        [&](pagecore::AsyncLoadResult result) { error_result = std::move(result); });

    require(
        pump_event_loop_until(
            loop,
            [&] { return data_result && file_result && error_result; },
            std::chrono::milliseconds(2000)),
        "async data:/file: loads should complete promptly");

    require(!data_result->error && data_result->response.body == "hello-async"
                && data_result->response.mime_type == "text/plain",
            "async data: URL should decode through the async engine");
    require(!file_result->error && file_result->response.body == "file-async-body",
            "async file: URL should read through the async engine");
    require(static_cast<bool>(error_result->error),
            "a missing file should surface as an async error");

    // A synchronous policy violation throws from start() (JS converts it into
    // an async rejection).
    bool threw = false;
    try {
        async_loader.start(
            pagecore::ResourceRequest{"gopher://example.test/x"},
            [](pagecore::AsyncLoadResult) {});
    } catch (const pagecore::ResourceError&) {
        threw = true;
    }
    require(threw, "a disallowed scheme should throw synchronously from start()");

    // Regression: cancel() must also drop non-network (data:/file:) loads,
    // whose completion is a queued task rather than a curl transfer.
    bool cancelled_completed = false;
    const std::uint64_t cancel_id = async_loader.start(
        pagecore::ResourceRequest{"data:text/plain,cancelled"},
        [&](pagecore::AsyncLoadResult) { cancelled_completed = true; });
    async_loader.cancel(cancel_id);
    (void) pump_event_loop_until(loop, [] { return false; }, std::chrono::milliseconds(50));
    require(!cancelled_completed,
            "a cancelled data: load must not deliver its completion");
}

#if !defined(_WIN32)
void test_async_loader_http_transfer_and_cancel()
{
    const BoundTestServer bound = bind_loopback_test_server(4, "async http");
    const int server_fd = bound.fd;
    const int port = bound.port;

    // Serves two ordinary requests; further connections (the cancelled and the
    // abandoned transfer) are accepted but never answered. The stuck sockets
    // stay open until after the client-side assertions (closing them earlier
    // would complete the "abandoned" transfer with a transport error).
    std::vector<int> stuck_clients;
    std::thread server([server_fd, &stuck_clients] {
        for (int i = 0; i < 4; ++i) {
            const int client = ::accept(server_fd, nullptr, nullptr);
            if (client < 0) {
                break;
            }
            std::string request;
            std::array<char, 2048> buffer{};
            while (request.find("\r\n\r\n") == std::string::npos) {
                const ssize_t n = ::recv(client, buffer.data(), buffer.size(), 0);
                if (n <= 0) {
                    break;
                }
                request.append(buffer.data(), static_cast<std::size_t>(n));
            }
            if (request.find("GET /slow") != std::string::npos) {
                // Leave the connection open without responding.
                stuck_clients.push_back(client);
                continue;
            }
            const std::string body = request.find("GET /a") != std::string::npos ? "alpha" : "beta";
            const std::string response =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/plain\r\n"
                "Content-Length: " + std::to_string(body.size()) + "\r\n"
                "Connection: close\r\n"
                "\r\n" + body;
            (void) ::send(client, response.data(), response.size(), 0);
            ::close(client);
        }
        ::close(server_fd);
    });

    try {
        pagecore::EventLoop loop;
        pagecore::ResourcePolicy policy;
        policy.block_private_hosts = false;
        auto loader = std::make_shared<pagecore::CurlResourceLoader>("pagecore-async-test", policy);
        const std::string base = "http://127.0.0.1:" + std::to_string(port);

        {
            pagecore::CurlMultiAsyncLoader async_loader(loop, loader, "pagecore-async-test");

            std::optional<pagecore::AsyncLoadResult> result_a;
            std::optional<pagecore::AsyncLoadResult> result_b;
            async_loader.start(
                pagecore::ResourceRequest{base + "/a"},
                [&](pagecore::AsyncLoadResult result) { result_a = std::move(result); });
            async_loader.start(
                pagecore::ResourceRequest{base + "/b"},
                [&](pagecore::AsyncLoadResult result) { result_b = std::move(result); });
            require(loop.external_work() == 2, "in-flight transfers should register as external work");

            require(
                pump_event_loop_until(
                    loop,
                    [&] { return result_a && result_b; },
                    std::chrono::milliseconds(10000)),
                "concurrent async HTTP transfers should complete");
            require(!result_a->error && result_a->response.status == 200 && result_a->response.body == "alpha",
                    "first async HTTP transfer should succeed");
            require(!result_b->error && result_b->response.status == 200 && result_b->response.body == "beta",
                    "second async HTTP transfer should succeed");
            require(loop.external_work() == 0, "completed transfers should release external work");

            // Cancel mid-transfer: the completion callback must never fire.
            bool cancelled_completed = false;
            const std::uint64_t cancel_id = async_loader.start(
                pagecore::ResourceRequest{base + "/slow-cancel"},
                [&](pagecore::AsyncLoadResult) { cancelled_completed = true; });
            (void) pump_event_loop_until(loop, [] { return false; }, std::chrono::milliseconds(50));
            async_loader.cancel(cancel_id);
            require(loop.external_work() == 0, "cancel should release the transfer's external work");
            (void) pump_event_loop_until(loop, [] { return false; }, std::chrono::milliseconds(50));
            require(!cancelled_completed, "a cancelled transfer must not deliver a completion");

            // Leave one transfer in flight; destroying the loader (and then the
            // loop) with it pending must neither hang nor crash.
            bool abandoned_completed = false;
            async_loader.start(
                pagecore::ResourceRequest{base + "/slow-abandon"},
                [&](pagecore::AsyncLoadResult) { abandoned_completed = true; });
            (void) pump_event_loop_until(loop, [] { return false; }, std::chrono::milliseconds(50));
            async_loader.shutdown();
            require(!abandoned_completed, "teardown with an in-flight transfer must not run its callback");
        }
        loop.shutdown();
    } catch (...) {
        if (server.joinable()) {
            server.join();
        }
        for (const int client : stuck_clients) {
            ::close(client);
        }
        throw;
    }

    if (server.joinable()) {
        server.join();
    }
    for (const int client : stuck_clients) {
        ::close(client);
    }
}
#endif

#if !defined(_WIN32)
// End-to-end proof that page fetch() is truly asynchronous on the curl engine:
// a slow transfer must not block a fast one or delay timers.
void test_page_fetch_truly_async_over_http()
{
    const BoundTestServer bound = bind_loopback_test_server(4, "async page fetch");
    const int server_fd = bound.fd;
    const int port = bound.port;

    // Concurrent server: one thread per connection so /slow does not serialize
    // /fast behind it.
    std::thread server([server_fd] {
        std::vector<std::thread> workers;
        for (int i = 0; i < 2; ++i) {
            const int client = ::accept(server_fd, nullptr, nullptr);
            if (client < 0) {
                break;
            }
            workers.emplace_back([client] {
                std::string request;
                std::array<char, 2048> buffer{};
                while (request.find("\r\n\r\n") == std::string::npos) {
                    const ssize_t n = ::recv(client, buffer.data(), buffer.size(), 0);
                    if (n <= 0) {
                        break;
                    }
                    request.append(buffer.data(), static_cast<std::size_t>(n));
                }
                const bool slow = request.find("GET /slow") != std::string::npos;
                if (slow) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(150));
                }
                const std::string body = slow ? "slow" : "fast";
                const std::string response =
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: text/plain\r\n"
                    "Content-Length: " + std::to_string(body.size()) + "\r\n"
                    "Connection: close\r\n"
                    "\r\n" + body;
                (void) ::send(client, response.data(), response.size(), 0);
                ::close(client);
            });
        }
        for (auto& worker : workers) {
            worker.join();
        }
        ::close(server_fd);
    });

    try {
        pagecore::ResourcePolicy policy;
        policy.block_private_hosts = false;
        auto curl_loader = std::make_shared<pagecore::CurlResourceLoader>("pagecore-test", policy);

        pagecore::LoadOptions options;
        options.wait_until = pagecore::WaitUntil::Ready;
        options.wait_time = std::chrono::milliseconds(10000);
        options.stable_window = std::chrono::milliseconds(5);

        pagecore::Page page(options);
        page.set_resource_loader(curl_loader);
        page.load_html(R"HTML(
<html><body>
  <script>
    const order = [];
    const check = () => {
      if (order.length === 3) document.body.setAttribute('data-order', order.join(','));
    };
    fetch('/slow').then((r) => r.text()).then((t) => { order.push(t); check(); });
    fetch('/fast').then((r) => r.text()).then((t) => { order.push(t); check(); });
    setTimeout(() => { order.push('timer'); check(); }, 30);
  </script>
</body></html>
)HTML", "http://127.0.0.1:" + std::to_string(port) + "/index.html");

        // The 150ms transfer (started FIRST) must not block the instant fetch
        // or the 30ms timer: everything completes, and 'slow' lands last. The
        // relative order of 'fast' vs 'timer' is a network-vs-timer race and
        // deliberately not asserted.
        const std::string order = page.eval("document.body.getAttribute('data-order') || ''");
        require(order == "fast,timer,slow" || order == "timer,fast,slow",
                "a slow fetch must not block a fast fetch or a timer (truly async transfers); got: " + order);
    } catch (...) {
        if (server.joinable()) {
            server.join();
        }
        throw;
    }
    if (server.joinable()) {
        server.join();
    }
}

void test_page_fetch_abort_and_teardown_with_inflight_transfer()
{
    const BoundTestServer bound = bind_loopback_test_server(4, "abort/teardown fetch");
    const int server_fd = bound.fd;
    const int port = bound.port;

    // Accepts connections and never responds; sockets stay open until the end
    // of the test. The loop ends when the listening socket is closed by the
    // main thread (accept then fails), so a missing connection can never hang
    // the test in join().
    std::vector<int> stuck_clients;
    std::mutex stuck_mutex;
    std::thread server([server_fd, &stuck_clients, &stuck_mutex] {
        for (;;) {
            const int client = ::accept(server_fd, nullptr, nullptr);
            if (client < 0) {
                break;
            }
            std::lock_guard<std::mutex> lock(stuck_mutex);
            stuck_clients.push_back(client);
        }
    });
    auto finish_server = [&] {
        // Shut down the listener; accept() in the server thread fails and the
        // thread exits.
        ::shutdown(server_fd, SHUT_RDWR);
        ::close(server_fd);
        if (server.joinable()) {
            server.join();
        }
        std::lock_guard<std::mutex> lock(stuck_mutex);
        for (const int client : stuck_clients) {
            ::close(client);
        }
        stuck_clients.clear();
    };

    const std::string base = "http://127.0.0.1:" + std::to_string(port);
    pagecore::ResourcePolicy policy;
    policy.block_private_hosts = false;

    try {
        // Abort mid-transfer: the fetch rejects with AbortError and readiness
        // completes (the cancelled transfer no longer holds the loop busy).
        {
            auto curl_loader = std::make_shared<pagecore::CurlResourceLoader>("pagecore-test", policy);
            pagecore::LoadOptions options;
            options.wait_until = pagecore::WaitUntil::Ready;
            options.wait_time = std::chrono::milliseconds(10000);
            options.stable_window = std::chrono::milliseconds(5);

            pagecore::Page page(options);
            page.set_resource_loader(curl_loader);
            page.load_html(R"HTML(
<html><body>
  <script>
    const controller = new AbortController();
    fetch('/stuck-abort', { signal: controller.signal }).then(
      () => document.body.setAttribute('data-abort', 'resolved'),
      (error) => document.body.setAttribute('data-abort', error && error.name === 'AbortError' ? 'aborted' : 'other'));
    setTimeout(() => controller.abort(), 30);
  </script>
</body></html>
)HTML", base + "/index.html");

            require(page.outer_html("body[data-abort='aborted']").has_value(),
                    "aborting a fetch mid-transfer should reject with AbortError and unblock readiness");
        }

        // Teardown with an in-flight transfer: destroying the page while the
        // server never answers must neither hang nor crash.
        {
            auto curl_loader = std::make_shared<pagecore::CurlResourceLoader>("pagecore-test", policy);
            pagecore::LoadOptions options;
            options.wait_until = pagecore::WaitUntil::Load;
            options.wait_time = std::chrono::milliseconds(0);

            pagecore::Page page(options);
            page.set_resource_loader(curl_loader);
            page.load_html(R"HTML(
<html><body>
  <script>
    fetch('/stuck-teardown').then(() => {}, () => {});
  </script>
</body></html>
)HTML", base + "/index.html");
            // Give the transfer a moment to actually connect, then let `page`
            // go out of scope with the transfer still pending.
            page.run_until_ready(pagecore::PageReadinessOptions{
                pagecore::WaitUntil::NetworkIdle,
                std::chrono::milliseconds(50),
                std::chrono::milliseconds(0),
            });
        }
    } catch (...) {
        finish_server();
        throw;
    }

    finish_server();
}
#endif

void test_request_animation_frame_real_frames()
{
    pagecore::LoadOptions options;
    options.wait_until = pagecore::WaitUntil::Ready;
    options.wait_time = std::chrono::milliseconds(5000);
    // Generous stability window: the first ~16ms frame must land inside it
    // even on slow (sanitizer) runs, so the frame chain provably runs before
    // readiness completes.
    options.stable_window = std::chrono::milliseconds(150);

    pagecore::Page page(options);
    const auto load_start = std::chrono::steady_clock::now();
    page.load_html(R"HTML(
<html><body>
  <script>
    window.__frames = [];
    const cancelled = requestAnimationFrame(() => { window.__cancelledRan = true; });
    cancelAnimationFrame(cancelled);
    // Each frame mutates the DOM: rAF alone never blocks readiness, but its
    // DOM mutations extend the stability window, so both frames run before
    // the page is considered ready.
    requestAnimationFrame((t1) => {
      window.__frames.push(t1);
      document.body.setAttribute('data-frame-one', 'done');
      requestAnimationFrame((t2) => {
        window.__frames.push(t2);
        document.body.setAttribute('data-frames', 'done');
      });
    });
    // performance.now() monotonicity across a real timer.
    window.__now1 = performance.now();
    setTimeout(() => { window.__now2 = performance.now(); }, 10);
    // Anchor the stability window to the end of this script.
    document.body.setAttribute('data-parsed', 'yes');
  </script>
</body></html>
)HTML", "https://example.test/index.html");
    const auto load_elapsed = std::chrono::steady_clock::now() - load_start;

    require(page.outer_html("body[data-frames='done']").has_value(),
            "two chained animation frames should run during a ready wait");
    require(page.eval("window.__cancelledRan === undefined") == "true",
            "a cancelled animation frame callback must not run");
    require(page.eval("window.__frames.length === 2 && window.__frames[0] > 0") == "true",
            "animation frame timestamps should be positive");
    require(page.eval("window.__frames[1] > window.__frames[0]") == "true",
            "animation frame timestamps should be monotonic");
    // Two consecutive frames should be roughly one frame interval apart
    // (>=10ms allows scheduler slack around the 16ms cadence).
    require(page.eval("(window.__frames[1] - window.__frames[0]) >= 10") == "true",
            "consecutive animation frames should be ~16ms apart");
    require(page.eval("window.__now2 > window.__now1") == "true",
            "performance.now() should be monotonic across real timers");
    // rAF alone must not hold readiness open for the whole wait budget.
    require(load_elapsed < std::chrono::milliseconds(3000),
            "animation frames alone must not hold run_until_ready open");
}

void test_animation_frame_loop_does_not_block_idle_or_ready()
{
    pagecore::LoadOptions options;
    options.wait_until = pagecore::WaitUntil::Ready;
    options.wait_time = std::chrono::milliseconds(4000);
    // Wide enough for the ~16ms frames to land inside it on slow runs.
    options.stable_window = std::chrono::milliseconds(150);

    pagecore::Page page(options);
    const auto load_start = std::chrono::steady_clock::now();
    page.load_html(R"HTML(
<html><body>
  <script>
    // A self-perpetuating rAF loop. The first three frames mutate the DOM
    // (each mutation extends the stability window, so they are guaranteed to
    // run before readiness); after that the loop keeps scheduling frames
    // without mutating, and readiness must complete without waiting out the
    // whole budget.
    window.__frameCount = 0;
    const tick = () => {
      window.__frameCount++;
      if (window.__frameCount <= 3) document.body.setAttribute('data-ticks', window.__frameCount);
      requestAnimationFrame(tick);
    };
    requestAnimationFrame(tick);
    // Anchor the stability window to the end of this script.
    document.body.setAttribute('data-parsed', 'yes');
  </script>
</body></html>
)HTML", "https://example.test/index.html");
    const auto load_elapsed = std::chrono::steady_clock::now() - load_start;

    require(load_elapsed < std::chrono::milliseconds(3000),
            "a non-mutating rAF loop must not hold readiness open for the whole budget");
    require(page.eval("window.__frameCount >= 3") == "true",
            "DOM-mutating frames should all run before readiness completes");

    // run_until_idle with a wait budget must also terminate: pending frames
    // never count as pending work for idleness.
    const auto idle_start = std::chrono::steady_clock::now();
    page.run_until_idle();
    require(std::chrono::steady_clock::now() - idle_start < std::chrono::milliseconds(3000),
            "a rAF loop must not keep run_until_idle spinning for the whole budget");
}

void test_task_queue_async_loader_wraps_blocking_loader()
{
    pagecore::EventLoop loop;
    auto memory = std::make_shared<pagecore::MemoryResourceLoader>();
    memory->add("https://example.test/data.json", "{\"ok\":true}", "application/json");

    pagecore::TaskQueueAsyncLoader async_loader(loop, memory);

    std::optional<pagecore::AsyncLoadResult> hit;
    std::optional<pagecore::AsyncLoadResult> miss;
    async_loader.start(
        pagecore::ResourceRequest{"https://example.test/data.json"},
        [&](pagecore::AsyncLoadResult result) { hit = std::move(result); });
    async_loader.start(
        pagecore::ResourceRequest{"https://example.test/missing.json"},
        [&](pagecore::AsyncLoadResult result) { miss = std::move(result); });

    bool cancelled_completed = false;
    const std::uint64_t cancel_id = async_loader.start(
        pagecore::ResourceRequest{"https://example.test/data.json"},
        [&](pagecore::AsyncLoadResult) { cancelled_completed = true; });
    async_loader.cancel(cancel_id);

    require(!hit.has_value(), "blocking-loader completions must be queued, not delivered from start()");
    require(
        pump_event_loop_until(loop, [&] { return hit && miss; }, std::chrono::milliseconds(2000)),
        "task-queue async loads should complete");
    require(!hit->error && hit->response.body == "{\"ok\":true}",
            "task-queue async loader should deliver the blocking loader's response");
    require(static_cast<bool>(miss->error), "a missing memory resource should surface as an async error");
    require(!cancelled_completed, "a cancelled task-queue load must not deliver a completion");
}

void test_caching_loader_bounds_and_skips_errors()
{
    auto memory = std::make_shared<pagecore::MemoryResourceLoader>();
    memory->add("https://example.test/a", "A", "text/plain");
    memory->add("https://example.test/b", "B", "text/plain");
    memory->add("https://example.test/c", "C", "text/plain");

    pagecore::CachingResourceLoader cache(memory, 2);
    (void) cache.load(pagecore::ResourceRequest{"https://example.test/a"});
    (void) cache.load(pagecore::ResourceRequest{"https://example.test/b"});
    (void) cache.load(pagecore::ResourceRequest{"https://example.test/c"});
    require(cache.size() <= 2, "caching loader should bound its cache size");

    // Missing resources throw (transport-style); the failure must not be cached.
    require_resource_error(
        pagecore::ResourceErrorCode::NotFound,
        [&] { (void) cache.load(pagecore::ResourceRequest{"https://example.test/missing"}); },
        "missing resource should surface NotFound");
    bool second_throws = false;
    try {
        (void) cache.load(pagecore::ResourceRequest{"https://example.test/missing"});
    } catch (const pagecore::ResourceError&) {
        second_throws = true;
    }
    require(second_throws, "thrown errors must not be cached as success");
}

void test_resource_load_all_returns_in_order()
{
    auto memory = std::make_shared<pagecore::MemoryResourceLoader>();
    memory->add("https://example.test/a", "A");
    memory->add("https://example.test/b", "B");
    memory->add("https://example.test/c", "C");

    const std::vector<pagecore::ResourceRequest> requests{
        {"https://example.test/a", pagecore::ResourceKind::Script},
        {"https://example.test/b", pagecore::ResourceKind::Script},
        {"https://example.test/c", pagecore::ResourceKind::Script},
    };
    const auto responses = memory->load_all(requests);
    require(responses.size() == 3, "load_all returns one response per request");
    require(responses[0].body == "A" && responses[1].body == "B" && responses[2].body == "C",
            "load_all preserves request order");
    require(responses[0].kind == pagecore::ResourceKind::Script, "load_all carries the request kind");
}

void test_resource_load_all_propagates_first_error()
{
    auto memory = std::make_shared<pagecore::MemoryResourceLoader>();
    memory->add("https://example.test/a", "A");
    memory->add("https://example.test/c", "C");

    const std::vector<pagecore::ResourceRequest> requests{
        {"https://example.test/a"},
        {"https://example.test/missing"},
        {"https://example.test/c"},
    };
    require_resource_error(
        pagecore::ResourceErrorCode::NotFound,
        [&] { (void) memory->load_all(requests); },
        "load_all surfaces the first request-order failure");
}

void test_resource_load_all_lenient_tolerates_failures()
{
    auto memory = std::make_shared<pagecore::MemoryResourceLoader>();
    memory->add("https://example.test/a", "A");
    memory->add("https://example.test/c", "C");

    const std::vector<pagecore::ResourceRequest> requests{
        {"https://example.test/a", pagecore::ResourceKind::Image},
        {"https://example.test/missing", pagecore::ResourceKind::Image},
        {"https://example.test/c", pagecore::ResourceKind::Image},
    };

    // Lenient must not throw; failures become status-0 placeholders in place.
    const auto responses = memory->load_all(requests, pagecore::BatchErrorMode::Lenient);
    require(responses.size() == 3, "lenient load_all returns one entry per request");
    require(responses[0].body == "A" && responses[0].status == 200, "successful entries are intact");
    require(responses[1].status == 0 && responses[1].body.empty(), "failed entries become status-0 placeholders");
    require(responses[1].url == "https://example.test/missing", "placeholder keeps the requested URL");
    require(responses[2].body == "C", "entries after a failure are still loaded");
}

void test_caching_loader_load_all_serves_hits_and_caches()
{
    auto memory = std::make_shared<pagecore::MemoryResourceLoader>();
    memory->add("https://example.test/a", "A");
    memory->add("https://example.test/b", "B");

    pagecore::CachingResourceLoader cache(memory, 8);
    const auto warm = cache.load(pagecore::ResourceRequest{"https://example.test/a", pagecore::ResourceKind::Script});
    require(!warm.from_cache, "first load is not served from cache");
    require(cache.size() == 1, "first load populates the cache");

    const std::vector<pagecore::ResourceRequest> requests{
        {"https://example.test/a", pagecore::ResourceKind::Script},
        {"https://example.test/b", pagecore::ResourceKind::Script},
    };
    const auto responses = cache.load_all(requests);
    require(responses.size() == 2, "load_all returns all responses in order");
    require(responses[0].body == "A" && responses[0].from_cache, "previously cached entry is served from cache");
    require(responses[1].body == "B" && !responses[1].from_cache, "uncached entry is fetched fresh");
    require(cache.size() == 2, "load_all caches freshly fetched misses");

    const auto again = cache.load_all(requests);
    require(again[0].from_cache && again[1].from_cache, "a repeated batch is served entirely from cache");

    auto dynamic = std::make_shared<RecordingResourceLoader>();
    dynamic->add("https://example.test/api", "OK");
    pagecore::CachingResourceLoader request_cache(dynamic, 8);
    (void) request_cache.load(pagecore::ResourceRequest{
        "https://example.test/api",
        pagecore::ResourceKind::Other,
        "https://example.test/page.html",
        "https://example.test/page.html",
        "POST",
        "first",
        {{"content-type", "text/plain"}},
    });
    (void) request_cache.load(pagecore::ResourceRequest{
        "https://example.test/api",
        pagecore::ResourceKind::Other,
        "https://example.test/page.html",
        "https://example.test/page.html",
        "POST",
        "second",
        {{"content-type", "text/plain"}},
    });
    require(dynamic->requests.size() == 2,
            "cache key should include request method, body and headers, not just URL");
}

void test_external_scripts_load_in_document_order()
{
    auto loader = std::make_shared<RecordingResourceLoader>();
    loader->add("https://example.test/one.js", "window.__order = (window.__order || '') + '1';");
    loader->add("https://example.test/two.js", "window.__order = (window.__order || '') + '2';");
    loader->add("https://example.test/three.js", "window.__order = (window.__order || '') + '3';");

    pagecore::Page page;
    page.set_resource_loader(loader);
    page.load_html(R"HTML(
<html><body>
  <script src="/one.js"></script>
  <script src="/two.js"></script>
  <script src="/three.js"></script>
</body></html>
)HTML", "https://example.test/index.html");

    require(page.eval("window.__order") == "123",
            "batch-prefetched external scripts must still execute in document order");
    require(loader->requests.size() == 3, "all external scripts are fetched");
    require(has_request_kind(*loader, "https://example.test/two.js", pagecore::ResourceKind::Script),
            "external scripts are requested with the Script kind");
}

void test_native_bridge_not_exposed_to_page()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<!doctype html>
<html><body><div id="x"></div><script>document.getElementById('x').textContent='ok';</script></body></html>
)HTML");
    require(page.eval("typeof window.__host") == "undefined",
            "native host bridge must not be reachable from page script");
    require(page.eval("typeof window.__dom") == "undefined",
            "native dom bridge must not be reachable from page script");
    require(page.eval("typeof document") == "object",
            "document shim should still be available to page script");
    auto text = page.text_content("#x");
    require(text && *text == "ok", "shim should still mediate DOM access after bridge removal");
}

#if defined(PAGECORE_ENABLE_RENDERING)
void test_cairo_raster_backend()
{
    pagecore::DisplayList display_list;
    display_list.viewport = pagecore::Viewport{160, 80, 1.0f};
    display_list.commands.emplace_back(pagecore::SolidFillCommand{
        pagecore::Rect{2, 3, 5, 4},
        pagecore::Color{10, 20, 30, 255},
        false,
    });
    display_list.commands.emplace_back(pagecore::BorderCommand{
        pagecore::Rect{10, 10, 6, 6},
        pagecore::BorderSide{2, pagecore::Color{200, 0, 0, 255}, pagecore::BorderStyle::Solid},
        pagecore::BorderSide{1, pagecore::Color{0, 200, 0, 255}, pagecore::BorderStyle::Solid},
        pagecore::BorderSide{2, pagecore::Color{0, 0, 200, 255}, pagecore::BorderStyle::Solid},
        pagecore::BorderSide{1, pagecore::Color{100, 100, 0, 255}, pagecore::BorderStyle::Solid},
        false,
    });
    display_list.commands.emplace_back(pagecore::TextCommand{
        "Hello",
        pagecore::Rect{24, 16, 120, 40},
        pagecore::Color{0, 0, 0, 255},
        pagecore::Font{"sans-serif", 28.0f, 400, false},
    });

    auto raster = pagecore::create_default_raster_backend(pagecore::Color{255, 255, 255, 255});
    const auto image = raster->render(display_list);

    require(image.width == 160 && image.height == 80, "Cairo raster should use viewport dimensions");
    require(image.rgba.size() == 160 * 80 * 4, "Cairo raster should emit RGBA pixels");
    require(pixel_matches(image, 2, 3, pagecore::Color{10, 20, 30, 255}), "Cairo raster should draw solid fills");
    require(pixel_matches(image, 13, 10, pagecore::Color{0, 200, 0, 255}), "Cairo raster should draw top border");
    require(pixel_matches(image, 10, 12, pagecore::Color{200, 0, 0, 255}), "Cairo raster should draw left border");
    require(image_has_non_solid_text_pixel(image), "Cairo/Pango raster should render anti-aliased text, not placeholder bars");
}

void test_cairo_raster_rounded_border_uses_inner_curve()
{
    pagecore::DisplayList display_list;
    display_list.viewport = pagecore::Viewport{56, 56, 1.0f};
    display_list.commands.emplace_back(pagecore::BorderCommand{
        pagecore::Rect{4, 4, 40, 40},
        pagecore::BorderSide{6, pagecore::Color{0, 0, 0, 255}, pagecore::BorderStyle::Solid},
        pagecore::BorderSide{6, pagecore::Color{0, 0, 0, 255}, pagecore::BorderStyle::Solid},
        pagecore::BorderSide{6, pagecore::Color{0, 0, 0, 255}, pagecore::BorderStyle::Solid},
        pagecore::BorderSide{6, pagecore::Color{0, 0, 0, 255}, pagecore::BorderStyle::Solid},
        false,
        pagecore::BorderRadii{
            pagecore::CornerRadii{16, 16},
            pagecore::CornerRadii{16, 16},
            pagecore::CornerRadii{16, 16},
            pagecore::CornerRadii{16, 16},
        },
    });

    auto raster = pagecore::create_default_raster_backend(pagecore::Color{255, 255, 255, 255});
    const auto image = raster->render(display_list);

    require(pixel_matches(image, 4, 4, pagecore::Color{255, 255, 255, 255}),
            "rounded border should not fill outside the outer curve");
    require(pixel_is_dark(image, 10, 10), "rounded border should follow the inner curve, not square strips");
    require(pixel_matches(image, 15, 15, pagecore::Color{255, 255, 255, 255}),
            "rounded border should leave the inner corner open");
}

void test_cairo_raster_rounded_border_supports_uneven_widths()
{
    pagecore::DisplayList display_list;
    display_list.viewport = pagecore::Viewport{64, 56, 1.0f};
    display_list.commands.emplace_back(pagecore::BorderCommand{
        pagecore::Rect{4, 4, 48, 40},
        pagecore::BorderSide{10, pagecore::Color{0, 0, 0, 255}, pagecore::BorderStyle::Solid},
        pagecore::BorderSide{4, pagecore::Color{0, 0, 0, 255}, pagecore::BorderStyle::Solid},
        pagecore::BorderSide{6, pagecore::Color{0, 0, 0, 255}, pagecore::BorderStyle::Solid},
        pagecore::BorderSide{12, pagecore::Color{0, 0, 0, 255}, pagecore::BorderStyle::Solid},
        false,
        pagecore::BorderRadii{
            pagecore::CornerRadii{18, 18},
            pagecore::CornerRadii{18, 18},
            pagecore::CornerRadii{18, 18},
            pagecore::CornerRadii{18, 18},
        },
    });

    auto raster = pagecore::create_default_raster_backend(pagecore::Color{255, 255, 255, 255});
    const auto image = raster->render(display_list);

    require(pixel_is_dark(image, 15, 9), "uneven rounded border should fill curved ring outside strip-only regions");
    require(pixel_matches(image, 18, 12, pagecore::Color{255, 255, 255, 255}),
            "uneven rounded border should preserve the inner rounded opening");
    require(pixel_is_dark(image, 8, 24), "uneven rounded border should still draw the wide left side");
    require(pixel_is_dark(image, 28, 39), "uneven rounded border should still draw the wide bottom side");
}

void test_cairo_pdf_writer_emits_pdf_file()
{
    pagecore::DisplayList display_list;
    display_list.viewport = pagecore::Viewport{120, 80, 1.0f};
    display_list.commands.emplace_back(pagecore::SolidFillCommand{
        pagecore::Rect{0, 0, 120, 80},
        pagecore::Color{255, 255, 255, 255},
        true,
    });
    display_list.commands.emplace_back(pagecore::SolidFillCommand{
        pagecore::Rect{10, 10, 40, 24},
        pagecore::Color{30, 90, 180, 255},
        false,
    });
    display_list.commands.emplace_back(pagecore::TextCommand{
        "PDF",
        pagecore::Rect{12, 40, 90, 24},
        pagecore::Color{0, 0, 0, 255},
        pagecore::Font{"sans-serif", 18.0f, 400, false},
    });

    const std::filesystem::path output = std::filesystem::path(PAGECORE_BINARY_DIR) / "pagecore_pdf_writer_test.pdf";
    pagecore::write_pdf(display_list, output.string());

    std::ifstream in(output, std::ios::binary);
    require(static_cast<bool>(in), "PDF writer should create an output file");
    char header[4] = {};
    in.read(header, sizeof(header));
    require(in.gcount() == 4 && std::string(header, sizeof(header)) == "%PDF", "PDF writer should emit a PDF header");
}

void test_viewport_culling_is_byte_identical()
{
    auto build_display_list = []() {
        pagecore::DisplayList display_list;
        display_list.viewport = pagecore::Viewport{100, 60, 1.0f};
        display_list.commands.emplace_back(pagecore::SolidFillCommand{
            pagecore::Rect{10, 10, 20, 20},
            pagecore::Color{10, 20, 30, 255},
            false,
        });
        // Below-the-fold leaves, entirely outside the 100x60 canvas.
        display_list.commands.emplace_back(pagecore::ClipCommand{
            pagecore::Rect{0, 5000, 100, 40},
            true,
        });
        display_list.commands.emplace_back(pagecore::SolidFillCommand{
            pagecore::Rect{0, 5000, 100, 40},
            pagecore::Color{200, 0, 0, 255},
            false,
        });
        display_list.commands.emplace_back(pagecore::TextCommand{
            "below fold",
            pagecore::Rect{0, 5010, 100, 20},
            pagecore::Color{0, 0, 0, 255},
            pagecore::Font{"sans-serif", 14.0f, 400, false},
        });
        display_list.commands.emplace_back(pagecore::ClipCommand{
            pagecore::Rect{0, 5000, 100, 40},
            false,
        });
        display_list.commands.emplace_back(pagecore::SolidFillCommand{
            pagecore::Rect{0, 5100, 100, 20},
            pagecore::Color{0, 200, 0, 255},
            false,
        });
        return display_list;
    };

    pagecore::DisplayList culled = build_display_list();
    pagecore::DisplayList uncculled = build_display_list();
    uncculled.disable_viewport_culling = true;

    auto raster = pagecore::create_default_raster_backend(pagecore::Color{255, 255, 255, 255});
    const auto culled_image = raster->render(culled);
    const auto uncculled_image = raster->render(uncculled);

    require(pixel_matches(culled_image, 10, 10, pagecore::Color{10, 20, 30, 255}),
            "visible solid fill should still render within the viewport");
    require(culled_image.width == uncculled_image.width && culled_image.height == uncculled_image.height,
            "culling must not change canvas dimensions");
    require(culled_image.rgba == uncculled_image.rgba,
            "culling below-the-fold commands must be byte-identical to rendering every command, "
            "including a clip push/pop pair around a culled leaf");
}

void test_viewport_culling_preserves_expanded_viewport_content()
{
    pagecore::DisplayList display_list;
    display_list.viewport = pagecore::Viewport{100, 100, 1.0f};
    const pagecore::Color fill_color{40, 120, 200, 255};
    display_list.commands.emplace_back(pagecore::SolidFillCommand{
        pagecore::Rect{0, 2000, 100, 40},
        fill_color,
        false,
    });

    auto raster = pagecore::create_default_raster_backend(pagecore::Color{255, 255, 255, 255});

    const auto small_image = raster->render(display_list);
    require(small_image.width == 100 && small_image.height == 100,
            "small viewport canvas should match its own dimensions when the fill is culled");

    // Simulate --full-page inflating the viewport height to the content height:
    // the same below-the-fold command must now land on the canvas and paint.
    display_list.viewport.height = 3000;
    const auto expanded_image = raster->render(display_list);
    require(expanded_image.width == 100 && expanded_image.height == 3000,
            "expanded viewport canvas should grow to the new height");
    require(pixel_matches(expanded_image, 10, 2000, fill_color),
            "below-the-fold content must survive when the viewport expands to cover it, as --full-page does");
    require(pixel_matches(expanded_image, 10, 2039, fill_color),
            "below-the-fold content must survive across its full rect when the viewport expands");
}

void test_write_pdf_full_page_keeps_below_fold_content()
{
    pagecore::DisplayList display_list;
    display_list.viewport = pagecore::Viewport{100, 2500, 1.0f};
    display_list.commands.emplace_back(pagecore::SolidFillCommand{
        pagecore::Rect{0, 0, 100, 2500},
        pagecore::Color{255, 255, 255, 255},
        true,
    });
    display_list.commands.emplace_back(pagecore::SolidFillCommand{
        pagecore::Rect{0, 2000, 100, 40},
        pagecore::Color{40, 120, 200, 255},
        false,
    });

    const std::filesystem::path output = std::filesystem::path(PAGECORE_BINARY_DIR) / "pagecore_pdf_full_page_below_fold_test.pdf";
    pagecore::write_pdf(display_list, output.string());

    std::ifstream in(output, std::ios::binary);
    require(static_cast<bool>(in), "PDF writer should create an output file for a full-page-sized viewport");
    char header[4] = {};
    in.read(header, sizeof(header));
    require(in.gcount() == 4 && std::string(header, sizeof(header)) == "%PDF",
            "PDF writer should emit a PDF header even when the viewport covers below-the-fold content");
    in.seekg(0, std::ios::end);
    require(in.tellg() > 0, "PDF writer should emit a non-empty file for below-the-fold content");
}
#endif

void test_display_list_json_dump()
{
    pagecore::DisplayList display_list;
    display_list.viewport = pagecore::Viewport{80, 40, 2.0f};
    display_list.content_width = 70;
    display_list.content_height = 30;
    display_list.commands.emplace_back(pagecore::SolidFillCommand{
        pagecore::Rect{1, 2, 3, 4},
        pagecore::Color{5, 6, 7, 255},
        false,
        pagecore::BorderRadii{pagecore::CornerRadii{8, 8}, {}, {}, {}},
    });
    display_list.commands.emplace_back(pagecore::TextCommand{
        "quoted \"text\"",
        pagecore::Rect{10, 12, 30, 14},
        pagecore::Color{20, 30, 40, 255},
        pagecore::Font{"Arial", 16.0f, 700, true},
    });

    const auto dump = pagecore::display_list_to_json(display_list);
    require(dump.find("\"viewport\":{\"width\":80,\"height\":40") != std::string::npos,
            "display list dump should include viewport");
    require(dump.find("\"contentWidth\":70") != std::string::npos, "display list dump should include content width");
    require(dump.find("\"type\":\"solidFill\"") != std::string::npos, "display list dump should include solid fills");
    require(dump.find("\"type\":\"text\"") != std::string::npos, "display list dump should include text commands");
    require(dump.find("quoted \\\"text\\\"") != std::string::npos, "display list dump should escape strings");
}

void test_png_encoder_rgba()
{
    pagecore::RenderedImage image;
    image.width = 2;
    image.height = 1;
    image.rgba = {
        255, 0, 0, 255,
        0, 255, 0, 128,
    };

    const auto png = pagecore::encode_png_rgba(image);
    require(png.size() > 8, "PNG encoder should emit data");
    require(
        png[0] == 0x89 && png[1] == 'P' && png[2] == 'N' && png[3] == 'G'
            && png[4] == '\r' && png[5] == '\n' && png[6] == 0x1a && png[7] == '\n',
        "PNG encoder should emit PNG signature");

    bool saw_ihdr = false;
    bool saw_idat = false;
    bool saw_iend = false;
    std::size_t offset = 8;
    while (offset < png.size()) {
        const std::uint32_t length = read_be32(png, offset);
        offset += 4;
        require(offset + 4 <= png.size(), "PNG chunk should have a type");
        const std::string type(reinterpret_cast<const char*>(&png[offset]), 4);
        offset += 4;
        require(offset + length + 4 <= png.size(), "PNG chunk should fit in file");

        if (type == "IHDR") {
            saw_ihdr = true;
            require(length == 13, "IHDR should be 13 bytes");
            require(read_be32(png, offset) == 2, "IHDR should store image width");
            require(read_be32(png, offset + 4) == 1, "IHDR should store image height");
            require(png[offset + 8] == 8, "IHDR should use 8-bit samples");
            require(png[offset + 9] == 6, "IHDR should use RGBA color type");
        } else if (type == "IDAT") {
            saw_idat = true;
            require(length > 0, "IDAT should contain zlib data");
        } else if (type == "IEND") {
            saw_iend = true;
            require(length == 0, "IEND should be empty");
        }

        offset += length + 4;
    }

    require(saw_ihdr && saw_idat && saw_iend, "PNG encoder should emit required chunks");

    pagecore::RenderedImage invalid;
    invalid.width = 1;
    invalid.height = 1;
    bool rejected = false;
    try {
        (void) pagecore::encode_png_rgba(invalid);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    require(rejected, "PNG encoder should reject mismatched RGBA buffers");
}

#if defined(PAGECORE_ENABLE_RENDERING)
void test_png_decoder_rgba()
{
    const auto body = png_body(pagecore::Color{31, 63, 127, 255}, 3, 2);
    const auto decoded = pagecore::decode_image_rgba(body);

    require(decoded != nullptr, "PNG decoder should return an image");
    require(decoded->width == 3 && decoded->height == 2, "PNG decoder should preserve dimensions");
    require(decoded->rgba.size() == 3 * 2 * 4, "PNG decoder should emit RGBA pixels");
    require(decoded->rgba[0] == 31 && decoded->rgba[1] == 63 && decoded->rgba[2] == 127 && decoded->rgba[3] == 255,
            "PNG decoder should preserve pixel color");
}

void test_png_decoder_handles_16_bit_channels()
{
    // Cairo's PNG loader returns a floating-point surface (RGBA128F/RGB96F,
    // 4/3 x float32 per pixel) for >8-bit source PNGs instead of downsampling
    // to ARGB32, which previously desynchronized the decoder's fixed 4-bytes-
    // per-pixel read and produced garbage output.
    const auto decoded = pagecore::decode_image_rgba(png_body_16bit());

    require(decoded != nullptr, "PNG decoder should return an image for 16-bit-per-channel input");
    require(decoded->width == 2 && decoded->height == 2, "PNG decoder should preserve 16-bit PNG dimensions");
    require(color_close(decoded->rgba, pagecore::Color{233, 117, 39, 255}, 2),
            "PNG decoder should downsample 16-bit channels to the correct 8-bit color");
}

void test_jpeg_decoder_rgba()
{
    const auto body = jpeg_body();
    const auto decoded = pagecore::decode_image_rgba(body);

    require(decoded != nullptr, "JPEG decoder should return an image");
    require(decoded->width == 4 && decoded->height == 4, "JPEG decoder should preserve dimensions");
    require(decoded->rgba.size() == 4 * 4 * 4, "JPEG decoder should emit RGBA pixels");
    require(color_close(decoded->rgba, pagecore::Color{120, 80, 40, 255}, 8),
            "JPEG decoder should preserve approximate pixel color");
}

#if PAGECORE_ENABLE_WEBP
void test_webp_decoder_rgba()
{
    const auto body = webp_body(pagecore::Color{12, 200, 90, 255}, 4, 3);
    const auto decoded = pagecore::decode_image_rgba(body);

    require(decoded != nullptr, "WebP decoder should return an image");
    require(decoded->width == 4 && decoded->height == 3, "WebP decoder should preserve dimensions");
    require(decoded->rgba.size() == 4 * 3 * 4, "WebP decoder should emit RGBA pixels");
    require(color_close(decoded->rgba, pagecore::Color{12, 200, 90, 255}, 0),
            "WebP decoder should preserve lossless pixel color");
}
#endif

void test_jpeg_decoder_rejects_huge_dimensions()
{
    const std::string body = jpeg_header_with_dimensions(8192, 8192);

    require_runtime_error_contains(
        [&] {
            (void) pagecore::decode_jpeg_rgba(body);
        },
        "too large",
        "JPEG decoder should reject huge decoded dimensions before allocation");
}

void test_png_decoder_rejects_huge_dimensions()
{
    // Patch the IHDR width/height (BE u32 at offsets 16/20) of a valid PNG to a
    // multi-gigapixel canvas. The decoder must reject it from the header before
    // Cairo/libpng allocates the surface (CRC is intentionally left stale).
    std::string body = png_body(pagecore::Color{31, 63, 127, 255}, 2, 2);
    require(body.size() >= 24, "encoded PNG should contain an IHDR chunk");
    const auto write_be32 = [&](std::size_t at, std::uint32_t value) {
        body[at] = static_cast<char>((value >> 24) & 0xff);
        body[at + 1] = static_cast<char>((value >> 16) & 0xff);
        body[at + 2] = static_cast<char>((value >> 8) & 0xff);
        body[at + 3] = static_cast<char>(value & 0xff);
    };
    write_be32(16, 60000);
    write_be32(20, 60000);

    require_runtime_error_contains(
        [&] {
            (void) pagecore::decode_png_rgba(body);
        },
        "too large",
        "PNG decoder should reject huge IHDR dimensions before allocation");
}

void test_gif_decoder_rejects_huge_dimensions()
{
    const auto patch_u16 = [](std::string& body, std::size_t at, std::uint16_t value) {
        body[at] = static_cast<char>(value & 0xff);
        body[at + 1] = static_cast<char>((value >> 8) & 0xff);
    };

    // Logical-screen bomb: bytes 6-9 declare a 65535x65535 canvas.
    std::string screen_bomb = gif_body();
    patch_u16(screen_bomb, 6, 0xffff);
    patch_u16(screen_bomb, 8, 0xffff);
    require_runtime_error_contains(
        [&] {
            (void) pagecore::decode_gif_rgba(screen_bomb);
        },
        "too large",
        "GIF decoder should reject a huge logical screen before DGifSlurp");

    // Frame-descriptor bomb: the image descriptor (at offset 19) declares a
    // 65535x65535 frame even though the logical screen stays 1x1.
    std::string frame_bomb = gif_body();
    require(frame_bomb.size() >= 28 && static_cast<unsigned char>(frame_bomb[19]) == 0x2c,
            "test GIF should contain an image descriptor at offset 19");
    patch_u16(frame_bomb, 24, 0xffff);
    patch_u16(frame_bomb, 26, 0xffff);
    require_runtime_error_contains(
        [&] {
            (void) pagecore::decode_gif_rgba(frame_bomb);
        },
        "too large",
        "GIF decoder should reject a huge frame descriptor before DGifSlurp");
}

#if PAGECORE_ENABLE_WEBP
void test_webp_decoder_rejects_huge_dimensions()
{
    std::string body = webp_body(pagecore::Color{12, 200, 90, 255}, 4, 3);
    bool patched = false;
    for (std::size_t offset = 0; offset + 13 <= body.size(); ++offset) {
        if (body[offset] == 'V' && body[offset + 1] == 'P' && body[offset + 2] == '8' && body[offset + 3] == 'L') {
            require(static_cast<unsigned char>(body[offset + 8]) == 0x2f, "test WebP should contain a VP8L signature");
            const std::uint32_t width_minus_one = 8191;
            const std::uint32_t height_minus_one = 8191;
            const std::uint32_t packed_dimensions = width_minus_one | (height_minus_one << 14);
            body[offset + 9] = static_cast<char>(packed_dimensions & 0xff);
            body[offset + 10] = static_cast<char>((packed_dimensions >> 8) & 0xff);
            body[offset + 11] = static_cast<char>((packed_dimensions >> 16) & 0xff);
            body[offset + 12] = static_cast<char>((packed_dimensions >> 24) & 0xff);
            patched = true;
            break;
        }
    }
    require(patched, "test WebP should contain a VP8L chunk");

    require_runtime_error_contains(
        [&] {
            (void) pagecore::decode_webp_rgba(body);
        },
        "too large",
        "WebP decoder should reject huge decoded dimensions before allocation");
}
#endif

void test_gif_decoder_rgba()
{
    const auto decoded = pagecore::decode_image_rgba(gif_body());

    require(decoded != nullptr, "GIF decoder should return an image");
    require(decoded->width == 1 && decoded->height == 1, "GIF decoder should preserve first-frame canvas dimensions");
    require(decoded->rgba.size() == 4, "GIF decoder should emit RGBA pixels");
    require(color_close(decoded->rgba, pagecore::Color{255, 0, 0, 255}, 0),
            "GIF decoder should preserve first-frame palette color");
}

#if PAGECORE_ENABLE_SVG
void test_svg_decoder_rgba()
{
    const auto decoded = pagecore::decode_image_rgba(svg_body(pagecore::Color{240, 20, 30, 255}));

    require(decoded != nullptr, "SVG decoder should return an image");
    require(decoded->width == 4 && decoded->height == 3, "SVG decoder should use SVG intrinsic dimensions");
    require(decoded->rgba.size() == 4 * 3 * 4, "SVG decoder should emit RGBA pixels");
    require(color_close(decoded->rgba, pagecore::Color{240, 20, 30, 255}, 0),
            "SVG decoder should rasterize filled primitives");

    const auto path = pagecore::decode_image_rgba(
        R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8" viewBox="0 0 8 8">
  <path d="M1 1 H7 V7 H1 Z" fill="#00aa00"/>
</svg>)SVG");
    require(path->width == 8 && path->height == 8, "SVG decoder should parse path dimensions");
    require(path->rgba[4 * (static_cast<std::size_t>(4) * 8 + 4) + 1] > 120,
            "SVG decoder should rasterize basic path data");
}

void test_svg_path_parser_terminates_on_malformed_input()
{
    // Each of these previously caused an infinite loop in the path parser
    // (no forward progress on unrecognized tokens). They must now terminate
    // and still yield a placeholder image of the declared dimensions.
    const char* malformed[] = {
        R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8"><path d="z@" fill="#0a0"/></svg>)SVG",
        R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8"><path d="M0 0;" fill="#0a0"/></svg>)SVG",
        R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8"><path d="M1 1 ? L 2 2 @ # z !!!" fill="#0a0"/></svg>)SVG",
        R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8"><path d="ZZZ@@@(((" fill="#0a0"/></svg>)SVG",
    };
    for (const char* svg : malformed) {
        const auto decoded = pagecore::decode_image_rgba(svg);
        require(decoded != nullptr && decoded->width == 8 && decoded->height == 8,
                "malformed SVG path data must terminate and produce a placeholder image");
    }
}

void test_svg_decoder_rejects_huge_dimensions()
{
    require_runtime_error_contains(
        [&] {
            (void) pagecore::decode_svg_rgba(
                R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="100000" height="100000"></svg>)SVG");
        },
        "too large",
        "SVG decoder should enforce the decode byte budget before rendering the document");
}

void test_svg_decoder_renders_gradients()
{
    const auto linear = pagecore::decode_image_rgba(
        R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="8" height="4" viewBox="0 0 8 4">
  <defs>
    <linearGradient id="g" x1="0" y1="0" x2="1" y2="0">
      <stop offset="0" stop-color="#ff0000"/>
      <stop offset="1" stop-color="#0000ff"/>
    </linearGradient>
  </defs>
  <rect width="8" height="4" fill="url(#g)"/>
</svg>)SVG");
    require(linear->width == 8 && linear->height == 4, "linearGradient SVG should use intrinsic dimensions");
    require(pixel_close(linear->rgba, linear->width, 0, 2, pagecore::Color{255, 0, 0, 255}, 30),
            "linearGradient decoder should shade the start stop near the gradient origin");
    require(pixel_close(linear->rgba, linear->width, 7, 2, pagecore::Color{0, 0, 255, 255}, 30),
            "linearGradient decoder should shade the end stop near the gradient terminus");

    const auto radial = pagecore::decode_image_rgba(
        R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="10" height="10" viewBox="0 0 10 10">
  <defs>
    <radialGradient id="g" cx="0.5" cy="0.5" r="0.5">
      <stop offset="0" stop-color="#00ff00"/>
      <stop offset="1" stop-color="#ff00ff"/>
    </radialGradient>
  </defs>
  <rect width="10" height="10" fill="url(#g)"/>
</svg>)SVG");
    require(radial->width == 10 && radial->height == 10, "radialGradient SVG should use intrinsic dimensions");
    require(pixel_close(radial->rgba, radial->width, 5, 5, pagecore::Color{0, 255, 0, 255}, 45),
            "radialGradient decoder should shade the start stop near the gradient center");
    require(pixel_close(radial->rgba, radial->width, 0, 0, pagecore::Color{255, 0, 255, 255}, 20),
            "radialGradient decoder should shade the end stop near the gradient edge");
}

void test_svg_decoder_renders_use_and_defs_reference()
{
    const auto decoded = pagecore::decode_image_rgba(
        R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="6" height="6" viewBox="0 0 6 6">
  <defs><rect id="r" width="6" height="6" fill="#123456"/></defs>
  <use href="#r"/>
</svg>)SVG");
    require(decoded->width == 6 && decoded->height == 6, "<use>/<defs> SVG should use intrinsic dimensions");
    require(pixel_close(decoded->rgba, decoded->width, 3, 3, pagecore::Color{18, 52, 86, 255}, 0),
            "<use> decoder should rasterize the <defs>-only shape it references");
}

void test_svg_decoder_renders_nested_clip_path()
{
    const auto decoded = pagecore::decode_image_rgba(
        R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="10" height="10" viewBox="0 0 10 10">
  <clipPath id="outer">
    <clipPath id="inner"><circle cx="5" cy="5" r="5"/></clipPath>
    <rect x="2" y="2" width="6" height="6"/>
  </clipPath>
  <rect width="10" height="10" fill="#654321" clip-path="url(#outer)"/>
</svg>)SVG");
    require(decoded->width == 10 && decoded->height == 10, "nested clipPath SVG should use intrinsic dimensions");
    require(pixel_close(decoded->rgba, decoded->width, 5, 5, pagecore::Color{101, 67, 33, 255}, 0),
            "nested clipPath decoder should paint fill inside the clip region");
    require(pixel_close(decoded->rgba, decoded->width, 0, 0, pagecore::Color{0, 0, 0, 0}, 0),
            "nested clipPath decoder should leave pixels outside the clip region transparent");
}
#endif

void test_cairo_raster_handles_nonfinite_coordinates()
{
    pagecore::DisplayList display_list;
    display_list.viewport = pagecore::Viewport{32, 32, 1.0f};
    const float nan_value = std::numeric_limits<float>::quiet_NaN();
    const float inf_value = std::numeric_limits<float>::infinity();

    display_list.commands.emplace_back(pagecore::SolidFillCommand{
        pagecore::Rect{nan_value, inf_value, 1e30f, -inf_value},
        pagecore::Color{10, 20, 30, 255},
        false,
    });
    pagecore::LinearGradientCommand gradient;
    gradient.rect = pagecore::Rect{0, 0, 32, 32};
    gradient.start = pagecore::Point{nan_value, 0};
    gradient.end = pagecore::Point{inf_value, 10};
    gradient.stops.push_back(pagecore::GradientStop{0.0f, pagecore::Color{0, 0, 0, 255}});
    gradient.stops.push_back(pagecore::GradientStop{1.0f, pagecore::Color{255, 255, 255, 255}});
    display_list.commands.emplace_back(gradient);

    auto raster = pagecore::create_default_raster_backend(pagecore::Color{255, 255, 255, 255});
    const auto image = raster->render(display_list); // must not crash or invoke UB
    require(image.width == 32 && image.height == 32,
            "raster must survive NaN/Inf coordinates without UB");

    const std::string json = pagecore::display_list_to_json(display_list);
    require(json.find("nan") == std::string::npos
                && json.find("inf") == std::string::npos
                && json.find("NaN") == std::string::npos,
            "display-list JSON must not emit non-finite tokens");
}

void test_background_tiling_is_bounded()
{
    auto tile_image = std::make_shared<pagecore::DecodedImage>();
    tile_image->width = 1;
    tile_image->height = 1;
    tile_image->rgba = {0, 128, 255, 255};

    pagecore::DisplayList display_list;
    display_list.viewport = pagecore::Viewport{64, 64, 1.0f};
    pagecore::ImageCommand command;
    command.rect = pagecore::Rect{0, 0, 2000.0f, 2000000.0f}; // enormous element box
    command.tile = pagecore::Rect{0, 0, 1.0f, 1.0f};          // 1px tile
    command.repeat = pagecore::ImageRepeat::Repeat;
    command.image = tile_image;
    display_list.commands.emplace_back(command);

    auto raster = pagecore::create_default_raster_backend(pagecore::Color{255, 255, 255, 255});
    const auto image = raster->render(display_list); // ~4e9 tiles before the fix; bounded now
    require(image.width == 64 && image.height == 64,
            "tiling must be bounded by the surface, not the element box");
    require(pixel_matches(image, 10, 10, pagecore::Color{0, 128, 255, 255}),
            "bounded tiling should still fill the visible area");
}

void test_cairo_raster_shares_decoded_image_surface()
{
    // The same DecodedImage is referenced by two ImageCommands (a tiled
    // background plus a foreground <img>). The render pass converts it to a
    // premultiplied surface once and reuses it; both draws must be correct and
    // the shared cached surface must stay valid for the second command.
    auto shared = std::make_shared<pagecore::DecodedImage>();
    shared->width = 2;
    shared->height = 2;
    shared->rgba = {
        10, 20, 30, 255, 40, 50, 60, 255,
        70, 80, 90, 255, 100, 110, 120, 255};

    pagecore::DisplayList display_list;
    display_list.viewport = pagecore::Viewport{64, 64, 1.0f};

    pagecore::ImageCommand background;
    background.rect = pagecore::Rect{0, 0, 32.0f, 32.0f};
    background.tile = pagecore::Rect{0, 0, 2.0f, 2.0f};
    background.repeat = pagecore::ImageRepeat::Repeat;
    background.image = shared;
    display_list.commands.emplace_back(background);

    pagecore::ImageCommand foreground;
    foreground.rect = pagecore::Rect{40, 40, 2.0f, 2.0f};
    foreground.tile = pagecore::Rect{40, 40, 2.0f, 2.0f};
    foreground.repeat = pagecore::ImageRepeat::NoRepeat;
    foreground.image = shared; // same pointer -> exercises the surface cache
    display_list.commands.emplace_back(foreground);

    auto raster = pagecore::create_default_raster_backend(pagecore::Color{255, 255, 255, 255});
    const auto image = raster->render(display_list); // must not crash / corrupt

    require(image.width == 64 && image.height == 64, "shared-image render must keep the viewport size");
    // Top-left pixel of both the tiled background and the standalone image maps
    // to the image's top-left texel.
    require(pixel_matches(image, 0, 0, pagecore::Color{10, 20, 30, 255}),
            "tiled background must draw the shared decoded image");
    require(pixel_matches(image, 40, 40, pagecore::Color{10, 20, 30, 255}),
            "foreground image reusing the cached surface must draw identically");
    require(pixel_matches(image, 41, 41, pagecore::Color{100, 110, 120, 255}),
            "cached surface must preserve all texels for the reusing command");
}

void test_cairo_raster_opaque_image_roundtrip()
{
    // An opaque colored image drawn 1:1 must survive the RGBA->premultiplied
    // ARGB conversion and the surface_to_rgba read-back exactly (alpha == 255 is
    // the identity path for both premultiply and unpremultiply).
    const pagecore::Color opaque{37, 149, 214, 255};
    auto solid = std::make_shared<pagecore::DecodedImage>();
    solid->width = 4;
    solid->height = 4;
    solid->rgba.reserve(static_cast<std::size_t>(4 * 4 * 4));
    for (int i = 0; i < 16; ++i) {
        solid->rgba.push_back(opaque.r);
        solid->rgba.push_back(opaque.g);
        solid->rgba.push_back(opaque.b);
        solid->rgba.push_back(opaque.a);
    }

    pagecore::DisplayList display_list;
    display_list.viewport = pagecore::Viewport{4, 4, 1.0f};
    pagecore::ImageCommand command;
    command.rect = pagecore::Rect{0, 0, 4.0f, 4.0f};
    command.tile = pagecore::Rect{0, 0, 4.0f, 4.0f};
    command.repeat = pagecore::ImageRepeat::NoRepeat;
    command.image = solid;
    display_list.commands.emplace_back(command);

    auto raster = pagecore::create_default_raster_backend(pagecore::Color{255, 255, 255, 255});
    const auto image = raster->render(display_list);

    require(image.width == 4 && image.height == 4, "opaque image render must match the viewport");
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            require(pixel_matches(image, x, y, opaque),
                    "opaque image pixels must round-trip exactly through premultiply/unpremultiply");
        }
    }
}

void test_cairo_raster_zero_area_background_paints_nothing()
{
    // Regression: a collapsed, zero-width element carrying a background sprite
    // (its border box and clip box are zero-area, but the origin/tile box is a
    // full sprite sheet) must paint nothing. Previously the empty boxes skipped
    // installing a Cairo clip, and the tiling loop still emitted a single
    // origin-box-sized tile, smearing the whole sprite across the page — the
    // "icon soup" seen on google.com's collapsed search buttons.
    const pagecore::Color sprite_color{200, 0, 0, 255};
    auto sprite = std::make_shared<pagecore::DecodedImage>();
    sprite->width = 2;
    sprite->height = 2;
    sprite->rgba = {
        200, 0, 0, 255, 200, 0, 0, 255,
        200, 0, 0, 255, 200, 0, 0, 255};

    pagecore::DisplayList display_list;
    display_list.viewport = pagecore::Viewport{64, 64, 1.0f};

    pagecore::ImageCommand command;
    command.rect = pagecore::Rect{20, 20, 0.0f, 30.0f};  // zero-width border box
    command.clip = pagecore::Rect{20, 20, 0.0f, 30.0f};  // zero-width clip box
    command.tile = pagecore::Rect{20, 0, 40.0f, 60.0f};  // full sprite tile
    command.repeat = pagecore::ImageRepeat::Repeat;
    command.image = sprite;
    display_list.commands.emplace_back(command);

    auto raster = pagecore::create_default_raster_backend(pagecore::Color{255, 255, 255, 255});
    const auto image = raster->render(display_list);

    require(image.width == 64 && image.height == 64, "zero-area image render must keep the viewport size");
    require(!image_has_pixel(image, sprite_color),
            "a zero-area element's background sprite must not paint anywhere");
    require(pixel_matches(image, 30, 30, pagecore::Color{255, 255, 255, 255}),
            "the tile area of a collapsed element must stay the background color");
}

void test_litehtml_text_width_is_deterministic()
{
    // Exercises the width cache heterogeneous lookup and the create_font metrics
    // path that now reuses the shared measurement layout: identical text must
    // measure to one identical width, repeated renders must reproduce it, and
    // real glyphs must still rasterize (metrics not degraded).
    const char* html = R"HTML(
<html><head><style>
  body { margin: 0; font-family: sans-serif; font-size: 20px; }
  p { margin: 0; }
</style></head><body>
  <p>Deterministic</p>
  <p>Deterministic</p>
  <p>Deterministic</p>
</body></html>
)HTML";

    const auto widths_for = [&]() {
        pagecore::Page page;
        page.load_html(html, "https://example.test/index.html");
        pagecore::RenderOptions options;
        options.viewport = pagecore::Viewport{320, 240, 1.0f};
        const auto& dl = page.display_list(options);
        std::vector<float> widths;
        for (const auto& command : dl.commands) {
            if (const auto* text = std::get_if<pagecore::TextCommand>(&command)) {
                if (text->text.find("Deterministic") != std::string::npos) {
                    widths.push_back(text->rect.width);
                }
            }
        }
        return widths;
    };

    const auto first = widths_for();
    require(first.size() >= 3, "each identical paragraph should emit its own text run");
    for (const float w : first) {
        require(w > 0.0f, "measured text width must be positive");
        require(w == first.front(),
                "identical text must measure to an identical width (width-cache determinism)");
    }

    const auto second = widths_for();
    require(second == first, "a fresh render of the same page must reproduce identical text widths");

    pagecore::Page page;
    page.load_html(html, "https://example.test/index.html");
    pagecore::RenderOptions options;
    options.viewport = pagecore::Viewport{320, 240, 1.0f};
    const auto image = page.render(options);
    require(image_has_non_solid_text_pixel(image),
            "text must render as anti-aliased glyphs after measurement-layout reuse");
}

void test_image_decoder_rejects_malformed_input()
{
    const auto expect_throws = [](std::string_view bytes, const char* what) {
        bool threw = false;
        try {
            (void) pagecore::decode_image_rgba(bytes);
        } catch (const std::exception&) {
            threw = true;
        }
        require(threw, std::string("decode_image_rgba must reject malformed input without crashing: ") + what);
    };

    expect_throws("", "empty input");
    expect_throws("this is definitely not an image at all", "non-image bytes");

    const auto png = png_body(pagecore::Color{10, 20, 30, 255});
    expect_throws(std::string_view(png).substr(0, png.size() / 3), "truncated PNG");
    const auto jpeg = jpeg_body();
    expect_throws(std::string_view(jpeg).substr(0, jpeg.size() / 3), "truncated JPEG");
    const auto gif = gif_body();
    expect_throws(std::string_view(gif).substr(0, gif.size() / 3), "truncated GIF");
#if PAGECORE_ENABLE_WEBP
    const auto webp = webp_body(pagecore::Color{10, 20, 30, 255});
    expect_throws(std::string_view(webp).substr(0, webp.size() / 3), "truncated WebP");
#endif
}

void test_cairo_raster_and_io_error_paths()
{
    // An oversized surface (beyond Cairo's dimension limit) must fail cleanly
    // rather than proceed with an error-state surface.
    pagecore::DisplayList display_list;
    display_list.viewport = pagecore::Viewport{100000, 100000, 1.0f};
    display_list.commands.emplace_back(pagecore::SolidFillCommand{
        pagecore::Rect{0, 0, 10, 10}, pagecore::Color{0, 0, 0, 255}, false});
    auto raster = pagecore::create_default_raster_backend(pagecore::Color{255, 255, 255, 255});
    bool render_threw = false;
    try {
        (void) raster->render(display_list);
    } catch (const std::exception&) {
        render_threw = true;
    }
    require(render_threw, "rendering an oversized surface must throw, not read an error-state surface");

    // Writing a PNG to an unwritable path must throw cleanly.
    pagecore::RenderedImage image;
    image.width = 2;
    image.height = 2;
    image.rgba.assign(static_cast<std::size_t>(2 * 2 * 4), 255);
    bool write_threw = false;
    try {
        pagecore::write_png_rgba(image, "/pagecore-nonexistent-directory/out.png");
    } catch (const std::exception&) {
        write_threw = true;
    }
    require(write_threw, "writing a PNG to an unwritable path must throw");
}
#endif

void test_deep_dom_traversal_is_iterative()
{
    // Build a deep tree directly through the DOM API (no JS / HTML parse
    // overhead) to exercise the now-iterative traversals. Lexbor's own deep-tree
    // operations are super-linear, so the depth is kept moderate; the iterative
    // rewrite removes the native-stack depth limit regardless of this value.
    pagecore::DomDocument doc;
    doc.parse("<html><body></body></html>");
    const pagecore::NodeId body = doc.body();
    const pagecore::NodeId root = doc.create_element("div");
    doc.append_child(body, root);

    const int depth = 15000;
    pagecore::NodeId cur = root;
    for (int i = 0; i < depth; ++i) {
        const pagecore::NodeId child = doc.create_element("div");
        doc.append_child(cur, child);
        cur = child;
    }
    doc.append_child(cur, doc.create_text_node("deep"));

    // append_descendant_text over a 200000-deep subtree (was recursive).
    const std::string content = doc.text_content(root);
    require(content.find("deep") != std::string::npos,
            "text_content over a deeply nested tree must not overflow the native stack");

    // collect_subtree via set_inner_html over the deep subtree (was recursive).
    doc.set_inner_html(root, "<span>x</span>");
    require(doc.query_selector(root, "span") != pagecore::kInvalidNodeId,
            "set_inner_html over a deep subtree must not overflow the native stack");
}

void test_set_inner_html_forgets_old_subtree_ids()
{
    // Regression: set_inner_html must read/forget the old subtree BEFORE Lexbor
    // destroys it. Reading those raw pointers afterwards is a use-after-free, and
    // forgetting them by the freed pointer erased the wrong id (or none), leaving
    // the old ids dangling in id_to_node -> has_node(old) wrongly true -> the next
    // require_node(old) handed back a freed node (second UAF).
    pagecore::DomDocument doc;
    doc.parse("<html><body><div id='host'>"
              "<span id='old'><b id='deep'>x</b></span>"
              "<style id='sheet'>.a{color:red}</style>"
              "</div></body></html>");

    const pagecore::NodeId host = doc.get_element_by_id("host");
    const pagecore::NodeId old_span = doc.get_element_by_id("old");
    const pagecore::NodeId deep = doc.get_element_by_id("deep");
    const pagecore::NodeId sheet = doc.get_element_by_id("sheet");
    require(host != pagecore::kInvalidNodeId, "fixture #host must resolve");
    require(doc.has_node(old_span) && doc.has_node(deep) && doc.has_node(sheet),
            "old subtree ids must be live before the innerHTML replacement");

    const std::uint64_t forget_before = doc.forget_version();

    doc.set_inner_html(host, "<p id='fresh'>new</p>");

    require(!doc.has_node(old_span), "old child id must be forgotten after innerHTML replace");
    require(!doc.has_node(deep), "deep descendant id must be forgotten after innerHTML replace");
    require(!doc.has_node(sheet), "old <style> child id must be forgotten after innerHTML replace");
    require(doc.forget_version() > forget_before,
            "forgetting invalidated ids must bump forget_version");

    require(doc.has_node(host), "the host element id must survive its own innerHTML write");
    const pagecore::NodeId fresh = doc.get_element_by_id("fresh");
    require(fresh != pagecore::kInvalidNodeId && doc.has_node(fresh),
            "new innerHTML content must be addressable");
    require(doc.text_content(host) == "new", "host text must reflect the new innerHTML");

    // Reuse the freed memory with fresh allocations; a corrupted id map would now
    // mis-resolve the stale ids.
    for (int i = 0; i < 64; ++i) {
        (void) doc.create_element("div");
    }
    require(!doc.has_node(old_span) && !doc.has_node(deep) && !doc.has_node(sheet),
            "stale ids must stay forgotten even after new allocations reuse freed memory");
}

void test_deep_clone_assigns_fresh_subtree_ids()
{
    // Ids are stored in each node's lxb_dom_node_t.user slot, which
    // lxb_dom_node_clone copies onto the clone and every descendant. clone_node
    // must reset the cloned subtree so clones get fresh ids instead of aliasing
    // the originals' — otherwise mutating a clone would corrupt the source.
    pagecore::DomDocument doc;
    doc.parse("<html><body><div id='a'><span>x</span></div></body></html>");

    const pagecore::NodeId original = doc.get_element_by_id("a");
    require(original != pagecore::kInvalidNodeId, "fixture #a must resolve");
    const pagecore::NodeId original_span = doc.query_selector(original, "span");
    require(original_span != pagecore::kInvalidNodeId, "fixture span must resolve");

    const pagecore::NodeId clone = doc.clone_node(original, /*deep=*/true);
    require(clone != original, "deep clone must get a fresh id, not the source's");

    const pagecore::NodeId clone_span = doc.query_selector(clone, "span");
    require(clone_span != pagecore::kInvalidNodeId, "cloned span must resolve");
    require(clone_span != original_span,
            "cloned descendant must get a fresh id; aliasing means the user slot was not reset");

    // Independence: mutating the clone's span must not touch the original's.
    doc.set_text_content(clone_span, "y");
    require(doc.text_content(original_span) == "x",
            "mutating the cloned span must leave the original span unchanged");
    require(doc.text_content(clone_span) == "y", "cloned span mutation must apply");
}

void test_dom_layout_mutation_version_ignores_service_attributes()
{
    pagecore::DomDocument doc;
    doc.parse("<html><body><div id='x' class='a' style='width:10px'></div></body></html>");

    const pagecore::NodeId node = doc.get_element_by_id("x");
    require(node != pagecore::kInvalidNodeId, "fixture #x must resolve");

    const auto initial_mutation_version = doc.mutation_version();
    const auto initial_layout_version = doc.layout_mutation_version();

    doc.set_attribute(node, "data-state", "ready");
    require(
        doc.mutation_version() == initial_mutation_version + 1,
        "service attribute writes still bump the DOM mutation version");
    require(
        doc.layout_mutation_version() == initial_layout_version,
        "data-* writes must not invalidate layout/style cache");

    doc.set_attribute(node, "aria-label", "Ready");
    require(
        doc.layout_mutation_version() == initial_layout_version,
        "aria-* writes must not invalidate layout/style cache");

    doc.remove_attribute(node, "data-state");
    require(
        doc.layout_mutation_version() == initial_layout_version,
        "removing data-* must not invalidate layout/style cache");

    doc.set_layout_sensitive_attributes({"data-state"});
    doc.set_attribute(node, "data-state", "ready");
    require(
        doc.layout_mutation_version() == initial_layout_version + 1,
        "data-* writes must invalidate layout/style cache when an attribute selector depends on them");
    require(
        doc.self_dirty_layout_version(node) == doc.layout_mutation_version(),
        "a selector-relevant attribute mutation should self-dirty the changed node");

    const auto selected_layout_version = doc.layout_mutation_version();
    doc.set_attribute(node, "aria-label", "Still service metadata");
    require(
        doc.layout_mutation_version() == selected_layout_version,
        "unselected service attributes must still avoid layout/style invalidation");

    doc.set_attribute(node, "class", "b");
    require(
        doc.layout_mutation_version() == selected_layout_version + 1,
        "class writes must invalidate layout/style cache");
    require(
        doc.self_dirty_layout_version(node) == doc.layout_mutation_version(),
        "a class mutation should self-dirty the changed node");

    const auto class_layout_version = doc.layout_mutation_version();
    doc.set_text_content(node, "changed");
    require(
        doc.layout_mutation_version() == class_layout_version + 1,
        "text mutations must invalidate layout/style cache");

    const auto text_layout_version = doc.layout_mutation_version();
    const pagecore::NodeId child = doc.create_element("span");
    const pagecore::NodeId appended = doc.append_child(node, child);
    require(
        doc.layout_mutation_version() == text_layout_version + 1,
        "append_child must invalidate layout/style cache");
    require(
        doc.self_dirty_layout_version(appended) == doc.layout_mutation_version(),
        "append_child should self-dirty the appended node");

    doc.parse("<html><body><script id='s'>var a = 1;</script><div id='x'>layout</div></body></html>");
    const pagecore::NodeId script = doc.get_element_by_id("s");
    const auto script_layout_version = doc.layout_mutation_version();
    doc.set_text_content(script, "var a = 2;");
    require(
        doc.layout_mutation_version() == script_layout_version,
        "script text mutations must not invalidate layout/style cache");
}

void test_dom_layout_mutation_journal_records_inline_style()
{
    pagecore::DomDocument doc;
    doc.parse("<html><body><div id='x' style='color:red'></div></body></html>");
    const pagecore::NodeId x = doc.get_element_by_id("x");

    const auto base = doc.layout_mutation_version();
    doc.set_attribute(x, "style", "color:blue;width:10px");

    auto journal = doc.layout_mutations_since(base);
    require(journal.complete, "journal must be complete for a recent version");
    require(journal.records.size() == 1, "one inline-style write must produce one record");
    const auto& record = journal.records.front();
    require(record.kind == pagecore::LayoutMutationRecord::Kind::InlineStyle, "record kind must be InlineStyle");
    require(record.node == x, "record must carry the mutated node");
    require(record.layout_mutation_version == doc.layout_mutation_version(),
            "record version must equal the post-mutation layout version");
    require(record.had_old_value && record.old_value == "color:red", "record must capture the pre-mutation style");
    require(record.has_new_value && record.new_value == "color:blue;width:10px",
            "record must capture the post-mutation style");

    // Removing the attribute records a value-less InlineStyle entry.
    const auto after_set = doc.layout_mutation_version();
    doc.remove_attribute(x, "style");
    auto removal = doc.layout_mutations_since(after_set);
    require(removal.records.size() == 1, "removing style must record one entry");
    require(removal.records.front().kind == pagecore::LayoutMutationRecord::Kind::InlineStyle,
            "style removal must be InlineStyle");
    require(removal.records.front().had_old_value && removal.records.front().old_value == "color:blue;width:10px",
            "style removal must capture the old value");
    require(!removal.records.front().has_new_value, "style removal must have no new value");
}

void test_dom_layout_mutation_journal_records_inline_style_from_js()
{
    pagecore::Page page;
    page.load_html("<html><body><div id='x' style='color:red'>hi</div></body></html>", "https://example.test/");
    const pagecore::NodeId x = page.document().get_element_by_id("x");

    const auto base = page.document().layout_mutation_version();
    page.eval("document.getElementById('x').style.width = '25px'");

    auto journal = page.document().layout_mutations_since(base);
    require(journal.complete, "journal must be complete after a JS inline-style write");
    require(!journal.records.empty(), "a JS inline-style write must be journaled");
    const auto& record = journal.records.back();
    require(record.kind == pagecore::LayoutMutationRecord::Kind::InlineStyle,
            "el.style.width writes through setAttribute('style',...) and must journal as InlineStyle");
    require(record.node == x, "record must carry the mutated node");
    require(record.has_new_value && record.new_value.find("width") != std::string::npos,
            "record must capture the new style text containing the width");
}

void test_dom_layout_mutation_journal_other_kinds()
{
    pagecore::DomDocument doc;
    doc.parse("<html><body><div id='x'>hi</div></body></html>");
    const pagecore::NodeId x = doc.get_element_by_id("x");

    const auto base = doc.layout_mutation_version();
    doc.set_attribute(x, "class", "wide");                 // not style → Other
    doc.append_child(x, doc.create_element("span"));        // structural → Other

    auto journal = doc.layout_mutations_since(base);
    require(journal.complete, "journal must be complete");
    require(journal.records.size() == 2, "two layout mutations must produce two records");
    for (const auto& record : journal.records) {
        require(record.kind == pagecore::LayoutMutationRecord::Kind::Other,
                "non-inline-style layout mutations must be Kind::Other");
        require(!record.had_old_value && !record.has_new_value,
                "Other records must not carry style values");
    }
}

void test_dom_layout_mutation_journal_ignores_non_layout_mutations()
{
    pagecore::DomDocument doc;
    doc.parse("<html><body><div id='x'></div></body></html>");
    const pagecore::NodeId x = doc.get_element_by_id("x");

    const auto base = doc.layout_mutation_version();
    doc.set_attribute(x, "data-state", "ready");   // service attribute: no layout bump
    doc.set_attribute(x, "aria-label", "Ready");   // service attribute: no layout bump

    auto journal = doc.layout_mutations_since(base);
    require(journal.complete, "journal must be complete");
    require(journal.records.empty(), "service-attribute writes must not be journaled");
    require(doc.layout_mutation_version() == base, "service-attribute writes must not bump the layout version");
}

void test_dom_layout_mutation_journal_ring_completeness()
{
    pagecore::DomDocument doc;
    doc.parse("<html><body><div id='x' style='width:0px'></div></body></html>");
    const pagecore::NodeId x = doc.get_element_by_id("x");

    const auto base = doc.layout_mutation_version();

    // 65 layout mutations overflow the 64-entry ring by one.
    for (int i = 0; i < 65; ++i) {
        doc.set_attribute(x, "style", "width:" + std::to_string(i) + "px");
    }

    // A query from the original base can no longer be answered completely.
    auto from_base = doc.layout_mutations_since(base);
    require(!from_base.complete, "a query older than the retained window must report incomplete");
    require(from_base.records.size() == 64, "the ring must retain exactly its capacity of records");

    // A query from within the retained window is complete.
    const auto recent = doc.layout_mutation_version() - 3;
    auto from_recent = doc.layout_mutations_since(recent);
    require(from_recent.complete, "a query within the retained window must be complete");
    require(from_recent.records.size() == 3, "exactly the three most recent records must be returned");
    require(from_recent.records.back().new_value == "width:64px", "the last record must reflect the final write");
}

void test_dom_is_layout_sensitive_attribute()
{
    pagecore::DomDocument doc;
    doc.parse("<html><body><div id='x'></div></body></html>");

    require(!doc.is_layout_sensitive_attribute("style"),
            "style must not be layout-sensitive without a matching attribute selector");

    doc.set_layout_sensitive_attributes({"style", "data-role"});
    require(doc.is_layout_sensitive_attribute("style"),
            "style becomes layout-sensitive once a [style...] selector is registered");
    require(doc.is_layout_sensitive_attribute("STYLE"), "attribute-name matching must be ASCII case-insensitive");
    require(doc.is_layout_sensitive_attribute("data-role"), "registered selector attributes must be layout-sensitive");
    require(!doc.is_layout_sensitive_attribute("class"), "unregistered attributes must not be layout-sensitive");

    doc.set_layout_sensitive_attributes({}, /*wildcard=*/true);
    require(doc.is_layout_sensitive_attribute("style"), "the wildcard makes every attribute layout-sensitive");
    require(doc.is_layout_sensitive_attribute("anything"), "the wildcard makes every attribute layout-sensitive");
}

struct RecordingLayoutTreeVisitor : pagecore::DomDocument::LayoutTreeVisitor {
    struct Event {
        std::string kind;  // "enter", "leave", "text", "raw_text", "comment"
        pagecore::NodeId id = pagecore::kInvalidNodeId;
        std::string tag;
        std::vector<pagecore::DomDocument::Attribute> attributes;
        std::string text;
    };

    std::vector<Event> events;

    void enter_element(
        pagecore::NodeId id,
        std::string_view tag_name,
        const std::vector<pagecore::DomDocument::Attribute>& attributes) override
    {
        events.push_back(Event{"enter", id, std::string(tag_name), attributes, {}});
    }

    void leave_element(pagecore::NodeId id) override
    {
        events.push_back(Event{"leave", id, {}, {}, {}});
    }

    void text_run(std::string_view text, bool raw_run) override
    {
        events.push_back(Event{raw_run ? "raw_text" : "text", pagecore::kInvalidNodeId, {}, {}, std::string(text)});
    }

    void comment(std::string_view text) override
    {
        events.push_back(Event{"comment", pagecore::kInvalidNodeId, {}, {}, std::string(text)});
    }

    const Event* find_enter(std::string_view tag) const
    {
        for (const auto& event : events) {
            if (event.kind == "enter" && event.tag == tag) {
                return &event;
            }
        }
        return nullptr;
    }

    bool has_text(std::string_view text) const
    {
        for (const auto& event : events) {
            if ((event.kind == "text" || event.kind == "raw_text") && event.text == text) {
                return true;
            }
        }
        return false;
    }
};

std::optional<std::string> visitor_attribute(
    const RecordingLayoutTreeVisitor::Event& event,
    std::string_view name)
{
    for (const auto& attribute : event.attributes) {
        if (attribute.name == name) {
            return attribute.value;
        }
    }
    return std::nullopt;
}

void test_dom_visit_layout_tree_structure_and_attributes()
{
    pagecore::DomDocument doc;
    doc.parse(
        "<!DOCTYPE html><html><head><style>p{color:red}</style></head><body>"
        "<div id='a' class='b' style='color:red'>Hi <span>x</span></div>"
        "<script>var s = \"a b\";</script>"
        "</body></html>");

    RecordingLayoutTreeVisitor visitor;
    doc.visit_layout_tree(visitor);

    require(!visitor.events.empty(), "visitor must receive events");
    require(visitor.events.front().kind == "enter" && visitor.events.front().tag == "html",
            "walk must start at the html element (doctype skipped)");
    require(visitor.events.front().id == doc.document_element(),
            "html enter event must carry the document element id");

    const auto* div = visitor.find_enter("div");
    require(div != nullptr, "div must be visited");
    require(div->id == doc.get_element_by_id("a"), "div id must match the DOM NodeId");
    require(visitor_attribute(*div, "id") == std::optional<std::string>("a"), "div id attribute reported");
    require(visitor_attribute(*div, "class") == std::optional<std::string>("b"), "div class attribute reported");
    require(visitor_attribute(*div, "style") == std::optional<std::string>("color:red"),
            "div style attribute reported");

    require(visitor.has_text("Hi "), "text run inside div must be reported verbatim");

    bool saw_raw_script_text = false;
    for (const auto& event : visitor.events) {
        if (event.kind == "raw_text" && event.text == "var s = \"a b\";") {
            saw_raw_script_text = true;
        }
    }
    require(saw_raw_script_text, "script text must be reported as one raw run");

    int depth = 0;
    for (const auto& event : visitor.events) {
        if (event.kind == "enter") {
            ++depth;
        } else if (event.kind == "leave") {
            require(depth > 0, "leave events must not underflow");
            --depth;
        }
    }
    require(depth == 0, "enter/leave events must be balanced");
}

void test_dom_visit_layout_tree_omits_noscript_and_head_text()
{
    const std::string_view html =
        "<html><head><title>t</title></head><body>"
        "<noscript><p>fallback</p></noscript><div>content</div>"
        "</body></html>";

    pagecore::DomDocument doc;
    doc.parse(html);

    // Text directly inside <head> can only appear via DOM mutation.
    doc.append_child(doc.head(), doc.create_text_node("stray head text"));

    RecordingLayoutTreeVisitor with_js;
    doc.visit_layout_tree(with_js, /*omit_js_disabled_content=*/true);
    require(with_js.find_enter("noscript") == nullptr, "noscript subtree must be omitted when JS is enabled");
    require(!with_js.has_text("fallback"), "noscript content must be omitted when JS is enabled");
    require(!with_js.has_text("stray head text"), "direct head text must always be omitted");
    require(with_js.has_text("content"), "regular content must be visited");

    // With the parser scripting flag off (JS-disabled pages), <noscript>
    // content parses as real elements and the walk visits them.
    pagecore::DomDocument no_js_doc;
    no_js_doc.set_scripting_enabled(false);
    no_js_doc.parse(html);
    no_js_doc.append_child(no_js_doc.head(), no_js_doc.create_text_node("stray head text"));

    RecordingLayoutTreeVisitor without_js;
    no_js_doc.visit_layout_tree(without_js, /*omit_js_disabled_content=*/false);
    require(without_js.find_enter("noscript") != nullptr, "noscript must be visited when JS is disabled");
    require(without_js.find_enter("p") != nullptr, "noscript fallback elements must be visited when JS is disabled");
    require(without_js.has_text("fallback"), "noscript content must be visited when JS is disabled");
    require(!without_js.has_text("stray head text"), "direct head text must be omitted regardless of JS mode");
}

void test_dom_visit_layout_tree_merges_style_overrides()
{
    pagecore::DomDocument doc;
    doc.parse(
        "<html><body>"
        "<div id='styled' style='color:red'></div>"
        "<div id='bare'></div>"
        "</body></html>");

    const pagecore::NodeId styled = doc.get_element_by_id("styled");
    const pagecore::NodeId bare = doc.get_element_by_id("bare");

    std::vector<pagecore::DomDocument::LayoutStyleOverride> overrides;
    overrides.push_back({styled, "width:10px"});
    overrides.push_back({bare, "height:5px"});
    overrides.push_back({pagecore::kInvalidNodeId, "ignored:1"});
    overrides.push_back({styled + bare + 1000, "ignored:2"});

    RecordingLayoutTreeVisitor visitor;
    doc.visit_layout_tree(visitor, false, overrides);

    bool checked_styled = false;
    bool checked_bare = false;
    for (const auto& event : visitor.events) {
        if (event.kind != "enter") {
            continue;
        }
        if (event.id == styled) {
            require(visitor_attribute(event, "style") == std::optional<std::string>("color:red;width:10px"),
                    "override must merge after the existing inline style");
            checked_styled = true;
        }
        if (event.id == bare) {
            require(visitor_attribute(event, "style") == std::optional<std::string>("height:5px"),
                    "override must synthesize a style attribute when absent");
            checked_bare = true;
        }
    }
    require(checked_styled && checked_bare, "both override targets must be visited");

    require(doc.get_attribute(styled, "style") == std::optional<std::string>("color:red"),
            "overrides must not leak into the DOM");
}

void test_dom_visit_layout_tree_coalesces_adjacent_text_runs()
{
    pagecore::DomDocument doc;
    doc.parse("<html><body><p id='p'></p><p id='q'></p></body></html>");

    const pagecore::NodeId p = doc.get_element_by_id("p");
    doc.append_child(p, doc.create_text_node("fo"));
    doc.append_child(p, doc.create_text_node("o"));
    doc.append_child(p, doc.create_comment("split"));
    doc.append_child(p, doc.create_text_node("bar"));

    const pagecore::NodeId q = doc.get_element_by_id("q");
    const pagecore::NodeId noscript = doc.create_element("noscript");
    doc.append_child(q, doc.create_text_node("a"));
    doc.append_child(q, noscript);
    doc.append_child(q, doc.create_text_node("b"));

    RecordingLayoutTreeVisitor visitor;
    doc.visit_layout_tree(visitor, /*omit_js_disabled_content=*/true);

    std::vector<std::string> texts;
    for (const auto& event : visitor.events) {
        if (event.kind == "text") {
            texts.push_back(event.text);
        }
    }
    require(texts.size() == 3, "expected exactly three coalesced text runs");
    require(texts[0] == "foo", "adjacent text nodes must coalesce into one run");
    require(texts[1] == "bar", "comments must break text runs");
    require(texts[2] == "ab", "text separated only by detached nodes must coalesce");

    bool saw_comment = false;
    for (const auto& event : visitor.events) {
        if (event.kind == "comment" && event.text == "split") {
            saw_comment = true;
        }
    }
    require(saw_comment, "comments must be reported");
}

void test_dom_visit_layout_tree_skips_template_subtrees()
{
    pagecore::DomDocument doc;
    doc.parse(
        "<html><body><div id='d'>a<template id='t'><p>tpl text</p></template>b</div></body></html>");

    RecordingLayoutTreeVisitor visitor;
    doc.visit_layout_tree(visitor);

    require(visitor.find_enter("template") == nullptr, "inert template elements must not be visited");
    require(visitor.find_enter("p") == nullptr, "template content must not be visited");
    require(!visitor.has_text("tpl text"), "template content text must not be visited");

    // The skipped template is still an element boundary: the surrounding text
    // nodes must stay separate runs (a serialize/re-parse round trip keeps
    // them apart because the template element remains in the markup).
    require(visitor.has_text("a") && visitor.has_text("b"),
            "text around a template must be visited as separate runs");
    require(!visitor.has_text("ab"), "text must not coalesce across a template element");
}

void test_dom_visit_layout_tree_does_not_mutate()
{
    pagecore::DomDocument doc;
    doc.parse("<html><body><div id='a' style='color:red'>x</div></body></html>");

    const auto mutation_version = doc.mutation_version();
    const auto layout_version = doc.layout_mutation_version();
    const auto stylesheet_generation = doc.stylesheet_generation();

    std::vector<pagecore::DomDocument::LayoutStyleOverride> overrides;
    overrides.push_back({doc.get_element_by_id("a"), "width:10px"});

    RecordingLayoutTreeVisitor visitor;
    doc.visit_layout_tree(visitor, true, overrides);

    require(doc.mutation_version() == mutation_version, "visit must not bump mutation_version");
    require(doc.layout_mutation_version() == layout_version, "visit must not bump layout_mutation_version");
    require(doc.stylesheet_generation() == stylesheet_generation, "visit must not bump stylesheet_generation");
    require(doc.serialize_html().find("data-pc-sid") == std::string::npos,
            "visit must not leave transient attributes in the DOM");
    require(doc.get_attribute(doc.get_element_by_id("a"), "style") == std::optional<std::string>("color:red"),
            "visit must not leave merged style overrides in the DOM");
}

void test_dom_quirks_mode_from_doctype()
{
    pagecore::DomDocument no_quirks;
    no_quirks.parse("<!DOCTYPE html><html><body></body></html>");
    require(no_quirks.quirks_mode() == pagecore::DomDocument::QuirksMode::NoQuirks,
            "a standard doctype must parse in no-quirks mode");

    pagecore::DomDocument quirks;
    quirks.parse("<html><body></body></html>");
    require(quirks.quirks_mode() == pagecore::DomDocument::QuirksMode::Quirks,
            "a missing doctype must parse in quirks mode");

    pagecore::DomDocument limited;
    limited.parse(
        "<!DOCTYPE HTML PUBLIC \"-//W3C//DTD XHTML 1.0 Transitional//EN\" "
        "\"http://www.w3.org/TR/xhtml1/DTD/xhtml1-transitional.dtd\">"
        "<html><body></body></html>");
    require(limited.quirks_mode() == pagecore::DomDocument::QuirksMode::LimitedQuirks,
            "a transitional doctype must parse in limited-quirks mode");
}

void test_document_compat_mode_reflects_dom_quirks_mode()
{
    pagecore::Page no_quirks;
    no_quirks.load_html("<!DOCTYPE html><html><body></body></html>", "https://example.test/index.html");
    require(no_quirks.eval("document.compatMode") == "CSS1Compat",
            "document.compatMode should be CSS1Compat in no-quirks mode");

    pagecore::Page quirks;
    quirks.load_html("<html><body></body></html>", "https://example.test/index.html");
    require(quirks.eval("document.compatMode") == "BackCompat",
            "document.compatMode should be BackCompat in quirks mode");

    pagecore::Page limited;
    limited.load_html(
        "<!DOCTYPE HTML PUBLIC \"-//W3C//DTD XHTML 1.0 Transitional//EN\" "
        "\"http://www.w3.org/TR/xhtml1/DTD/xhtml1-transitional.dtd\">"
        "<html><body></body></html>",
        "https://example.test/index.html");
    require(limited.eval("document.compatMode") == "CSS1Compat",
            "document.compatMode should be CSS1Compat in limited-quirks mode");
}

void test_layout_input_digest_superset_invariants()
{
    pagecore::DomDocument doc;
    doc.parse(
        "<html><head></head><body>"
        "<div id='a' class='x'><span id='s1'>t</span><span id='s2'></span></div>"
        "<div id='b'></div>"
        "</body></html>");

    const pagecore::NodeId a = doc.get_element_by_id("a");
    const pagecore::NodeId s1 = doc.get_element_by_id("s1");
    const pagecore::NodeId s2 = doc.get_element_by_id("s2");
    require(a != pagecore::kInvalidNodeId && s1 != pagecore::kInvalidNodeId && s2 != pagecore::kInvalidNodeId,
            "digest fixture nodes must resolve");

    auto digest = [&](pagecore::NodeId node, int vw = 1280, int vh = 720, float scale = 1.0f) {
        return doc.layout_input_digest(node, vw, vh, scale);
    };

    require(digest(s2) == digest(s2), "digest must be stable for identical DOM and viewport");
    require(digest(s2) != digest(s2, 640), "digest must differ on viewport width");
    require(digest(s2) != digest(s2, 1280, 480), "digest must differ on viewport height");
    require(digest(s2) != digest(s2, 1280, 720, 2.0f), "digest must differ on device scale factor");

    const auto before_service = digest(s2);
    doc.set_attribute(s2, "data-test", "value");
    require(digest(s2) == before_service, "non-selector service attribute change must NOT change the digest");

    const auto before_class = digest(s2);
    doc.set_attribute(s2, "class", "y");
    require(digest(s2) != before_class, "class change must change the digest");

    const auto before_style = digest(s2);
    doc.set_attribute(s2, "style", "color:red");
    require(digest(s2) != before_style, "inline style change must change the digest");

    const auto before_sibling_class = digest(s2);
    doc.set_attribute(s1, "class", "sibling");
    require(digest(s2) != before_sibling_class, "a sibling's class change must change the digest (sibling combinators)");

    const auto before_insert = digest(s2);
    const pagecore::NodeId inserted = doc.create_element("span");
    doc.insert_before(a, inserted, s2);
    require(digest(s2) != before_insert, "inserting a preceding sibling must change the digest (:nth-child/index)");

    const auto before_child = digest(s2);
    const pagecore::NodeId child = doc.create_element("em");
    doc.append_child(s2, child);
    require(digest(s2) != before_child, "adding a first child must change the digest (:empty)");

    const auto before_stylesheet = digest(s2);
    const pagecore::NodeId style = doc.create_element("style");
    doc.append_child(doc.head(), style);
    doc.set_text_content(style, ".y { width: 5px; }");
    require(digest(s2) != before_stylesheet, "inserting a stylesheet must change every node's digest");
}

void test_subtree_dirty_epoch_tracks_descendant_mutations()
{
    pagecore::DomDocument doc;
    doc.parse(
        "<html><body>"
        "<div id='parent'><div id='mid'><div id='leaf'></div></div></div>"
        "<div id='other'></div>"
        "</body></html>");

    const pagecore::NodeId parent = doc.get_element_by_id("parent");
    const pagecore::NodeId mid = doc.get_element_by_id("mid");
    const pagecore::NodeId leaf = doc.get_element_by_id("leaf");
    const pagecore::NodeId other = doc.get_element_by_id("other");
    require(parent && mid && leaf && other, "dirty-epoch fixture nodes must resolve");

    const auto v0 = doc.layout_mutation_version();
    doc.set_attribute(leaf, "class", "z");
    const auto v1 = doc.layout_mutation_version();
    require(v1 > v0, "a class mutation must bump the layout version");

    require(doc.self_dirty_layout_version(leaf) == v1, "the mutated node must be self-dirty at the new version");
    require(doc.subtree_dirty_layout_version(leaf) == v1, "the mutated node must be subtree-dirty at the new version");
    require(doc.subtree_dirty_layout_version(mid) == v1, "an ancestor's subtree must be dirtied by a descendant mutation");
    require(doc.subtree_dirty_layout_version(parent) == v1, "every ancestor's subtree must be dirtied");
    require(doc.self_dirty_layout_version(mid) < v1, "an ancestor's own box must NOT be self-dirtied by a descendant mutation");
    require(doc.self_dirty_layout_version(parent) < v1, "an ancestor's own box must NOT be self-dirtied by a descendant mutation");
    require(doc.subtree_dirty_layout_version(other) < v1, "an unrelated subtree must not be dirtied");

    doc.set_attribute(parent, "class", "p");
    const auto v2 = doc.layout_mutation_version();
    require(doc.self_dirty_layout_version(parent) == v2, "an own-attribute mutation must self-dirty the node");
    require(doc.subtree_dirty_layout_version(leaf) == v1, "a descendant's subtree must be unchanged by an ancestor's own mutation");
}

void test_query_selector_cache_returns_all_and_first()
{
    pagecore::DomDocument doc;
    doc.parse(
        "<html><body>"
        "<section><p class='x'>1</p><p class='x'>2</p></section>"
        "<p class='x'>3</p>"
        "<span class='y'>4</span>"
        "</body></html>");
    const pagecore::NodeId root = doc.document_node();

    const auto all = doc.query_selector_all(root, ".x");
    require(all.size() == 3, "querySelectorAll should return every match");

    // Re-run identical and varying selectors to exercise the compiled-selector
    // cache (a stale/shared memory pool would corrupt subsequent results).
    const auto all_again = doc.query_selector_all(root, ".x");
    require(all_again == all, "cached querySelectorAll must return the same matches");
    require(doc.query_selector_all(root, ".y").size() == 1,
            "a second distinct selector must not be corrupted by the cache");
    require(doc.query_selector_all(root, ".x").size() == 3,
            "the first selector must still match after caching a second one");

    // Early-stop query_selector must yield the first match in document order.
    const pagecore::NodeId first = doc.query_selector(root, ".x");
    require(first == all.front(), "querySelector should return the first match in tree order");

    require(doc.query_selector(root, ".missing") == pagecore::kInvalidNodeId,
            "querySelector returns invalid when nothing matches");
    require(doc.query_selector_all(root, ".missing").empty(),
            "querySelectorAll returns empty when nothing matches");
}

void test_described_traversal_wraps_children_correctly()
{
    pagecore::Page page;
    page.load_html(
        "<html><body><div id=\"host\">a<span class=\"s\">b</span><!--c--><p>d</p></div></body></html>");

    // childNodes goes through the batched describe path: text, element, comment, element.
    // It is a NodeList, so it is iterable but has no Array methods.
    require(page.eval(
                "(() => { const k = document.getElementById('host').childNodes;"
                " return k.length + ':' + [...k].map(n => n.nodeType).join(','); })()") == "4:3,1,8,1",
            "childNodes returns every child with the correct node types in order");

    // children must surface only the element children, with correct tags.
    // It is an HTMLCollection, so it has no Array methods.
    require(page.eval(
                "[...document.getElementById('host').children].map(e => e.tagName).join(',')") == "SPAN,P",
            "children returns only element nodes with correct tag names");

    // A freshly created + appended element exercises describeNode for a new node:
    // it must be wrapped with the right HTMLElement subclass.
    require(page.eval(
                "(() => { const d = document.createElement('b'); document.body.appendChild(d);"
                " const kids = document.body.children; const last = kids[kids.length - 1];"
                " return last.tagName + ':' + (last instanceof HTMLElement); })()") == "B:true",
            "a newly appended element is wrapped via describeNode with the correct constructor");
}

void test_child_node_list_cache_reflects_mutations()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html><body><ul id="l"><li>1</li><li>2</li></ul>
<script>
  const l = document.getElementById('l');
  const before = l.childNodes.length;       // builds + caches the list

  // The returned list is a copy: mutating it must not corrupt the cache.
  const snapshot = l.childNodes;
  snapshot.length = 0;
  const afterSnapshotMutate = l.childNodes.length;

  // A real DOM mutation must invalidate the cache (the version bumps).
  const li = document.createElement('li');
  li.textContent = '3';
  l.appendChild(li);
  const afterAppend = l.childNodes.length;

  // Cached fast paths stay consistent with the rebuilt list.
  const consistent = l.firstChild === l.childNodes[0]
    && l.lastChild.textContent === '3'
    && l.children.length === 3
    && l.lastElementChild.textContent === '3';

  document.body.setAttribute(
    'data-r', before + ',' + afterSnapshotMutate + ',' + afterAppend + ',' + consistent);
</script></body></html>
)HTML");

    require(page.eval("document.body.getAttribute('data-r')") == "2,2,3,true",
            "childNodes cache reflects mutations, isolates the returned copy, and keeps fast paths consistent");
}

void test_eval_api()
{
    pagecore::Page page;
    page.load_html("<html><body><main id='m'>x</main></body></html>");
    const auto result = page.eval("document.getElementById('m').textContent");
    require(result == "x", "Page::eval should return stringified JS values");
}

void test_dom_methods_report_as_native()
{
    pagecore::Page page;
    page.load_html("<html><body><div id='a'></div></body></html>");
    const auto native_probe = page.eval(R"JS((() => {
      const rnative = /^[^{]+\{\s*\[native \w/;
      return JSON.stringify([
        rnative.test(document.querySelectorAll),
        rnative.test(Element.prototype.matches),
        rnative.test(document.getElementsByTagName),
        rnative.test(getComputedStyle)
      ]);
    })())JS");
    require(native_probe == "[true,true,true,true]",
            "selector/matching/getComputedStyle methods must stringify as native for jQuery/Sizzle feature detection");

    const auto qsa_source = page.eval("document.querySelectorAll.toString()");
    require(qsa_source.find("[native code]") != std::string::npos,
            "querySelectorAll.toString() should contain [native code]");
}

void test_scope_selector_support()
{
    pagecore::Page page;
    page.load_html(R"HTML(<html><body>
      <div id="root">
        <div id="a"></div>
        <div id="b"></div>
        <span><div id="c"></div></span>
      </div>
    </body></html>)HTML");

    const auto direct_children = page.eval(R"JS((() => {
      const root = document.getElementById('root');
      return JSON.stringify([...root.querySelectorAll(':scope > div')].map((e) => e.id));
    })())JS");
    require(direct_children == R"(["a","b"])",
            ":scope > div should match only the query root's direct div children");

    const auto self = page.eval(R"JS((() => {
      const root = document.getElementById('root');
      return JSON.stringify([...root.querySelectorAll(':scope')].map((e) => e.id));
    })())JS");
    require(self == R"(["root"])", ":scope should match the query root itself");

    const auto descendants = page.eval(R"JS((() => {
      const root = document.getElementById('root');
      return JSON.stringify([...root.querySelectorAll(':scope div')].map((e) => e.id).sort());
    })())JS");
    require(descendants == R"(["a","b","c"])",
            ":scope div should match every descendant div of the query root");

    const auto matches_scope = page.eval("document.getElementById('root').matches(':scope')");
    require(matches_scope == "true", "element.matches(':scope') should be true");
}

void test_console_error_includes_error_header()
{
    std::vector<std::pair<std::string, std::string>> console_logs;
    pagecore::LoadOptions options;
    options.console_log = [&](std::string_view severity, std::string_view message) {
        console_logs.emplace_back(severity, message);
    };

    pagecore::Page page(options);
    page.load_html(R"HTML(<html><body>
      <script>console.error(new Error('boom'));</script>
    </body></html>)HTML");

    require(console_logs.size() == 1, "console.error should be routed to the log callback once");
    require(console_logs[0].first == "error", "console.error severity should be error");
    require(console_logs[0].second.rfind("Error: boom", 0) == 0,
            "console.error(Error) should start with the 'Name: message' header");
    require(console_logs[0].second.find("    at") != std::string::npos,
            "console.error(Error) should still include the stack frames");
}

void test_event_capture_bubble_phases()
{
    pagecore::Page page;
    page.load_html(R"HTML(<html><body>
      <div id="root"><div id="mid"><div id="leaf"></div></div></div>
    </body></html>)HTML");

    const auto order = page.eval(R"JS((() => {
      const log = [];
      const root = document.getElementById('root');
      const mid = document.getElementById('mid');
      const leaf = document.getElementById('leaf');
      root.addEventListener('x', () => log.push('root-capture'), true);
      root.addEventListener('x', () => log.push('root-bubble'), false);
      mid.addEventListener('x', () => log.push('mid-capture'), true);
      leaf.addEventListener('x', () => log.push('leaf-target'));
      leaf.dispatchEvent(new CustomEvent('x', { bubbles: true }));
      return log.join(',');
    })())JS");
    require(order == "root-capture,mid-capture,leaf-target,root-bubble",
            "event must run capture (root->target), then target, then bubble");

    const auto non_bubbling = page.eval(R"JS((() => {
      const log = [];
      document.getElementById('root').addEventListener('y', () => log.push('root-capture'), true);
      document.getElementById('leaf').addEventListener('y', () => log.push('leaf'));
      document.getElementById('leaf').dispatchEvent(new Event('y'));
      return log.join(',');
    })())JS");
    require(non_bubbling == "root-capture,leaf",
            "capture phase must reach ancestor listeners even for non-bubbling events");

    const auto detached = page.eval(R"JS((() => {
      let reached = false;
      window.addEventListener('z', () => { reached = true; });
      const d = document.createElement('div');
      d.dispatchEvent(new CustomEvent('z', { bubbles: true }));
      return reached ? 'reached' : 'not-reached';
    })())JS");
    require(detached == "not-reached",
            "a detached node's event must not propagate to window");

    const auto redispatch = page.eval(R"JS((() => {
      let count = 0;
      let first = true;
      const e = new CustomEvent('r', { bubbles: true });
      const root = document.getElementById('root');
      const leaf = document.getElementById('leaf');
      leaf.addEventListener('r', (ev) => { if (first) ev.stopPropagation(); });
      root.addEventListener('r', () => { count++; });
      leaf.dispatchEvent(e); first = false; // root blocked -> 0
      leaf.dispatchEvent(e);                 // re-dispatch, not stopped -> 1
      return String(count);
    })())JS");
    require(redispatch == "1",
            "re-dispatching an Event must reset stop-propagation state");
}

void test_mutation_observer_old_value()
{
    pagecore::Page page;
    page.load_html(R"HTML(<html><body><div id="t" data-x="old"></div></body></html>)HTML");

    const auto with_old = page.eval(R"JS((() => {
      const el = document.getElementById('t');
      const obs = new MutationObserver(() => {});
      obs.observe(el, { attributes: true, attributeOldValue: true });
      el.setAttribute('data-x', 'new');
      const recs = obs.takeRecords();
      return recs.length === 1 && recs[0].type === 'attributes'
        && recs[0].attributeName === 'data-x' && recs[0].oldValue === 'old'
        ? 'ok' : JSON.stringify(recs.map(r => r.oldValue));
    })())JS");
    require(with_old == "ok",
            "MutationObserver should report attribute oldValue when requested");

    const auto without_old = page.eval(R"JS((() => {
      const el = document.getElementById('t');
      const obs = new MutationObserver(() => {});
      obs.observe(el, { attributes: true });
      el.setAttribute('data-x', 'newer');
      const recs = obs.takeRecords();
      return recs.length === 1 && recs[0].oldValue === null ? 'ok' : 'bad';
    })())JS");
    require(without_old == "ok",
            "MutationObserver must null oldValue when attributeOldValue was not requested");
}

void test_js_runtime_robust_exception_paths()
{
    pagecore::Page page;
    page.load_html("<html><body></body></html>");
    // A throwing toString must not leave a dangling pending exception.
    const auto bad = page.eval("({ toString() { throw new Error('boom'); } })");
    require(bad.empty(), "evaluate should return empty string when stringification throws");
    const auto ok = page.eval("1 + 1");
    require(ok == "2", "a prior stringification failure must not corrupt later evaluation");

    // Throwing a non-object from a page script must be logged without corrupting
    // later JS execution.
    std::vector<std::pair<std::string, std::string>> console_logs;
    pagecore::LoadOptions options;
    options.console_log = [&](std::string_view severity, std::string_view message) {
        console_logs.emplace_back(severity, message);
    };
    pagecore::Page page2(options);
    page2.load_html("<html><body><script>throw null;</script><script>document.body.setAttribute('data-after-null-throw', 'ok')</script></body></html>");
    require(page2.outer_html("body[data-after-null-throw='ok']").has_value(),
            "a throwing page script should not abort later scripts");
    require(console_logs.size() == 1 && console_logs[0].first == "error",
            "throwing a non-object from a page script should be logged as an error");
    require(
        console_logs[0].second.find("JS exception (<inline-script-0>): null") != std::string::npos,
        "logged non-object exception should include the script source and thrown value");
}

void test_web_shim_crypto_url_input()
{
    pagecore::Page page;
    page.load_html(R"HTML(<html><body>
      <form id="f"><input id="inp" value="default"></form>
    </body></html>)HTML");

    // crypto must be CSPRNG-backed (host.randomBytes), producing valid, unique
    // v4 UUIDs rather than Math.random output.
    const auto uuid = page.eval(R"JS((() => {
      const re = /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/;
      const a = crypto.randomUUID();
      const b = crypto.randomUUID();
      const arr = new Uint8Array(16);
      crypto.getRandomValues(arr);
      return re.test(a) && re.test(b) && a !== b && arr.length === 16 ? 'ok' : 'bad';
    })())JS");
    require(uuid == "ok", "crypto.randomUUID/getRandomValues must use the host CSPRNG");

    // url.searchParams must be live: mutations reflect back into search/href.
    const auto sp = page.eval(R"JS((() => {
      const u = new URL('https://a.test/?x=1');
      u.searchParams.append('y', '2');
      u.searchParams.set('x', '3');
      return u.search === '?x=3&y=2' && u.href === 'https://a.test/?x=3&y=2'
        ? 'ok' : ('search=' + u.search + ' href=' + u.href);
    })())JS");
    require(sp == "ok", "URL.searchParams must stay synced with search/href");

    // input value uses the dirty-value flag, separate from the content attribute.
    const auto input = page.eval(R"JS((() => {
      const inp = document.getElementById('inp');
      const before = inp.value;
      inp.value = 'typed';
      const after = inp.value;
      const dv = inp.defaultValue;
      const serialized = inp.outerHTML;
      document.getElementById('f').reset();
      const reset = inp.value;
      return (before === 'default' && after === 'typed' && dv === 'default'
        && !serialized.includes('typed') && reset === 'default') ? 'ok'
        : JSON.stringify({ before, after, dv, serialized, reset });
    })())JS");
    require(input == "ok",
            "input value must be separate from defaultValue and restored by reset");
}

void test_streams_writable_controller_and_tee()
{
    pagecore::Page page;
    page.load_html("<html><body></body></html>");

    const auto controller = page.eval(R"JS((() => {
      let ok = false;
      new WritableStream({ start(c) { ok = c && typeof c.error === 'function'; } });
      return ok ? 'ok' : 'bad';
    })())JS");
    require(controller == "ok", "WritableStream must pass a controller to sink.start");

    // tee both branches; reading one fully should yield the source chunks and
    // must not throw or leave an unhandled rejection.
    page.eval(R"JS((() => {
      window.__tee = '';
      const rs = new ReadableStream({ start(c) { c.enqueue('a'); c.enqueue('b'); c.close(); } });
      const [b1, b2] = rs.tee();
      (async () => {
        const reader = b1.getReader();
        let out = '';
        for (;;) { const r = await reader.read(); if (r.done) break; out += r.value; }
        window.__tee = out;
      })();
      return 'started';
    })())JS");
    page.run_until_idle();
    const auto tee = page.eval("window.__tee");
    require(tee == "ab", "ReadableStream.tee must deliver source chunks to a branch");
}

void test_js_exception_message_includes_source_name()
{
    std::vector<std::pair<std::string, std::string>> console_logs;
    pagecore::LoadOptions options;
    options.console_log = [&](std::string_view severity, std::string_view message) {
        console_logs.emplace_back(severity, message);
    };

    pagecore::Page page(options);
    page.load_html(R"HTML(
<html><body>
  <script>throw new Error('boom')</script>
  <script>document.body.setAttribute('data-after-throw', 'ok')</script>
</body></html>
)HTML", "https://example.test/page.html");

    require(page.outer_html("body[data-after-throw='ok']").has_value(),
            "a throwing page script should not abort later scripts");
    require(console_logs.size() == 1 && console_logs[0].first == "error",
            "throwing page script should be logged as a console error");
    require(
        console_logs[0].second.find("JS exception (https://example.test/page.html#inline-script-0): Error: boom") != std::string::npos,
        "JS exception log should include the script source before the exception text");
}

#if defined(PAGECORE_ENABLE_RENDERING)
class CountingLayoutEngineFactory final : public pagecore::LayoutEngineFactory {
public:
    explicit CountingLayoutEngineFactory(std::shared_ptr<pagecore::LayoutEngineFactory> inner)
        : inner_(std::move(inner))
    {
    }

    std::unique_ptr<pagecore::LayoutEngine> create_layout_engine() override
    {
        ++count;
        return inner_->create_layout_engine();
    }

    int count = 0;

private:
    std::shared_ptr<pagecore::LayoutEngineFactory> inner_;
};

void test_page_display_list_is_memoized()
{
    auto counting = std::make_shared<CountingLayoutEngineFactory>(
        pagecore::create_litehtml_layout_engine_factory());

    pagecore::Page page;
    page.set_layout_engine_factory(counting);
    page.load_html(
        "<html><body><div id='x' style='width:50px;height:20px'>hi</div></body></html>",
        "https://example.test/");

    pagecore::RenderOptions options;
    options.viewport = pagecore::Viewport{320, 240, 1.0f};

    const auto first = page.display_list(options);
    const auto second = page.display_list(options);
    require(counting->count == 1, "repeated display_list with identical options must reuse the cache");
    require(first.content_height == second.content_height, "cached display list must be identical");

    // render() goes through the same cached display_list().
    (void) page.render(options);
    require(counting->count == 1, "render() with the same options must reuse the cached display list");

    // A different viewport is a distinct cache key and must rebuild.
    pagecore::RenderOptions other = options;
    other.viewport = pagecore::Viewport{640, 480, 1.0f};
    (void) page.display_list(other);
    require(counting->count == 2, "changing the viewport must rebuild the layout");

    // Layout-affecting DOM mutations bump the layout mutation version and
    // invalidate the cache.
    page.eval("document.getElementById('x').textContent = 'changed and considerably longer text'");
    (void) page.display_list(options);
    require(counting->count == 3, "a DOM mutation must rebuild the layout");
}

void test_page_display_list_ignores_service_attribute_mutations()
{
    auto counting = std::make_shared<CountingLayoutEngineFactory>(
        pagecore::create_litehtml_layout_engine_factory());

    pagecore::Page page;
    page.set_layout_engine_factory(counting);
    page.load_html(
        "<html><body><div id='x' class='a' style='width:50px;height:20px'>hi</div></body></html>",
        "https://example.test/");

    pagecore::RenderOptions options;
    options.viewport = pagecore::Viewport{320, 240, 1.0f};

    (void) page.display_list(options);
    require(counting->count == 1, "first display_list must build layout once");

    page.eval("document.getElementById('x').setAttribute('data-state', 'ready')");
    (void) page.display_list(options);
    require(counting->count == 1, "data-* mutation must reuse the cached layout");

    page.eval("document.getElementById('x').setAttribute('class', 'b')");
    (void) page.display_list(options);
    require(counting->count == 2, "class mutation must rebuild cached layout");
}

void test_page_display_list_rebuilds_for_service_attribute_selectors()
{
    auto counting = std::make_shared<CountingLayoutEngineFactory>(
        pagecore::create_litehtml_layout_engine_factory());

    pagecore::Page page;
    page.set_layout_engine_factory(counting);
    page.load_html(
        "<html><head><style>#x[data-state='ready'] { width:70px; }</style></head>"
        "<body><div id='x' style='width:50px;height:20px'>hi</div></body></html>",
        "https://example.test/");

    pagecore::RenderOptions options;
    options.viewport = pagecore::Viewport{320, 240, 1.0f};

    (void) page.display_list(options);
    require(counting->count == 1, "first display_list must build layout once");

    page.eval("document.getElementById('x').setAttribute('data-state', 'ready')");
    (void) page.display_list(options);
    require(counting->count == 2, "service attribute used by an inline selector must rebuild cached layout");
}

void test_page_display_list_rebuilds_for_external_service_attribute_selectors()
{
    auto counting = std::make_shared<CountingLayoutEngineFactory>(
        pagecore::create_litehtml_layout_engine_factory());
    auto loader = std::make_shared<RecordingResourceLoader>();
    loader->add(
        "https://example.test/style.css",
        "#x[data-state='ready'] { width:70px; }",
        "text/css");

    pagecore::Page page;
    page.set_layout_engine_factory(counting);
    page.set_resource_loader(loader);
    page.load_html(
        "<html><head><link rel='stylesheet' href='/style.css'></head>"
        "<body><div id='x' style='width:50px;height:20px'>hi</div></body></html>",
        "https://example.test/");

    pagecore::RenderOptions options;
    options.viewport = pagecore::Viewport{320, 240, 1.0f};

    (void) page.display_list(options);
    require(counting->count == 1, "first display_list must build layout once");

    page.eval("document.getElementById('x').setAttribute('data-state', 'ready')");
    (void) page.display_list(options);
    require(counting->count == 2, "service attribute used by an external selector must rebuild cached layout");
}

void test_page_display_list_ignores_script_text_mutations()
{
    auto counting = std::make_shared<CountingLayoutEngineFactory>(
        pagecore::create_litehtml_layout_engine_factory());

    pagecore::Page page;
    page.set_layout_engine_factory(counting);
    page.load_html(
        "<html><body><div id='x' style='width:50px;height:20px'>hi</div>"
        "<script id='s'>var a = 1;</script></body></html>",
        "https://example.test/");

    pagecore::RenderOptions options;
    options.viewport = pagecore::Viewport{320, 240, 1.0f};

    (void) page.display_list(options);
    require(counting->count == 1, "first display_list must build layout once");

    page.eval("document.getElementById('s').textContent = 'var a = 2;'");
    (void) page.display_list(options);
    require(counting->count == 1, "script text mutations must not invalidate cached layout");
}

void test_page_display_list_pipeline_with_external_css()
{
    auto loader = std::make_shared<RecordingResourceLoader>();
    loader->add(
        "https://example.test/style.css",
        "#box { width: 120px; height: 30px; background-color: rgb(8, 9, 10); border: 3px solid #00ff00; }");
    loader->add("https://example.test/pixel.png", "png", "image/png");

    pagecore::Page page;
    page.set_resource_loader(loader);
    page.load_html(R"HTML(
<html><body>
  <link rel="stylesheet" href="/style.css">
  <div id="box"></div>
  <img src="/pixel.png">
  <script>
    document.getElementById('box').textContent = 'Rendered';
  </script>
</body></html>
)HTML", "https://example.test/index.html");

    pagecore::RenderOptions options;
    options.viewport = pagecore::Viewport{320, 240, 1.0f};

    const auto display_list = page.display_list(options);
    require(display_list.viewport.width == 320, "display list should preserve viewport");
    require(display_list.content_width > 0, "litehtml should compute content width");
    require(display_list.content_height > 0, "litehtml should compute content height");

    bool has_text = false;
    bool has_fill = false;
    bool has_border = false;

    for (const auto& command : display_list.commands) {
        if (const auto* text = std::get_if<pagecore::TextCommand>(&command)) {
            has_text = has_text || text->text.find("Rendered") != std::string::npos;
        }
        if (const auto* fill = std::get_if<pagecore::SolidFillCommand>(&command)) {
            has_fill = has_fill || (fill->color.r == 8 && fill->color.g == 9 && fill->color.b == 10);
        }
        if (const auto* border = std::get_if<pagecore::BorderCommand>(&command)) {
            has_border = has_border || (border->top.width >= 3.0f && border->top.color.g == 255);
        }
    }

    require(has_text, "display list should include text after JS DOM mutation");
    require(has_fill, "display list should include solid fills from external CSS");
    require(has_border, "display list should include borders from external CSS");
    require(
        has_request_kind(*loader, "https://example.test/style.css", pagecore::ResourceKind::Stylesheet),
        "external CSS should be requested as Stylesheet");
    require(
        has_request_kind(*loader, "https://example.test/pixel.png", pagecore::ResourceKind::Image),
        "images should be requested as Image");
}

void test_page_display_list_hides_head_text_nodes()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html>
  <head>
    <style>body { margin: 0; }</style>
  </head>
  <body>
    <script>
      document.head.appendChild(document.createTextNode('head_text_should_not_render'));
    </script>
    <div>visible body</div>
  </body>
</html>
)HTML", "https://example.test/index.html");

    pagecore::RenderOptions options;
    options.viewport = pagecore::Viewport{320, 120, 1.0f};

    const auto display_list = page.display_list(options);
    bool has_head_text = false;
    bool has_body_text = false;
    for (const auto& command : display_list.commands) {
        if (const auto* text = std::get_if<pagecore::TextCommand>(&command)) {
            has_head_text = has_head_text || text->text.find("head_text_should_not_render") != std::string::npos;
            has_body_text = has_body_text || text->text.find("visible") != std::string::npos;
        }
    }

    require(!has_head_text, "display list should not include text nodes from head");
    require(has_body_text, "display list should still include body text");
}

void test_page_display_list_resolves_litehtml_relative_base_urls()
{
    auto loader = std::make_shared<RecordingResourceLoader>();
    loader->add(
        "https://example.test/styles.css",
        "#box { width: 32px; height: 32px; background-color: rgb(3, 4, 5); background-image: url('./pixel.png'); }",
        "text/css");
    loader->add(
        "https://example.test/pixel.png",
        png_body(pagecore::Color{240, 20, 30, 255}),
        "image/png");

    pagecore::Page page;
    page.set_resource_loader(loader);
    page.load_html(R"HTML(
<html><body>
  <link rel="stylesheet" href="styles.css">
  <div id="box"></div>
</body></html>
)HTML", "https://example.test/");

    pagecore::RenderOptions options;
    options.viewport = pagecore::Viewport{120, 90, 1.0f};

    const auto display_list = page.display_list(options);
    bool has_fill = false;
    bool has_background_image = false;
    for (const auto& command : display_list.commands) {
        if (const auto* fill = std::get_if<pagecore::SolidFillCommand>(&command)) {
            has_fill = has_fill || (fill->color.r == 3 && fill->color.g == 4 && fill->color.b == 5);
        }
        if (const auto* image = std::get_if<pagecore::ImageCommand>(&command)) {
            has_background_image = has_background_image || image->url == "https://example.test/pixel.png";
        }
    }

    require(has_fill, "relative root stylesheet should be loaded and applied");
    require(has_background_image, "relative CSS background image should resolve against stylesheet URL");
    require(
        has_request_kind(*loader, "https://example.test/styles.css", pagecore::ResourceKind::Stylesheet),
        "relative root stylesheet should be requested as absolute URL");
    require(
        has_request_kind(*loader, "https://example.test/pixel.png", pagecore::ResourceKind::Image),
        "relative CSS image should be requested as absolute URL");
}

void test_page_display_list_resolves_relative_base_element_against_document_url()
{
    auto loader = std::make_shared<RecordingResourceLoader>();
    loader->add(
        "https://example.test/assets/styles.css",
        "#box { width: 32px; height: 32px; background-image: url('./pixel.png'); }",
        "text/css");
    loader->add(
        "https://example.test/assets/pixel.png",
        png_body(pagecore::Color{240, 20, 30, 255}),
        "image/png");

    pagecore::Page page;
    page.set_resource_loader(loader);
    page.load_html(R"HTML(
<html><head>
  <base href="/assets/">
  <link rel="stylesheet" href="styles.css">
</head><body>
  <div id="box"></div>
</body></html>
)HTML", "https://example.test/docs/page.html");

    pagecore::RenderOptions options;
    options.viewport = pagecore::Viewport{120, 90, 1.0f};

    const auto display_list = page.display_list(options);
    bool has_background_image = false;
    for (const auto& command : display_list.commands) {
        if (const auto* image = std::get_if<pagecore::ImageCommand>(&command)) {
            has_background_image = has_background_image || image->url == "https://example.test/assets/pixel.png";
        }
    }

    require(has_background_image, "relative base element should resolve CSS image URLs against document origin");
    require(
        has_request_kind(*loader, "https://example.test/assets/styles.css", pagecore::ResourceKind::Stylesheet),
        "relative base element should resolve stylesheet URL against document origin");
    require(
        has_request_kind(*loader, "https://example.test/assets/pixel.png", pagecore::ResourceKind::Image),
        "relative base element should resolve image URL against document origin");
}

void test_page_display_list_resolves_protocol_relative_and_host_like_css_urls()
{
    auto loader = std::make_shared<RecordingResourceLoader>();
    loader->add(
        "https://portal.test/styles/main.css",
        R"CSS(
#protocol { width: 20px; height: 20px; background-image: url("//cdn.example.test/protocol.png"); }
#hostlike { width: 20px; height: 20px; background-image: url("cdn.example.test/host.png"); }
)CSS",
        "text/css");
    loader->add(
        "https://cdn.example.test/protocol.png",
        png_body(pagecore::Color{240, 20, 30, 255}),
        "image/png");
    loader->add(
        "https://cdn.example.test/host.png",
        png_body(pagecore::Color{20, 200, 90, 255}),
        "image/png");

    pagecore::Page page;
    page.set_resource_loader(loader);
    page.load_html(R"HTML(
<html><head>
  <link rel="stylesheet" href="/styles/main.css">
</head><body>
  <div id="protocol"></div>
  <div id="hostlike"></div>
</body></html>
)HTML", "https://portal.test/index.html");

    pagecore::RenderOptions options;
    options.viewport = pagecore::Viewport{120, 90, 1.0f};

    const auto display_list = page.display_list(options);
    bool has_protocol_relative_image = false;
    bool has_host_like_image = false;
    for (const auto& command : display_list.commands) {
        if (const auto* image = std::get_if<pagecore::ImageCommand>(&command)) {
            has_protocol_relative_image = has_protocol_relative_image
                || (image->url == "https://cdn.example.test/protocol.png" && image->image);
            has_host_like_image = has_host_like_image
                || (image->url == "https://cdn.example.test/host.png" && image->image);
        }
    }

    require(has_protocol_relative_image, "protocol-relative CSS image URL should resolve and decode");
    require(has_host_like_image, "host-like CSS image URL should resolve and decode without duplicating the stylesheet host");
    require(
        has_request_kind(*loader, "https://cdn.example.test/protocol.png", pagecore::ResourceKind::Image),
        "protocol-relative CSS image should be requested as an absolute Image resource");
    require(
        has_request_kind(*loader, "https://cdn.example.test/host.png", pagecore::ResourceKind::Image),
        "host-like CSS image should be requested as an absolute Image resource");
}

void test_shared_cookie_jar_render_resources()
{
    auto loader = std::make_shared<RecordingResourceLoader>();
    loader->add("https://example.test/image.png", "not-a-real-png", "image/png");
    loader->add("https://example.test/inline-bg.png", "not-a-real-png", "image/png");
    loader->add(
        "https://example.test/style.css",
        "body { background-image: url('/css-bg.png'); }"
        "@font-face { font-family: CookieFont; src: url('/font.woff2'); }",
        "text/css");
    loader->add("https://example.test/css-bg.png", "not-a-real-png", "image/png");
    loader->add("https://example.test/font.woff2", "not-a-real-font", "font/woff2");

    pagecore::Page page;
    page.set_resource_loader(loader);
    page.load_html(R"HTML(
<html><head>
  <link rel="stylesheet" href="/style.css">
</head><body>
  <script>document.cookie = 'render=sid; path=/';</script>
  <img src="/image.png">
  <div style="font-family:CookieFont;background-image:url('/inline-bg.png')">x</div>
</body></html>
)HTML", "https://example.test/index.html");

    pagecore::RenderOptions options;
    options.viewport = pagecore::Viewport{120, 90, 1.0f};
    (void) page.display_list(options);

    const auto* stylesheet_request = find_request(*loader, "https://example.test/style.css");
    require(stylesheet_request != nullptr && header_contains(*stylesheet_request, "cookie", "render=sid"),
            "stylesheet loads should receive cookies from the shared jar");
    require(stylesheet_request != nullptr && stylesheet_request->referrer == "https://example.test/index.html",
            "stylesheet render loads should carry the document referrer");

    const auto* image_request = find_request(*loader, "https://example.test/image.png");
    require(image_request != nullptr && header_contains(*image_request, "cookie", "render=sid"),
            "image loads should receive cookies from the shared jar");
    require(image_request != nullptr && image_request->referrer == "https://example.test/index.html",
            "image render loads should carry the document referrer");

    const auto* font_request = find_request(*loader, "https://example.test/font.woff2");
    require(font_request != nullptr && header_contains(*font_request, "cookie", "render=sid"),
            "font loads should receive cookies from the shared jar");
    require(font_request != nullptr && font_request->referrer == "https://example.test/index.html",
            "font render loads should carry the document referrer");
}

// Records load_all() batches separately from single load() calls so a test can
// distinguish an up-front prefetch from layout-time on-demand fetches.
class BatchRecordingLoader final : public pagecore::ResourceLoader {
public:
    using ResourceLoader::load;

    void add(std::string url, std::string body, std::string mime_type)
    {
        const std::string key = url;
        resources_[key] = pagecore::ResourceResponse{
            std::move(url), std::move(body), 200, std::move(mime_type), pagecore::ResourceKind::Other, false};
    }

    pagecore::ResourceResponse load(const pagecore::ResourceRequest& request) override
    {
        load_calls.push_back(request.url);
        return fetch(request);
    }

    std::vector<pagecore::ResourceResponse> load_all(
        const std::vector<pagecore::ResourceRequest>& requests,
        pagecore::BatchErrorMode mode) override
    {
        batch_sizes.push_back(requests.size());
        std::vector<pagecore::ResourceResponse> out;
        out.reserve(requests.size());
        for (const auto& request : requests) {
            try {
                out.push_back(fetch(request));
            } catch (const pagecore::ResourceError&) {
                if (mode == pagecore::BatchErrorMode::FailFast) {
                    throw;
                }
                out.push_back(pagecore::ResourceResponse{request.url, {}, 0, {}, request.kind, false});
            }
        }
        return out;
    }

    bool was_requested(const std::string& url) const
    {
        return std::find(all_requested.begin(), all_requested.end(), url) != all_requested.end();
    }

    std::vector<std::string> load_calls;
    std::vector<std::size_t> batch_sizes;
    std::vector<std::string> all_requested;

private:
    pagecore::ResourceResponse fetch(const pagecore::ResourceRequest& request)
    {
        all_requested.push_back(request.url);
        auto found = resources_.find(request.url);
        if (found == resources_.end()) {
            throw pagecore::ResourceError(pagecore::ResourceErrorCode::NotFound, request.url, "missing test resource");
        }
        pagecore::ResourceResponse response = found->second;
        response.kind = request.kind;
        response.from_cache = false;
        return response;
    }

    std::unordered_map<std::string, pagecore::ResourceResponse> resources_;
};

void test_render_prefetches_subresources_into_cache()
{
    auto loader = std::make_shared<BatchRecordingLoader>();
    loader->add("https://example.test/style.css",
                "#b { width: 40px; height: 20px; background-color: rgb(0, 255, 0); }", "text/css");
    loader->add("https://example.test/pixel.png", "fake-image-bytes", "image/png");

    pagecore::Page page;
    page.set_resource_loader(loader);
    page.load_html(R"HTML(
<html><head><link rel="stylesheet" href="/style.css"></head>
<body><div id="b"></div><img src="/pixel.png"></body></html>
)HTML", "https://example.test/index.html");

    pagecore::RenderOptions options;
    options.viewport = pagecore::Viewport{200, 100, 1.0f};
    (void) page.display_list(options);

    // The stylesheet and image are prefetched together in a single batch...
    require(loader->batch_sizes.size() == 1 && loader->batch_sizes[0] == 2,
            "render prefetches the stylesheet and image up front in one concurrent batch");
    // ...so litehtml's synchronous layout-time requests all hit the warm cache.
    require(loader->load_calls.empty(),
            "layout-time sub-resource requests are served from the prefetch cache, not re-fetched");
}

void test_perf_trace_records_render_prefetch_waves()
{
    std::vector<pagecore::PerfEvent> events;
    pagecore::LoadOptions load_options;
    load_options.perf_trace = [&](const pagecore::PerfEvent& event) {
        events.push_back(event);
    };

    auto loader = std::make_shared<BatchRecordingLoader>();
    loader->add("https://example.test/style.css",
                "#b { width: 40px; height: 20px; background-image: url('/bg.png'); }", "text/css");
    loader->add("https://example.test/pixel.png", "fake-image-bytes", "image/png");
    loader->add("https://example.test/bg.png", "fake-background-bytes", "image/png");

    pagecore::Page page(load_options);
    page.set_resource_loader(loader);
    page.load_html(R"HTML(
<html><head><link rel="stylesheet" href="/style.css"></head>
<body><div id="b"></div><img src="/pixel.png"></body></html>
)HTML", "https://example.test/index.html");

    pagecore::RenderOptions options;
    options.viewport = pagecore::Viewport{200, 100, 1.0f};
    (void) page.display_list(options);

    const auto has_event = [&](std::string_view name, std::string_view reason, std::string_view url = {}) {
        return std::any_of(events.begin(), events.end(), [&](const pagecore::PerfEvent& event) {
            return event.phase == pagecore::PerfPhase::ResourceLoad
                && event.name == name
                && event.reason == reason
                && (url.empty() || event.url == url);
        });
    };

    require(has_event("render_prefetch_wave1", "wave1"), "perf trace should record render prefetch wave1");
    require(has_event("render_prefetch_wave2", "wave2"), "perf trace should record render prefetch wave2");
    require(
        has_event("render_prefetch_response", "wave1", "https://example.test/style.css"),
        "perf trace should list wave1 stylesheet responses");
    require(
        has_event("render_prefetch_response", "wave1", "https://example.test/pixel.png"),
        "perf trace should list wave1 image responses");
    require(
        has_event("render_prefetch_response", "wave2", "https://example.test/bg.png"),
        "perf trace should list wave2 CSS image responses");
}

void test_render_resource_budget_blocks_prefetch_and_layout_misses()
{
    std::vector<pagecore::PerfEvent> events;
    pagecore::LoadOptions load_options;
    load_options.perf_trace = [&](const pagecore::PerfEvent& event) {
        events.push_back(event);
    };

    auto loader = std::make_shared<BatchRecordingLoader>();
    loader->add("https://example.test/a.png", "image-a", "image/png");
    loader->add("https://example.test/b.png", "image-b", "image/png");

    pagecore::Page page(load_options);
    page.set_resource_loader(loader);
    page.load_html(R"HTML(
<html><body><img src="/a.png"><img src="/b.png"></body></html>
)HTML", "https://example.test/index.html");

    pagecore::RenderOptions options;
    options.viewport = pagecore::Viewport{200, 100, 1.0f};
    options.max_external_resource_loads = 1;
    (void) page.display_list(options);

    const auto fetch_count = [&](const std::string& url) {
        return std::count(loader->all_requested.begin(), loader->all_requested.end(), url);
    };
    require(fetch_count("https://example.test/a.png") == 1, "first render resource should be loaded");
    require(fetch_count("https://example.test/b.png") == 0,
            "render resources blocked by budget should not reach the underlying loader");

    const bool saw_blocked = std::any_of(events.begin(), events.end(), [](const pagecore::PerfEvent& event) {
        return event.phase == pagecore::PerfPhase::ResourceLoad
            && event.name == "render_resource_blocked"
            && event.reason == "budget:max_render_resource_loads"
            && event.url == "https://example.test/b.png";
    });
    require(saw_blocked, "perf trace should record render resources blocked by budget");
}

// Regression: a script-heavy page invalidates its styled document on every DOM
// mutation, so getComputedStyle()/geometry/render rebuild the litehtml document
// many times during one load. The sub-resource cache must persist across those
// rebuilds so each image/stylesheet is fetched once, not re-downloaded on every
// rebuild (which made large real-world pages spend tens of seconds re-fetching).
void test_render_resource_cache_persists_across_rebuilds()
{
    auto loader = std::make_shared<BatchRecordingLoader>();
    loader->add("https://example.test/style.css", "#b { color: rgb(1, 2, 3); }", "text/css");
    loader->add("https://example.test/pixel.png", "fake-image-bytes", "image/png");

    pagecore::Page page;
    page.set_resource_loader(loader);
    page.load_html(R"HTML(
<html><head><link rel="stylesheet" href="/style.css"></head>
<body><div id="b"></div><img src="/pixel.png"></body></html>
)HTML", "https://example.test/index.html");

    pagecore::RenderOptions options;
    options.viewport = pagecore::Viewport{200, 100, 1.0f};
    (void) page.display_list(options);

    const auto fetch_count = [&](const std::string& url) {
        return std::count(loader->all_requested.begin(), loader->all_requested.end(), url);
    };
    require(fetch_count("https://example.test/style.css") == 1, "stylesheet fetched once on the first render");
    require(fetch_count("https://example.test/pixel.png") == 1, "image fetched once on the first render");

    // A layout-affecting DOM mutation invalidates the styled-document cache so
    // the next render rebuilds the litehtml document from scratch.
    page.eval("document.getElementById('b').setAttribute('class', 'changed')");
    (void) page.display_list(options);

    // The persistent sub-resource cache survives the rebuild: the stylesheet and
    // image are served from cache, not re-fetched from the network.
    require(fetch_count("https://example.test/style.css") == 1,
            "stylesheet is served from the persistent cache on rebuild, not re-fetched");
    require(fetch_count("https://example.test/pixel.png") == 1,
            "image is served from the persistent cache on rebuild, not re-fetched");
}

void test_render_prefetches_css_background_images()
{
    auto loader = std::make_shared<BatchRecordingLoader>();
    // External stylesheet referencing a background image relative to ITS OWN URL
    // (must resolve against /assets/, not the document) plus a @font-face the
    // web-font pipeline fetches separately.
    loader->add("https://example.test/assets/style.css",
                ".ext { width: 12px; height: 12px; background-image: url(css-bg.png); }\n"
                "@font-face { font-family: x; src: url(font.woff2); }",
                "text/css");
    loader->add("https://example.test/assets/css-bg.png", "img", "image/png");
    loader->add("https://example.test/style-bg.png", "img", "image/png");
    loader->add("https://example.test/inline-bg.png", "img", "image/png");

    pagecore::Page page;
    page.set_resource_loader(loader);
    page.load_html(R"HTML(
<html><head>
  <link rel="stylesheet" href="/assets/style.css">
  <style>#a { width: 12px; height: 12px; background-image: url(/style-bg.png); }</style>
</head>
<body>
  <div id="a"></div>
  <div id="i" style="width:12px;height:12px;background-image:url(/inline-bg.png)"></div>
  <div class="ext"></div>
</body></html>
)HTML", "https://example.test/index.html");

    pagecore::RenderOptions options;
    options.viewport = pagecore::Viewport{200, 100, 1.0f};
    (void) page.display_list(options);

    // Wave 1 (DOM): the stylesheet, the <style> background, and the inline-style
    // background. Wave 2: the background referenced inside the external stylesheet,
    // resolved against the stylesheet URL. Wave 3: the web-font source.
    require(loader->batch_sizes.size() == 3, "CSS backgrounds and web fonts trigger follow-up prefetch waves");
    require(loader->batch_sizes[0] == 3, "wave 1 prefetches the stylesheet and DOM-level CSS backgrounds");
    require(loader->batch_sizes[1] == 1, "wave 2 prefetches the external stylesheet's image");
    require(loader->batch_sizes[2] == 1, "wave 3 prefetches the web-font source");

    require(loader->load_calls.empty(),
            "every CSS background image is served from the prefetch cache, including the external one");
    require(loader->was_requested("https://example.test/assets/css-bg.png"),
            "external-stylesheet background resolves against the stylesheet URL");
    require(loader->was_requested("https://example.test/assets/font.woff2"),
            "@font-face sources should be requested by the web-font pipeline");
}

void test_page_render_uses_cairo_raster_backend()
{
    auto loader = std::make_shared<pagecore::MemoryResourceLoader>();
    loader->add(
        "https://example.test/style.css",
        "#box { width: 40px; height: 30px; background-color: rgb(12, 34, 56); }",
        "text/css");

    pagecore::Page page;
    page.set_resource_loader(loader);
    page.load_html(R"HTML(
<html><body>
  <link rel="stylesheet" href="/style.css">
  <div id="box">Hello</div>
</body></html>
)HTML", "https://example.test/index.html");

    pagecore::RenderOptions options;
    options.viewport = pagecore::Viewport{100, 80, 1.0f};
    const auto image = page.render(options);

    require(image.width == 100 && image.height == 80, "Page::render should return viewport-sized image");
    require(image_has_pixel(image, pagecore::Color{12, 34, 56, 255}), "Page::render should rasterize display-list fills");
    require(image_has_non_solid_text_pixel(image), "Page::render should rasterize real anti-aliased text");
}

void test_tiny_tiled_background_does_not_explode()
{
    // Regression: a 1px background tile over a large element would otherwise force
    // up to ~canvas-pixels cairo fills (a CPU DoS). The tile-count cap covers the
    // area with a single stretched draw instead, so the render must still complete
    // and return a valid image.
    auto loader = std::make_shared<pagecore::MemoryResourceLoader>();
    loader->add("https://example.test/dot.png", png_body(pagecore::Color{10, 20, 30, 255}), "image/png");

    pagecore::Page page;
    page.set_resource_loader(loader);
    page.load_html(R"HTML(
<html><body style="margin:0">
  <div style="width:3000px;height:3000px;background-image:url(/dot.png);background-size:1px 1px;background-repeat:repeat"></div>
</body></html>
)HTML", "https://example.test/index.html");

    pagecore::RenderOptions options;
    options.viewport = pagecore::Viewport{3000, 3000, 1.0f};
    const auto image = page.render(options);
    require(image.width == 3000 && image.height == 3000,
            "a pathological 1px repeating background must still render a valid image "
            "(the tile-count cap prevents a CPU DoS)");
}

void test_shadow_dom_paints_real_content_and_hides_light_dom()
{
    // Shadow content is a real Lexbor subtree (see DomDocument::attach_shadow_root),
    // so it must reach litehtml's layout/paint pipeline exactly like ordinary DOM —
    // and, with no <slot>, the host's light-DOM child must never be laid out at all.
    pagecore::Page page;
    page.load_html(R"HTML(
<html><body style="margin:0">
  <color-host id="host" style="display:block;width:40px;height:30px"></color-host>
  <script>
    customElements.define('color-host', class extends HTMLElement {
      connectedCallback () {
        const root = this.attachShadow({ mode: 'open' });
        const shadowBlock = document.createElement('div');
        shadowBlock.setAttribute('style', 'width:40px;height:30px;background-color:rgb(10,20,30)');
        root.appendChild(shadowBlock);

        const lightChild = document.createElement('div');
        lightChild.setAttribute('style', 'width:40px;height:30px;background-color:rgb(200,50,60)');
        this.appendChild(lightChild);
      }
    });
  </script>
</body></html>
)HTML");

    pagecore::RenderOptions options;
    options.viewport = pagecore::Viewport{60, 50, 1.0f};
    const auto image = page.render(options);

    require(pixel_matches(image, 10, 10, pagecore::Color{10, 20, 30, 255}),
            "shadow-tree content should paint through the real lexbor/litehtml pipeline");
    require(!image_has_pixel(image, pagecore::Color{200, 50, 60, 255}),
            "host's light-DOM child has no slot to be distributed into and must not render");
}

void test_page_render_uses_web_font_formats()
{
    const auto fallback_image = [] {
        pagecore::Page page;
        page.load_html(R"HTML(
<html><body style="margin:0;background:white">
<div style="font-family:AliasIcon;font-size:48px;line-height:1;color:black">&#xe000;</div>
</body></html>
)HTML", "https://example.test/index.html");
        pagecore::RenderOptions options;
        options.viewport = pagecore::Viewport{90, 70, 1.0f};
        return page.render(options);
    }();
    const int fallback_dark = count_dark_pixels(fallback_image, 0, 0, 70, 60);

    const std::vector<std::pair<std::string, std::string>> formats{
        {"ttf", "font/ttf"},
        {"woff", "font/woff"},
        {"woff2", "font/woff2"},
    };

    for (const auto& [extension, mime] : formats) {
        auto loader = std::make_shared<RecordingResourceLoader>();
        const std::string font_url = "https://example.test/icon." + extension;
        loader->add(font_url, pagecore_icon_font_body(extension), mime);

        pagecore::Page page;
        page.set_resource_loader(loader);
        page.load_html(R"HTML(
<html><head><style>
@font-face {
  font-family: AliasIcon;
  src: url('/icon.)HTML" + extension + R"HTML(');
}
</style></head>
<body style="margin:0;background:white">
<div style="font-family:AliasIcon;font-size:48px;line-height:1;color:black">&#xe000;</div>
</body></html>
)HTML", "https://example.test/index.html");

        pagecore::RenderOptions options;
        options.viewport = pagecore::Viewport{90, 70, 1.0f};
        const auto image = page.render(options);
        const int webfont_dark = count_dark_pixels(image, 0, 0, 70, 60);

        require(has_request_kind(*loader, font_url, pagecore::ResourceKind::Font),
                "web font should be requested as a Font resource for " + extension);
        require(webfont_dark > fallback_dark * 2,
                "web font should render the embedded filled glyph instead of fallback tofu for " + extension);
        require(region_has_close_pixel(image, 8, 12, 28, 28, pagecore::Color{0, 0, 0, 255}, 8),
                "web font glyph should paint a solid dark interior for " + extension);
    }
}

void test_page_render_rejects_malformed_web_font()
{
    // The woff2/brotli font path parses attacker-controlled bytes. A corrupt or
    // truncated font must be rejected without crashing, and the page must still
    // render (degrading to the fallback font) rather than fail the whole load.
    auto loader = std::make_shared<RecordingResourceLoader>();
    const std::string font_url = "https://example.test/broken.woff2";
    std::string corrupt = "wOF2";           // valid signature...
    corrupt.append(300, '\x01');            // ...followed by garbage table/length fields
    loader->add(font_url, corrupt, "font/woff2");

    pagecore::Page page;
    page.set_resource_loader(loader);
    page.load_html(R"HTML(
<html><head><style>
@font-face { font-family: BrokenFont; src: url('/broken.woff2') format('woff2'); }
</style></head>
<body style="margin:0;background:white">
<div style="font-family:BrokenFont;font-size:32px;color:black">hello</div>
</body></html>
)HTML", "https://example.test/index.html");

    pagecore::RenderOptions options;
    options.viewport = pagecore::Viewport{120, 60, 1.0f};
    const auto image = page.render(options); // must not throw or crash
    require(image.width == 120 && image.height == 60,
            "a page with a corrupt web font should still render at the requested size");
    require(has_request_kind(*loader, font_url, pagecore::ResourceKind::Font),
            "the corrupt web font should still be requested as a Font resource");
}

void test_page_render_decodes_and_draws_png_images()
{
    auto loader = std::make_shared<pagecore::MemoryResourceLoader>();
    loader->add(
        "https://example.test/pixel.png",
        png_body(pagecore::Color{240, 20, 30, 255}),
        "image/png");

    pagecore::Page page;
    page.set_resource_loader(loader);
    page.load_html(R"HTML(
<html><body>
  <img src="/pixel.png" style="width: 24px; height: 24px">
</body></html>
)HTML", "https://example.test/index.html");

    pagecore::RenderOptions options;
    options.viewport = pagecore::Viewport{80, 60, 1.0f};

    const auto display_list = page.display_list(options);
    bool has_decoded_image = false;
    for (const auto& command : display_list.commands) {
        if (const auto* image = std::get_if<pagecore::ImageCommand>(&command)) {
            has_decoded_image = has_decoded_image || (image->image && image->image->width == 2 && image->image->height == 2);
        }
    }
    require(has_decoded_image, "display list should carry decoded image pixels");

    const auto rendered = page.render(options);
    require(image_has_pixel(rendered, pagecore::Color{240, 20, 30, 255}), "Page::render should draw decoded PNG pixels");
}

void test_page_render_decodes_and_draws_data_url_images()
{
    const std::string data_url =
        "data:image/png;base64," + pagecore::base64_encode(png_body(pagecore::Color{24, 160, 220, 255}));

    pagecore::Page page;
    page.load_html(
        "<html><body><img src=\"" + data_url + "\" style=\"width: 24px; height: 24px\"></body></html>",
        "https://example.test/index.html");

    pagecore::RenderOptions options;
    options.viewport = pagecore::Viewport{80, 60, 1.0f};

    const auto display_list = page.display_list(options);
    bool has_decoded_image = false;
    for (const auto& command : display_list.commands) {
        if (const auto* image = std::get_if<pagecore::ImageCommand>(&command)) {
            has_decoded_image = has_decoded_image
                || (image->url == data_url && image->image && image->image->width == 2 && image->image->height == 2);
        }
    }
    require(has_decoded_image, "display list should carry decoded data URL image pixels");

    const auto rendered = page.render(options);
    require(image_has_pixel(rendered, pagecore::Color{24, 160, 220, 255}),
            "Page::render should draw decoded data URL image pixels");
}

void test_page_render_decodes_css_data_url_background_images()
{
    const std::string data_url =
        "data:image/png;base64," + pagecore::base64_encode(png_body(pagecore::Color{210, 32, 90, 255}));

    pagecore::Page page;
    page.load_html(
        "<html><head><style>"
        "#box { width: 24px; height: 24px; background-image: url("
            + data_url
            + "); background-size: 24px 24px; background-repeat: no-repeat; }"
        "</style></head><body><div id=\"box\"></div></body></html>",
        "https://example.test/index.html");

    pagecore::RenderOptions options;
    options.viewport = pagecore::Viewport{80, 60, 1.0f};

    const auto display_list = page.display_list(options);
    bool has_background_image = false;
    for (const auto& command : display_list.commands) {
        if (const auto* image = std::get_if<pagecore::ImageCommand>(&command)) {
            has_background_image = has_background_image
                || (image->url == data_url && image->image && image->image->width == 2 && image->image->height == 2);
        }
    }
    require(has_background_image, "CSS background-image should decode data URL image pixels");

    const auto rendered = page.render(options);
    require(image_has_pixel(rendered, pagecore::Color{210, 32, 90, 255}),
            "Page::render should draw CSS data URL background pixels");
}

void test_page_render_decodes_and_draws_jpeg_images()
{
    auto loader = std::make_shared<pagecore::MemoryResourceLoader>();
    loader->add(
        "https://example.test/pixel.jpg",
        jpeg_body(),
        "image/jpeg");

    pagecore::Page page;
    page.set_resource_loader(loader);
    page.load_html(R"HTML(
<html><body>
  <img src="/pixel.jpg" style="width: 24px; height: 24px">
</body></html>
)HTML", "https://example.test/index.html");

    pagecore::RenderOptions options;
    options.viewport = pagecore::Viewport{80, 60, 1.0f};

    const auto display_list = page.display_list(options);
    bool has_decoded_image = false;
    for (const auto& command : display_list.commands) {
        if (const auto* image = std::get_if<pagecore::ImageCommand>(&command)) {
            has_decoded_image = has_decoded_image || (image->image && image->image->width == 4 && image->image->height == 4);
        }
    }
    require(has_decoded_image, "display list should carry decoded JPEG pixels");

    const auto rendered = page.render(options);
    require(image_has_close_pixel(rendered, pagecore::Color{120, 80, 40, 255}, 8),
            "Page::render should draw decoded JPEG pixels");
}

#if PAGECORE_ENABLE_WEBP
void test_page_render_decodes_and_draws_webp_images()
{
    auto loader = std::make_shared<pagecore::MemoryResourceLoader>();
    loader->add(
        "https://example.test/pixel.webp",
        webp_body(pagecore::Color{12, 200, 90, 255}, 4, 4),
        "image/webp");

    pagecore::Page page;
    page.set_resource_loader(loader);
    page.load_html(R"HTML(
<html><body>
  <img src="/pixel.webp" style="width: 24px; height: 24px">
</body></html>
)HTML", "https://example.test/index.html");

    pagecore::RenderOptions options;
    options.viewport = pagecore::Viewport{80, 60, 1.0f};

    const auto display_list = page.display_list(options);
    bool has_decoded_image = false;
    for (const auto& command : display_list.commands) {
        if (const auto* image = std::get_if<pagecore::ImageCommand>(&command)) {
            has_decoded_image = has_decoded_image || (image->image && image->image->width == 4 && image->image->height == 4);
        }
    }
    require(has_decoded_image, "display list should carry decoded WebP pixels");

    const auto rendered = page.render(options);
    require(image_has_pixel(rendered, pagecore::Color{12, 200, 90, 255}), "Page::render should draw decoded WebP pixels");
}
#endif

void test_page_render_decodes_and_draws_gif_images()
{
    auto loader = std::make_shared<pagecore::MemoryResourceLoader>();
    loader->add(
        "https://example.test/pixel.gif",
        gif_body(),
        "image/gif");

    pagecore::Page page;
    page.set_resource_loader(loader);
    page.load_html(R"HTML(
<html><body>
  <img src="/pixel.gif" style="width: 24px; height: 24px">
</body></html>
)HTML", "https://example.test/index.html");

    pagecore::RenderOptions options;
    options.viewport = pagecore::Viewport{80, 60, 1.0f};

    const auto display_list = page.display_list(options);
    bool has_decoded_image = false;
    for (const auto& command : display_list.commands) {
        if (const auto* image = std::get_if<pagecore::ImageCommand>(&command)) {
            has_decoded_image = has_decoded_image || (image->image && image->image->width == 1 && image->image->height == 1);
        }
    }
    require(has_decoded_image, "display list should carry decoded GIF pixels");

    const auto rendered = page.render(options);
    require(image_has_close_pixel(rendered, pagecore::Color{255, 0, 0, 255}, 16),
            "Page::render should draw decoded GIF pixels");
}

#if PAGECORE_ENABLE_SVG
void test_page_render_decodes_and_draws_svg_images()
{
    auto loader = std::make_shared<pagecore::MemoryResourceLoader>();
    loader->add(
        "https://example.test/pixel.svg",
        svg_body(pagecore::Color{240, 20, 30, 255}),
        "image/svg+xml");

    pagecore::Page page;
    page.set_resource_loader(loader);
    page.load_html(R"HTML(
<html><body>
  <img src="/pixel.svg" style="width: 24px; height: 24px">
</body></html>
)HTML", "https://example.test/index.html");

    pagecore::RenderOptions options;
    options.viewport = pagecore::Viewport{80, 60, 1.0f};

    const auto display_list = page.display_list(options);
    bool has_decoded_image = false;
    for (const auto& command : display_list.commands) {
        if (const auto* image = std::get_if<pagecore::ImageCommand>(&command)) {
            has_decoded_image = has_decoded_image || (image->image && image->image->width == 4 && image->image->height == 3);
        }
    }
    require(has_decoded_image, "display list should carry decoded SVG pixels");

    const auto rendered = page.render(options);
    require(image_has_pixel(rendered, pagecore::Color{240, 20, 30, 255}), "Page::render should draw decoded SVG pixels");
}
#endif

void test_page_render_background_image_size_position_and_repeat()
{
    auto loader = std::make_shared<pagecore::MemoryResourceLoader>();
    loader->add(
        "https://example.test/red.png",
        png_body(pagecore::Color{240, 20, 30, 255}),
        "image/png");

    pagecore::Page page;
    page.set_resource_loader(loader);
    page.load_html(R"HTML(
<html>
  <body style="margin:0">
    <div id="box" style="
      width: 40px;
      height: 24px;
      background-image: url('/red.png');
      background-size: 10px 10px;
      background-position: 20px 5px;
      background-repeat: repeat-x;
    "></div>
  </body>
</html>
)HTML", "https://example.test/index.html");

    pagecore::RenderOptions options;
    options.viewport = pagecore::Viewport{60, 40, 1.0f};

    const auto display_list = page.display_list(options);
    bool has_background_tile = false;
    for (const auto& command : display_list.commands) {
        if (const auto* image = std::get_if<pagecore::ImageCommand>(&command)) {
            has_background_tile = has_background_tile
                || (image->image
                    && image->repeat == pagecore::ImageRepeat::RepeatX
                    && static_cast<int>(image->tile.width) == 10
                    && static_cast<int>(image->tile.height) == 10
                    && static_cast<int>(image->tile.x) == 20
                    && static_cast<int>(image->tile.y) == 5);
        }
    }
    require(has_background_tile, "display list should carry background-size, position and repeat metadata");

    const auto rendered = page.render(options);
    require(pixel_matches(rendered, 22, 7, pagecore::Color{240, 20, 30, 255}),
            "background image should draw at positioned tile");
    require(pixel_matches(rendered, 12, 7, pagecore::Color{240, 20, 30, 255}),
            "background-repeat-x should draw tiles before the positioned tile");
    require(pixel_matches(rendered, 32, 7, pagecore::Color{240, 20, 30, 255}),
            "background-repeat-x should draw tiles after the positioned tile");
    require(!pixel_matches(rendered, 22, 17, pagecore::Color{240, 20, 30, 255}),
            "background-repeat-x should not repeat along the y axis");
}

void test_page_render_linear_gradient_background()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html>
  <body style="margin:0">
    <div id="box" style="
      width: 100px;
      height: 24px;
      background-image: linear-gradient(to right, rgb(240, 20, 30), rgb(20, 30, 240));
    "></div>
  </body>
</html>
)HTML", "https://example.test/index.html");

    pagecore::RenderOptions options;
    options.viewport = pagecore::Viewport{120, 40, 1.0f};

    const auto display_list = page.display_list(options);
    bool has_gradient = false;
    for (const auto& command : display_list.commands) {
        if (const auto* gradient = std::get_if<pagecore::LinearGradientCommand>(&command)) {
            has_gradient = has_gradient || (gradient->stops.size() >= 2 && static_cast<int>(gradient->rect.width) == 100);
        }
    }
    require(has_gradient, "display list should carry linear gradient backgrounds");

    const auto rendered = page.render(options);
    require(pixel_close(rendered, 4, 12, pagecore::Color{240, 20, 30, 255}, 30),
            "linear gradient should draw the first color near the start");
    require(pixel_close(rendered, 96, 12, pagecore::Color{20, 30, 240, 255}, 30),
            "linear gradient should draw the last color near the end");
}

void test_page_render_radial_gradient_background()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html>
  <body style="margin:0">
    <div id="box" style="
      width: 100px;
      height: 100px;
      background-image: radial-gradient(circle closest-side, rgb(240, 20, 30), rgb(20, 30, 240));
    "></div>
  </body>
</html>
)HTML", "https://example.test/index.html");

    pagecore::RenderOptions options;
    options.viewport = pagecore::Viewport{120, 120, 1.0f};

    const auto display_list = page.display_list(options);
    bool has_gradient = false;
    for (const auto& command : display_list.commands) {
        if (const auto* gradient = std::get_if<pagecore::RadialGradientCommand>(&command)) {
            has_gradient = has_gradient || (gradient->stops.size() >= 2 && static_cast<int>(gradient->rect.width) == 100);
        }
    }
    require(has_gradient, "display list should carry radial gradient backgrounds");

    const auto rendered = page.render(options);
    require(pixel_close(rendered, 50, 50, pagecore::Color{240, 20, 30, 255}, 30),
            "radial gradient should draw the first color at the center");
    require(pixel_close(rendered, 95, 95, pagecore::Color{20, 30, 240, 255}, 30),
            "radial gradient should draw the last color past the radius");
}

void test_page_render_conic_gradient_background()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html>
  <body style="margin:0">
    <div id="box" style="
      width: 100px;
      height: 100px;
      background-image: conic-gradient(rgb(240, 20, 30) 0deg 90deg, rgb(20, 30, 240) 90deg 360deg);
    "></div>
  </body>
</html>
)HTML", "https://example.test/index.html");

    pagecore::RenderOptions options;
    options.viewport = pagecore::Viewport{120, 120, 1.0f};

    const auto display_list = page.display_list(options);
    bool has_gradient = false;
    for (const auto& command : display_list.commands) {
        if (const auto* gradient = std::get_if<pagecore::ConicGradientCommand>(&command)) {
            has_gradient = has_gradient || (gradient->stops.size() >= 2 && static_cast<int>(gradient->rect.width) == 100);
        }
    }
    require(has_gradient, "display list should carry conic gradient backgrounds");

    const auto rendered = page.render(options);
    require(pixel_close(rendered, 75, 25, pagecore::Color{240, 20, 30, 255}, 20),
            "conic gradient should draw the first color in the top-right wedge");
    require(pixel_close(rendered, 25, 75, pagecore::Color{20, 30, 240, 255}, 20),
            "conic gradient should draw the second color in the bottom-left wedge");
}

void test_conic_radial_gradient_raster_robustness()
{
    pagecore::DisplayList display_list;
    display_list.viewport = pagecore::Viewport{32, 32, 1.0f};
    const float nan_value = std::numeric_limits<float>::quiet_NaN();
    const float inf_value = std::numeric_limits<float>::infinity();

    pagecore::RadialGradientCommand radial;
    radial.rect = pagecore::Rect{0, 0, 32, 32};
    radial.center = pagecore::Point{nan_value, 0};
    radial.radius = pagecore::Point{0, 0};
    radial.stops.push_back(pagecore::GradientStop{0.0f, pagecore::Color{0, 0, 0, 255}});
    radial.stops.push_back(pagecore::GradientStop{1.0f, pagecore::Color{255, 255, 255, 255}});
    display_list.commands.emplace_back(radial);

    pagecore::ConicGradientCommand conic;
    conic.rect = pagecore::Rect{0, 0, 32, 32};
    conic.center = pagecore::Point{inf_value, 0};
    conic.angle = 0.0f;
    conic.stops.push_back(pagecore::GradientStop{0.0f, pagecore::Color{0, 0, 0, 255}});
    conic.stops.push_back(pagecore::GradientStop{1.0f, pagecore::Color{255, 255, 255, 255}});
    display_list.commands.emplace_back(conic);

    auto raster = pagecore::create_default_raster_backend(pagecore::Color{255, 255, 255, 255});
    const auto image = raster->render(display_list); // must not crash or invoke UB
    require(image.width == 32 && image.height == 32,
            "raster must survive degenerate radius / non-finite center without UB");

    const std::string json = pagecore::display_list_to_json(display_list);
    require(json.find("nan") == std::string::npos
                && json.find("inf") == std::string::npos
                && json.find("NaN") == std::string::npos,
            "display-list JSON must not emit non-finite tokens for radial/conic gradients");
}

void test_conic_gradient_clamps_oversized_element()
{
    // Regression for the conic-gradient DoS: the painter must clamp its temporary
    // surface to the visible canvas rather than allocate a surface the size of the
    // whole (huge) element box and run a per-pixel atan2 over every pixel. A
    // 30000x30000 element on a 32x32 canvas must render instantly and correctly
    // instead of allocating gigabytes.
    pagecore::DisplayList display_list;
    display_list.viewport = pagecore::Viewport{32, 32, 1.0f};

    pagecore::ConicGradientCommand conic;
    conic.rect = pagecore::Rect{0, 0, 30000, 30000};
    conic.center = pagecore::Point{15000, 15000};
    conic.angle = 0.0f;
    conic.stops.push_back(pagecore::GradientStop{0.0f, pagecore::Color{240, 20, 30, 255}});
    conic.stops.push_back(pagecore::GradientStop{1.0f, pagecore::Color{20, 30, 240, 255}});
    display_list.commands.emplace_back(conic);

    auto raster = pagecore::create_default_raster_backend(pagecore::Color{255, 255, 255, 255});
    const auto image = raster->render(display_list);
    require(image.width == 32 && image.height == 32,
            "oversized conic gradient must render at the canvas size without a full-box allocation");

    bool painted = false;
    for (int y = 0; y < image.height && !painted; ++y) {
        for (int x = 0; x < image.width && !painted; ++x) {
            const auto* px = pixel_at(image, x, y);
            if (!(px[0] == 255 && px[1] == 255 && px[2] == 255)) {
                painted = true;
            }
        }
    }
    require(painted, "the visible region of the clamped conic gradient must still be painted");
}

void test_page_render_clips_background_to_border_radius()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html>
  <body style="margin:0">
    <div id="box" style="
      width: 40px;
      height: 40px;
      border-radius: 16px;
      background: rgb(240, 20, 30);
    "></div>
  </body>
</html>
)HTML", "https://example.test/index.html");

    pagecore::RenderOptions options;
    options.viewport = pagecore::Viewport{60, 60, 1.0f};

    const auto display_list = page.display_list(options);
    bool has_rounded_fill = false;
    for (const auto& command : display_list.commands) {
        if (const auto* fill = std::get_if<pagecore::SolidFillCommand>(&command)) {
            has_rounded_fill = has_rounded_fill || static_cast<int>(fill->radii.top_left.x) == 16;
        }
    }
    require(has_rounded_fill, "display list should carry border-radius for solid backgrounds");

    const auto rendered = page.render(options);
    require(pixel_matches(rendered, 20, 20, pagecore::Color{240, 20, 30, 255}),
            "rounded background should fill the center");
    require(pixel_matches(rendered, 1, 1, pagecore::Color{255, 255, 255, 255}),
            "rounded background should not fill the clipped corner");
}

void test_page_render_clips_background_image_to_border_radius()
{
    auto loader = std::make_shared<pagecore::MemoryResourceLoader>();
    loader->add(
        "https://example.test/red.png",
        png_body(pagecore::Color{240, 20, 30, 255}, 4, 4),
        "image/png");

    pagecore::Page page;
    page.set_resource_loader(loader);
    page.load_html(R"HTML(
<html>
  <body style="margin:0">
    <div id="box" style="
      width: 40px;
      height: 40px;
      border-radius: 16px;
      background-image: url('/red.png');
      background-size: 40px 40px;
      background-repeat: no-repeat;
    "></div>
  </body>
</html>
)HTML", "https://example.test/index.html");

    pagecore::RenderOptions options;
    options.viewport = pagecore::Viewport{60, 60, 1.0f};

    const auto display_list = page.display_list(options);
    bool has_rounded_image = false;
    for (const auto& command : display_list.commands) {
        if (const auto* image = std::get_if<pagecore::ImageCommand>(&command)) {
            has_rounded_image = has_rounded_image || static_cast<int>(image->radii.top_left.x) == 16;
        }
    }
    require(has_rounded_image, "display list should carry border-radius for image backgrounds");

    const auto rendered = page.render(options);
    require(pixel_matches(rendered, 20, 20, pagecore::Color{240, 20, 30, 255}),
            "rounded background image should fill the center");
    require(pixel_matches(rendered, 1, 1, pagecore::Color{255, 255, 255, 255}),
            "rounded background image should not fill the clipped corner");
}

void test_page_render_hides_noscript_when_javascript_is_enabled()
{
    constexpr std::string_view html = R"HTML(
<html>
  <head>
    <style>body { margin: 0; }</style>
  </head>
  <body>
    <noscript><div style="width: 40px; height: 20px; background: rgb(240, 20, 30);"></div></noscript>
    <div style="width: 40px; height: 20px; background: rgb(20, 240, 30);"></div>
  </body>
</html>
)HTML";

    pagecore::RenderOptions options;
    options.viewport = pagecore::Viewport{60, 40, 1.0f};

    pagecore::Page scripting_enabled_page;
    scripting_enabled_page.load_html(html, "https://example.test/index.html");
    const auto scripting_enabled = scripting_enabled_page.render(options);
    require(pixel_matches(scripting_enabled, 5, 5, pagecore::Color{20, 240, 30, 255}),
            "noscript fallback should not participate in layout when JavaScript is enabled");

    pagecore::LoadOptions load_options;
    load_options.enable_js = false;
    pagecore::Page scripting_disabled_page(load_options);
    scripting_disabled_page.load_html(html, "https://example.test/index.html");
    const auto scripting_disabled = scripting_disabled_page.render(options);
    require(pixel_matches(scripting_disabled, 5, 5, pagecore::Color{240, 20, 30, 255}),
            "noscript fallback should remain visible when JavaScript is disabled");
}

void test_layout_serialization_preserves_user_layout_id_attribute()
{
    pagecore::Page page;
    page.load_html(
        "<html><body><div id='x' data-pc-sid='author-value' style='width:10px;height:10px'></div></body></html>",
        "https://example.test/index.html");

    pagecore::RenderOptions options;
    options.viewport = pagecore::Viewport{60, 40, 1.0f};
    (void) page.display_list(options);

    const std::string html = page.serialize_html();
    require(
        html.find("data-pc-sid=\"author-value\"") != std::string::npos,
        "layout serialization must restore an author-provided data-pc-sid attribute");
}

void test_visual_fixture_regression()
{
    // Pixel-coordinate checks against the macOS-generated fixture; font metrics
    // differ per platform, so this only holds in the reference environment.
    if (skip_env_sensitive_tests()) {
        return;
    }
    const std::filesystem::path source_dir(PAGECORE_SOURCE_DIR);
    const std::filesystem::path fixture = source_dir / "examples" / "visual-regression" / "index.html";
    const std::filesystem::path output = std::filesystem::path(PAGECORE_BINARY_DIR) / "pagecore_visual_fixture_test.png";

    pagecore::Page page;
    page.load_url("file://" + fixture.string());

    pagecore::RenderOptions options;
    options.viewport = pagecore::Viewport{1280, 1000, 1.0f};
    const auto image = page.render(options);
    pagecore::write_png_rgba(image, output.string());

    require(image.width == 1280 && image.height == 1000, "visual fixture should render the requested viewport");
    require(
        region_has_close_pixel(image, 460, 112, 220, 48, pagecore::Color{35, 111, 82, 255}, 6),
        "visual fixture should show JS-mutated green status chips");
    require(
        region_has_close_pixel(image, 12, 328, 356, 58, pagecore::Color{229, 241, 255, 255}, 4),
        "visual fixture should show imported theme.css styles");

    require(region_has_saturated_pixel(image, 56, 596, 112, 76), "visual fixture should draw PNG img content");
    require(region_has_saturated_pixel(image, 220, 596, 118, 76), "visual fixture should draw JPEG img content");
#if PAGECORE_ENABLE_WEBP
    require(region_has_saturated_pixel(image, 56, 728, 112, 76), "visual fixture should draw WebP img content");
#endif

    require(region_has_saturated_pixel(image, 408, 580, 352, 58), "visual fixture should draw JPEG background image");
    require(region_has_saturated_pixel(image, 408, 646, 352, 58), "visual fixture should draw PNG repeated background image");
#if PAGECORE_ENABLE_WEBP
    require(region_has_saturated_pixel(image, 408, 712, 352, 58), "visual fixture should draw WebP repeated background image");
#endif

    require(
        region_has_close_pixel(image, 12, 930, 760, 54, pagecore::Color{23, 32, 42, 255}, 4),
        "visual fixture footer should be visible and not clipped out of the viewport");
}

// getBoundingClientRect()/offsetWidth/clientWidth/clientTop etc. read back
// litehtml's real box-model geometry (see LiteHtmlLayoutEngine::element_geometry
// and Element.elementGeometry() in 30_dom.js), replacing the old hardcoded
// zeros. width:100,height:50,padding:10,border:5,margin:20 gives a
// border-box of 130x80 (100 + 2*10 + 2*5) and a padding-box of 120x70
// (100 + 2*10); border thickness (5) is clientTop/clientLeft.
void test_geometry_box_model_apis_reflect_real_layout()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html><body style="margin:0">
  <div id="box" style="width:100px;height:50px;padding:10px;border:5px solid black;margin:20px;"></div>
  <script>
    const box = document.getElementById('box');
    const rect = box.getBoundingClientRect();
    const rects = box.getClientRects();
    const checks = [
      rect.width === 130,
      rect.height === 80,
      box.offsetWidth === 130,
      box.offsetHeight === 80,
      box.clientWidth === 120,
      box.clientHeight === 70,
      box.clientTop === 5,
      box.clientLeft === 5,
      box.scrollWidth === box.clientWidth,
      box.scrollHeight === box.clientHeight,
      rects.length === 1,
      rects[0].width === 130 && rects[0].height === 80
    ];
    document.body.setAttribute('data-geometry-check', checks.every(Boolean) ? 'ok' : 'bad');
  </script>
</body></html>
)HTML", "https://example.test/index.html");

    require(
        page.outer_html("body[data-geometry-check='ok']").has_value(),
        "box-model geometry APIs should reflect the real litehtml layout");
}

void test_input_client_geometry_uses_content_box_inline_axis()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<!doctype html>
<html>
<head>
  <style>
    input, textarea {
      -webkit-appearance: none;
      appearance: none;
      height: 200px;
      width: 300px;
      border-style: solid;
      border-width: 10px 20px;
      padding: 2px;
      box-sizing: content-box;
    }
    .block { display: block; }
  </style>
</head>
<body>
  <input id="input-inline">
  <textarea id="textarea-inline"></textarea>
  <input id="input-block" class="block">
  <textarea id="textarea-block" class="block"></textarea>
  <script>
    const checks = [];
    for (const element of document.querySelectorAll('input, textarea')) {
      const isInput = element.nodeName === 'INPUT';
      checks.push(element.clientHeight === 204);
      checks.push(element.clientTop === 10);
      checks.push(element.clientWidth === (isInput ? 300 : 304));
      checks.push(element.clientLeft === (isInput ? 22 : 20));
    }
    document.body.setAttribute('data-input-client-geometry', checks.every(Boolean) ? 'ok' : 'bad');
  </script>
</body>
</html>
)HTML", "https://example.test/index.html");

    require(
        page.outer_html("body[data-input-client-geometry='ok']").has_value(),
        "input client geometry should use the content box on the inline axis while textarea uses padding box");
}

void test_geometry_get_client_rects_returns_domrectlist()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html><body style="margin:0">
  <div id="box" style="width:20px;height:10px;"></div>
  <script>
    const box = document.getElementById('box');
    const rects = box.getClientRects();
    const range = new Range();
    range.selectNodeContents(box);
    const rangeRects = range.getClientRects();
    const checks = [
      Object.prototype.toString.call(rects) === '[object DOMRectList]',
      typeof rects.item === 'function',
      rects.item(0) instanceof DOMRect,
      Object.prototype.toString.call(rects.item(0)) === '[object DOMRect]',
      rects.item(999) === null,
      Object.prototype.toString.call(rangeRects) === '[object DOMRectList]',
      rangeRects.item(0) instanceof DOMRect,
      Object.prototype.toString.call(rangeRects.item(0)) === '[object DOMRect]'
    ];
    document.body.setAttribute('data-domrectlist-check', checks.every(Boolean) ? 'ok' : 'bad');
  </script>
</body></html>
)HTML", "https://example.test/index.html");

    require(
        page.outer_html("body[data-domrectlist-check='ok']").has_value(),
        "getClientRects should expose DOMRectList-compatible objects");
}

void test_window_named_element_access_exposes_document_ids()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html><body>
  <div id="namedBox"></div>
  <img id="imageById" name="imageByName">
  <div id="document"></div>
  <script>
    const checks = [
      window.namedBox === document.getElementById('namedBox'),
      namedBox === window.namedBox,
      window.imageById === document.getElementById('imageById'),
      window.imageByName === document.getElementById('imageById'),
      window.document.nodeType === Node.DOCUMENT_NODE
    ];
    document.body.setAttribute('data-window-named-check', checks.every(Boolean) ? 'ok' : 'bad');
  </script>
</body></html>
)HTML", "https://example.test/index.html");

    require(
        page.outer_html("body[data-window-named-check='ok']").has_value(),
        "window named element access should expose ids/names without overriding real globals");
}

void test_root_client_geometry_uses_viewport()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<!doctype html>
<html style="margin:5px"><body><div style="height:200vh;width:200vw"></div></body></html>
)HTML", "https://example.test/index.html");

    pagecore::RenderOptions options;
    options.viewport = pagecore::Viewport{375, 812, 1.0f};
    (void) page.display_list(options);

    require(page.eval("document.documentElement.clientTop") == "0", "root clientTop should be zero");
    require(page.eval("document.documentElement.clientLeft") == "0", "root clientLeft should be zero");
    require(
        page.eval("document.documentElement.clientWidth === window.innerWidth") == "true",
        "root clientWidth should match the viewport width");
    require(
        page.eval("document.documentElement.clientHeight === window.innerHeight") == "true",
        "root clientHeight should match the viewport height");
}

void test_html_image_element_x_y_reflect_layout_position()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<!doctype html>
<html><body style="margin:0">
  <img id="shown" style="position:absolute;left:10px;top:15px">
  <img id="hidden" style="display:none;position:absolute;left:20px;top:25px">
  <script>
    const dynamicHidden = document.createElement('img');
    document.body.appendChild(dynamicHidden);
    dynamicHidden.style.setProperty('position', 'absolute');
    dynamicHidden.style.setProperty('left', '30px');
    dynamicHidden.style.setProperty('top', '35px');
    dynamicHidden.style.setProperty('display', 'none');
    const checks = [
      shown.x === 10,
      shown.y === 15,
      hidden.x === 0,
      hidden.y === 0,
      dynamicHidden.x === 0,
      dynamicHidden.y === 0
    ];
    document.body.setAttribute('data-image-xy-check', checks.every(Boolean) ? 'ok' : 'bad');
  </script>
</body></html>
)HTML", "https://example.test/index.html");

    require(
        page.outer_html("body[data-image-xy-check='ok']").has_value(),
        "HTMLImageElement.x/y should expose the image layout position");
}

void test_layout_serialization_materializes_cached_absolute_width()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html><head><style>
  body { margin:0; background:#eee; }
  .wrap { position:relative; width:944px; height:300px; }
  .cell {
    width:25%;
    padding-left:9px;
    padding-right:9px;
    box-sizing:border-box;
  }
  .card {
    width:100%;
    height:100px;
    background:#fff;
    border:1px solid #000;
    box-sizing:border-box;
  }
</style></head><body>
  <div class="wrap" id="wrap"><div class="cell" id="cell"><div class="card" id="card">card</div></div></div>
  <script>
    const cell = document.getElementById('cell');
    const measuredWidth = cell.offsetWidth;
    document.body.setAttribute('data-cell-width-ok', measuredWidth > 200 ? 'true' : 'false');
    cell.style.position = 'absolute';
    cell.style.left = '0px';
    cell.style.top = '0px';
  </script>
</body></html>
)HTML", "https://example.test/index.html");

    require(
        page.outer_html("body[data-cell-width-ok='true']").has_value(),
        "test fixture should cache the absolute element's browser-facing geometry before render");

    pagecore::RenderOptions options;
    options.viewport = pagecore::Viewport{1280, 720, 1.0f};
    const auto& display = page.display_list(options);

    const bool saw_wide_card_fill = std::any_of(display.commands.begin(), display.commands.end(), [](const auto& command) {
        const auto* fill = std::get_if<pagecore::SolidFillCommand>(&command);
        return fill
            && fill->color.r == 255
            && fill->color.g == 255
            && fill->color.b == 255
            && fill->rect.x >= 0.0f
            && fill->rect.x < 20.0f
            && fill->rect.y >= 0.0f
            && fill->rect.y < 20.0f
            && fill->rect.width > 200.0f
            && fill->rect.height >= 90.0f;
    });
    require(
        saw_wide_card_fill,
        "render serialization should preserve cached used width for JS-positioned absolute elements");
    const auto cell_html = page.outer_html("#cell");
    require(cell_html.has_value(), "test fixture should keep the positioned cell in the DOM");
    require(
        cell_html->find("width:") == std::string::npos,
        "transient layout width overrides must not mutate user-visible serialized HTML");
}

void test_layout_serialization_materializes_absolute_percentage_width_without_js_measure()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html><head><style>
  body { margin:0; background:#eee; }
  .wrap { position:relative; width:872px; height:260px; }
  .cell {
    width:25%;
    padding-left:9px;
    padding-right:9px;
    box-sizing:border-box;
  }
  .card {
    width:100%;
    height:100px;
    background:#fff;
    border:1px solid #000;
    box-sizing:border-box;
  }
</style></head><body>
  <div class="wrap" id="wrap">
    <div class="cell" id="cell" style="position:absolute;left:0px;top:0px">
      <div class="card" id="card">card</div>
    </div>
  </div>
</body></html>
)HTML", "https://example.test/index.html");

    pagecore::RenderOptions options;
    options.viewport = pagecore::Viewport{1280, 720, 1.0f};
    const auto& display = page.display_list(options);

    const bool saw_wide_card_fill = std::any_of(display.commands.begin(), display.commands.end(), [](const auto& command) {
        const auto* fill = std::get_if<pagecore::SolidFillCommand>(&command);
        return fill
            && fill->color.r == 255
            && fill->color.g == 255
            && fill->color.b == 255
            && fill->rect.x >= 0.0f
            && fill->rect.x < 20.0f
            && fill->rect.y >= 0.0f
            && fill->rect.y < 20.0f
            && fill->rect.width > 190.0f
            && fill->rect.height >= 90.0f;
    });
    require(
        saw_wide_card_fill,
        "render serialization should materialize absolute percentage widths even without a prior JS geometry read");

    const auto cell_html = page.outer_html("#cell");
    require(cell_html.has_value(), "test fixture should keep the positioned cell in the DOM");
    require(
        cell_html->find("width:") == std::string::npos,
        "transient layout width overrides must not leak into user-visible serialized HTML");
}

void test_layout_serialization_skips_stale_cached_width_after_history_rollover()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html><head><style>
  body { margin:0; }
  #parent { position:relative; width:400px; height:60px; }
  #child {
    height:20px;
    background:rgb(12, 34, 56);
  }
  #child.wide { width:200px; }
  #child.narrow { width:100px; }
</style></head><body>
  <div id="parent"><div id="child" class="wide"></div></div>
  <script>
    const child = document.getElementById('child');
    document.body.setAttribute('data-measured', String(child.offsetWidth));
    child.className = 'narrow';
    child.style.position = 'absolute';
    child.style.left = '0px';
    child.style.top = '0px';
    for (let i = 0; i < 160; ++i) {
      const marker = document.createElement('div');
      marker.setAttribute('data-marker', String(i));
      document.body.appendChild(marker);
    }
  </script>
</body></html>
)HTML", "https://example.test/index.html");

    require(
        page.outer_html("body[data-measured='200']").has_value(),
        "test fixture should cache the old wide geometry before class mutation");

    pagecore::RenderOptions options;
    options.viewport = pagecore::Viewport{640, 240, 1.0f};
    const auto& display = page.display_list(options);

    bool saw_narrow_child_fill = false;
    bool saw_stale_wide_child_fill = false;
    for (const auto& command : display.commands) {
        const auto* fill = std::get_if<pagecore::SolidFillCommand>(&command);
        if (!fill
            || fill->color.r != 12
            || fill->color.g != 34
            || fill->color.b != 56) {
            continue;
        }
        saw_narrow_child_fill = saw_narrow_child_fill
            || (fill->rect.width > 95.0f && fill->rect.width < 105.0f);
        saw_stale_wide_child_fill = saw_stale_wide_child_fill
            || (fill->rect.width > 195.0f && fill->rect.width < 205.0f);
    }

    require(saw_narrow_child_fill, "render should use the post-mutation narrow CSS width");
    require(!saw_stale_wide_child_fill, "history rollover must not materialize stale cached width");
}

void test_layout_serialization_skips_stale_cached_absolute_width_after_parent_resize()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html><head><style>
  body { margin:0; }
  #parent { position:relative; width:200px; height:40px; }
  #child { width:50%; height:20px; background:rgb(12, 34, 56); }
</style></head><body>
  <div id="parent"><div id="child"></div></div>
  <script>
    const child = document.getElementById('child');
    document.body.setAttribute('data-measured', String(child.offsetWidth));
    document.getElementById('parent').style.width = '400px';
    child.style.position = 'absolute';
    child.style.left = '0px';
    child.style.top = '0px';
  </script>
</body></html>
)HTML", "https://example.test/index.html");

    require(
        page.outer_html("body[data-measured='100']").has_value(),
        "test fixture should cache the old child width before parent resize");

    pagecore::RenderOptions options;
    options.viewport = pagecore::Viewport{640, 240, 1.0f};
    const auto& display = page.display_list(options);

    const bool saw_resized_child_fill = std::any_of(display.commands.begin(), display.commands.end(), [](const auto& command) {
        const auto* fill = std::get_if<pagecore::SolidFillCommand>(&command);
        return fill
            && fill->color.r == 12
            && fill->color.g == 34
            && fill->color.b == 56
            && fill->rect.width > 190.0f
            && fill->rect.width < 210.0f;
    });
    require(
        saw_resized_child_fill,
        "render-time width materialization must ignore stale cached geometry after parent layout changes");
}

// A synchronous read of an absolute %-width element resolves the percentage against
// its positioned parent's used width, kept proportional to litehtml's own ancestor
// measurements so a JS grid library (Isotope/Masonry) that divides its container by
// an item's width computes the same column count litehtml's boxes imply — the
// browser's column count for these Bootstrap grids.
void test_geometry_absolute_percentage_width_resolves_against_positioned_parent()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html><head><style>
  * { box-sizing: border-box; }
  body { margin: 0; }
  .container { width: 1258px; margin: 0 auto; padding-left: 9px; padding-right: 9px; }
  .row { margin-left: -9px; margin-right: -9px; }
  .col-md-10, .col-sm-3 { position: relative; min-height: 1px; padding-left: 9px; padding-right: 9px; }
  .col-md-10 { float: left; width: 83.33333333%; }
  .col-md-offset-2 { margin-left: 16.66666667%; }
  .col-sm-3 { float: left; width: 25%; }
</style></head><body>
  <div class="container">
    <div class="row">
      <div class="col-md-offset-2 col-md-10">
        <div class="row">
          <div class="col-md-10">
            <div class="row">
              <div id="flat" style="position:relative">
                <div class="col-sm-3 cell" id="cell" style="position:absolute;left:0;top:0">x</div>
              </div>
            </div>
          </div>
        </div>
      </div>
    </div>
  </div>
</body></html>
)HTML", "https://example.test/index.html");

    const pagecore::NodeId root = page.document().document_node();
    const pagecore::NodeId flat = page.document().query_selector(root, "#flat");
    const pagecore::NodeId cell = page.document().query_selector(root, "#cell");
    require(flat != pagecore::kInvalidNodeId && cell != pagecore::kInvalidNodeId, "expected absolute percentage fixture");

    const auto flat_geometry = page.element_geometry(flat);
    const auto cell_geometry = page.element_geometry(cell);
    require(flat_geometry && cell_geometry, "expected exact geometry for absolute percentage fixture");

    const float expected_width = flat_geometry->padding_box.width * 0.25f;
    require(
        std::abs(cell_geometry->border_box.width - expected_width) < 0.5f,
        "absolute percentage width should resolve against the positioned parent's padding box");
}

void test_geometry_reads_load_stylesheets_but_skip_images_until_render()
{
    auto loader = std::make_shared<RecordingResourceLoader>();
    loader->add(
        "https://example.test/styles.css",
        "#box { width: 123px; height: 20px; background-image: url('/pixel.png'); }",
        "text/css");
    loader->add(
        "https://example.test/pixel.png",
        png_body(pagecore::Color{240, 20, 30, 255}),
        "image/png");

    pagecore::Page page;
    page.set_resource_loader(loader);
    page.load_html(R"HTML(
<html><head>
  <link rel="stylesheet" href="/styles.css">
</head><body>
  <div id="box"></div>
</body></html>
)HTML", "https://example.test/index.html");

    const pagecore::NodeId box = page.document().query_selector(page.document().document_node(), "#box");
    require(box != pagecore::kInvalidNodeId, "expected #box");

    const auto geometry = page.element_geometry(box);
    require(geometry && std::abs(geometry->border_box.width - 123.0f) < 0.5f,
            "geometry reads should still apply external CSS");
    require(
        has_request_kind(*loader, "https://example.test/styles.css", pagecore::ResourceKind::Stylesheet),
        "geometry reads should load external stylesheets");
    require(
        !has_request_kind(*loader, "https://example.test/pixel.png", pagecore::ResourceKind::Image),
        "geometry reads should not load image bytes");

    (void) page.display_list();
    require(
        has_request_kind(*loader, "https://example.test/pixel.png", pagecore::ResourceKind::Image),
        "final display-list rendering should still load images");
}

// offsetTop/offsetLeft must be measured relative to offsetParent's
// border-box, not the viewport — an absolutely positioned child inside a
// position:relative container offsets exactly by its own top/left.
void test_geometry_offset_top_left_relative_to_offset_parent()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html><body style="margin:0">
  <div id="container" style="position:relative; width:300px; height:200px; margin:50px;">
    <div id="child" style="position:absolute; top:10px; left:20px; width:40px; height:30px;"></div>
  </div>
  <script>
    const container = document.getElementById('container');
    const child = document.getElementById('child');
    const checks = [
      child.offsetParent === container,
      child.offsetTop === 10,
      child.offsetLeft === 20
    ];
    document.body.setAttribute('data-offset-check', checks.every(Boolean) ? 'ok' : 'bad');
  </script>
</body></html>
)HTML", "https://example.test/index.html");

    require(
        page.outer_html("body[data-offset-check='ok']").has_value(),
        "offsetTop/offsetLeft should be measured relative to offsetParent, not the viewport");
}

// offsetParent must walk up to the nearest *positioned* ancestor, not just
// return parentElement. #target's parentElement is #staticWrapper
// (position:static), so the old "return this.parentElement" fallback would
// have gotten this wrong; the real offsetParent is #positioned.
void test_geometry_offset_parent_finds_nearest_positioned_ancestor()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html><body>
  <div id="positioned" style="position:absolute;">
    <div id="staticWrapper">
      <div id="target"></div>
    </div>
  </div>
  <script>
    const positioned = document.getElementById('positioned');
    const target = document.getElementById('target');
    document.body.setAttribute('data-offset-parent-check', target.offsetParent === positioned ? 'ok' : 'bad');
  </script>
</body></html>
)HTML", "https://example.test/index.html");

    require(
        page.outer_html("body[data-offset-parent-check='ok']").has_value(),
        "offsetParent should walk to the nearest positioned ancestor, not just parentElement");
}

void test_geometry_offset_parent_cache_invalidates_after_style_mutation()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html><body style="margin:0">
  <div id="outer" style="position:relative; width:300px; height:200px;">
    <div id="wrapper">
      <div id="target" style="position:absolute; top:7px; left:9px; width:20px; height:10px;"></div>
    </div>
  </div>
  <script>
    const outer = document.getElementById('outer');
    const wrapper = document.getElementById('wrapper');
    const target = document.getElementById('target');

    const before = target.offsetParent === outer;
    wrapper.setAttribute('style', 'position:relative;');
    const after = target.offsetParent === wrapper && target.offsetTop === 7 && target.offsetLeft === 9;

    document.body.setAttribute('data-offset-parent-cache-check', before && after ? 'ok' : 'bad');
  </script>
</body></html>
)HTML", "https://example.test/index.html");

    require(
        page.outer_html("body[data-offset-parent-cache-check='ok']").has_value(),
        "offsetParent cache should invalidate after a style mutation changes the positioned ancestor");
}

// window.innerWidth/innerHeight/devicePixelRatio (and screen.*) used to be
// flat values fixed at install() time; they must now reflect whatever
// viewport the page was most recently rendered with.
void test_window_viewport_reflects_last_render_options()
{
    pagecore::Page page;
    page.load_html("<html><body></body></html>", "https://example.test/index.html");

    pagecore::RenderOptions options;
    options.viewport = pagecore::Viewport{375, 812, 2.0f};
    (void) page.display_list(options);

    require(page.eval("window.innerWidth") == "375", "window.innerWidth should reflect the last render viewport");
    require(page.eval("window.innerHeight") == "812", "window.innerHeight should reflect the last render viewport");
    require(
        page.eval("window.devicePixelRatio") == "2",
        "window.devicePixelRatio should reflect the last render viewport");
    require(page.eval("window.outerWidth") == "375", "window.outerWidth should mirror the render viewport width");
    require(
        page.eval("screen.width === window.innerWidth") == "true",
        "screen.width should stay in sync with the live viewport, not a stale snapshot");
}

// Regression: a display:none element has no render_item (layout() skips it
// entirely), so geometry reads must keep returning zeros, exactly like
// before this migration — not throw or return stale/garbage geometry.
void test_geometry_apis_return_zero_for_display_none()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html><body>
  <div id="hidden" style="display:none; width:100px; height:50px;"></div>
  <script>
    const hidden = document.getElementById('hidden');
    const rect = hidden.getBoundingClientRect();
    const checks = [
      rect.x === 0,
      rect.y === 0,
      rect.width === 0,
      rect.height === 0,
      hidden.offsetWidth === 0,
      hidden.offsetHeight === 0,
      hidden.clientWidth === 0,
      hidden.clientHeight === 0,
      hidden.getClientRects().length === 0
    ];
    document.body.setAttribute('data-hidden-check', checks.every(Boolean) ? 'ok' : 'bad');
  </script>
</body></html>
)HTML", "https://example.test/index.html");

    require(
        page.outer_html("body[data-hidden-check='ok']").has_value(),
        "geometry APIs should still return zeros for display:none elements");
}

void test_perf_trace_records_render_geometry_and_png_phases()
{
    std::vector<pagecore::PerfEvent> events;
    auto trace = [&](const pagecore::PerfEvent& event) {
        events.push_back(event);
    };

    pagecore::LoadOptions load_options;
    load_options.perf_trace = trace;

    pagecore::Page page(load_options);
    page.load_html(
        "<html><body style='margin:0'><div id='box' style='width:24px;height:12px;background:#f00'></div></body></html>",
        "https://example.test/");

    pagecore::RenderOptions render_options;
    render_options.viewport = pagecore::Viewport{80, 60, 1.0f};
    render_options.perf_trace = trace;

    (void) page.display_list(render_options);

    const pagecore::NodeId box = page.document().query_selector(page.document().document_node(), "#box");
    require(box != pagecore::kInvalidNodeId, "expected to find #box");
    (void) page.computed_style(box);
    (void) page.element_geometry(box);

    const auto image = page.render(render_options);
    const auto png = pagecore::encode_png_rgba(image, trace);
    require(!png.empty(), "PNG encoding should produce bytes");

    auto has_phase = [&](pagecore::PerfPhase phase) {
        return std::any_of(events.begin(), events.end(), [&](const pagecore::PerfEvent& event) {
            return event.phase == phase;
        });
    };
    auto count_for = [&](pagecore::PerfPhase phase) {
        std::uint64_t count = 0;
        for (const auto& event : events) {
            if (event.phase == phase) {
                count = std::max(count, event.count);
            }
        }
        return count;
    };

    // The direct-DOM default feeds litehtml straight from the DOM, so no
    // serialization phase may appear; the document load is traced as load_dom.
    require(!has_phase(pagecore::PerfPhase::SerializeHtml),
            "the direct-DOM path must not serialize the document for layout");
    require(has_phase(pagecore::PerfPhase::SubresourceScan), "perf trace should record subresource scanning");
    require(has_phase(pagecore::PerfPhase::LitehtmlLoadHtml), "perf trace should record the litehtml document load");
    const auto load_dom_event = std::any_of(events.begin(), events.end(), [](const pagecore::PerfEvent& event) {
        return event.phase == pagecore::PerfPhase::LitehtmlLoadHtml && event.name == "load_dom";
    });
    require(load_dom_event, "the direct-DOM document load should be traced as load_dom");
    require(has_phase(pagecore::PerfPhase::LitehtmlLayout), "perf trace should record litehtml layout");
    require(has_phase(pagecore::PerfPhase::ComputedStyle), "perf trace should record computed style reads");
    require(has_phase(pagecore::PerfPhase::Geometry), "perf trace should record geometry reads");
    require(has_phase(pagecore::PerfPhase::Raster), "perf trace should record rasterization");
    require(has_phase(pagecore::PerfPhase::PngEncode), "perf trace should record PNG encoding");
    require(count_for(pagecore::PerfPhase::PngEncode) == png.size(), "PNG trace count should report encoded bytes");

    auto computed = std::find_if(events.begin(), events.end(), [&](const pagecore::PerfEvent& event) {
        return event.phase == pagecore::PerfPhase::ComputedStyle && event.name == "computed_style";
    });
    require(computed != events.end(), "perf trace should include the full computed style event");
    require(computed->node_id && *computed->node_id == box, "computed style event should report node id");
    require(computed->mutation_version.has_value(), "computed style event should report DOM mutation version");
    require(computed->layout_mutation_version.has_value(), "computed style event should report layout mutation version");
    require(
        computed->styled_document_cache_hit.has_value(),
        "computed style event should report styled-document cache hit state");
    require(
        !computed->styled_document_cache_reason.empty(),
        "computed style event should report styled-document cache reason");
}

void test_perf_trace_records_document_and_initial_script_resources()
{
    std::vector<pagecore::PerfEvent> events;
    pagecore::LoadOptions load_options;
    load_options.perf_trace = [&](const pagecore::PerfEvent& event) {
        events.push_back(event);
    };

    auto loader = std::make_shared<pagecore::MemoryResourceLoader>();
    loader->add(
        "https://example.test/index.html",
        "<html><body><script src='/app.js'></script></body></html>",
        "text/html");
    loader->add(
        "https://example.test/app.js",
        "document.body.setAttribute('data-script-loaded', 'yes');",
        "text/javascript");

    pagecore::Page page(load_options);
    page.set_resource_loader(loader);
    page.load_url("https://example.test/index.html");

    require(
        page.outer_html("body[data-script-loaded='yes']").has_value(),
        "external script should execute during load_url");

    auto document_load = std::find_if(events.begin(), events.end(), [](const pagecore::PerfEvent& event) {
        return event.phase == pagecore::PerfPhase::ResourceLoad && event.name == "document_load";
    });
    require(document_load != events.end(), "perf trace should record the top-level document load");
    require(document_load->property == "document", "document load trace should identify the resource class");
    require(
        document_load->url == "https://example.test/index.html",
        "document load trace should report the effective URL");
    require(document_load->count > 0, "document load trace should report loaded bytes");

    // Parser scripts fetch through the async engine now: there is no batched
    // "initial_script_load_all" phase anymore, only per-response events.
    auto script_response = std::find_if(events.begin(), events.end(), [](const pagecore::PerfEvent& event) {
        return event.phase == pagecore::PerfPhase::ResourceLoad
            && event.name == "initial_script_response"
            && event.url == "https://example.test/app.js";
    });
    require(script_response != events.end(), "perf trace should list initial script responses by URL");
    require(script_response->property == "script", "fresh initial script response should be marked as script");
    require(script_response->count > 0, "initial script response trace should report loaded bytes");

    auto script_execute = std::find_if(events.begin(), events.end(), [](const pagecore::PerfEvent& event) {
        return event.phase == pagecore::PerfPhase::Script && event.name == "execute";
    });
    require(script_execute != events.end(), "perf trace should record script execution during load");
}

void test_computed_style_property_uses_layout_mutation_version_cache()
{
    std::vector<pagecore::PerfEvent> events;

    pagecore::LoadOptions load_options;
    load_options.perf_trace = [&](const pagecore::PerfEvent& event) {
        events.push_back(event);
    };

    pagecore::Page page(load_options);
    page.load_html(
        "<html><body><div id='x' style='display:block;width:24px;height:12px'></div></body></html>",
        "https://example.test/");

    page.eval(R"JS(
      const x = document.getElementById('x');
      const before = getComputedStyle(x).display;
      x.setAttribute('data-state', 'ready');
      const after = getComputedStyle(x).display;
      document.body.setAttribute('data-style-cache', before + ':' + after);
    )JS");

    require(
        page.outer_html("body[data-style-cache='block:block']").has_value(),
        "computed style reads should still return values across service-only mutations");

    int property_events = 0;
    for (const auto& event : events) {
        if (event.phase == pagecore::PerfPhase::ComputedStyle
            && event.name == "computed_style_property"
            && event.property == "display") {
            ++property_events;
        }
    }
    require(
        property_events == 1,
        "service-only DOM mutation must not invalidate layout-version keyed computed style property cache");
}

// Decorates a LayoutEngine to count real layout() passes (distinct from
// CountingLayoutEngineFactory's engine-creation count above), so the geometry
// cache-reuse guarantee can be checked directly rather than just inferred.
class LayoutCallCountingEngine final : public pagecore::LayoutEngine {
public:
    LayoutCallCountingEngine(std::unique_ptr<pagecore::LayoutEngine> inner, std::shared_ptr<int> layout_calls)
        : inner_(std::move(inner))
        , layout_calls_(std::move(layout_calls))
    {
    }

    void set_resource_loader(std::shared_ptr<pagecore::ResourceLoader> loader) override
    {
        inner_->set_resource_loader(std::move(loader));
    }
    void set_viewport(pagecore::Viewport viewport) override { inner_->set_viewport(viewport); }
    void load_html(std::string_view html, std::string_view base_url) override { inner_->load_html(html, base_url); }
    bool load_dom(const DomLayoutRequest& request) override { return inner_->load_dom(request); }
    void layout() override
    {
        ++(*layout_calls_);
        inner_->layout();
    }
    void set_font_environment(std::shared_ptr<const pagecore::FontEnvironment> font_environment) override
    {
        inner_->set_font_environment(std::move(font_environment));
    }
    const pagecore::DisplayList& display_list() const override { return inner_->display_list(); }
    void compute_styles_only() override { inner_->compute_styles_only(); }
    std::optional<pagecore::ComputedStyle> computed_style(std::string_view node_key) override
    {
        return inner_->computed_style(node_key);
    }
    std::optional<std::string> computed_style_property(std::string_view node_key, std::string_view property) override
    {
        return inner_->computed_style_property(node_key, property);
    }
    std::optional<pagecore::ElementGeometry> element_geometry(std::string_view node_key) override
    {
        return inner_->element_geometry(node_key);
    }
    std::vector<AbsolutePercentWidthOverride> collect_absolute_percent_width_overrides() override
    {
        return inner_->collect_absolute_percent_width_overrides();
    }
    bool apply_inline_style_patches(const std::vector<InlineStylePatch>& patches) override
    {
        return inner_->apply_inline_style_patches(patches);
    }

private:
    std::unique_ptr<pagecore::LayoutEngine> inner_;
    std::shared_ptr<int> layout_calls_;
};

class LayoutCallCountingFactory final : public pagecore::LayoutEngineFactory {
public:
    explicit LayoutCallCountingFactory(std::shared_ptr<pagecore::LayoutEngineFactory> inner)
        : layout_calls(std::make_shared<int>(0))
        , inner_(std::move(inner))
    {
    }

    std::unique_ptr<pagecore::LayoutEngine> create_layout_engine() override
    {
        return std::make_unique<LayoutCallCountingEngine>(inner_->create_layout_engine(), layout_calls);
    }

    std::shared_ptr<int> layout_calls;

private:
    std::shared_ptr<pagecore::LayoutEngineFactory> inner_;
};

class SlowGeometryEngine final : public pagecore::LayoutEngine {
public:
    explicit SlowGeometryEngine(std::shared_ptr<int> layout_calls)
        : layout_calls_(std::move(layout_calls))
    {
    }

    void set_resource_loader(std::shared_ptr<pagecore::ResourceLoader>) override { }
    void set_viewport(pagecore::Viewport viewport) override
    {
        viewport_ = viewport;
        display_list_.viewport = viewport_;
    }
    void load_html(std::string_view, std::string_view) override
    {
        laid_out_ = false;
        display_list_.clear();
        display_list_.viewport = viewport_;
    }
    void layout() override
    {
        ++(*layout_calls_);
        // Two forced layouts must exceed the 100ms cumulative geometry budget that
        // trips bounded mode (2 x 80ms = 160ms), while one op stays well under the
        // 250ms single-op "expensive" threshold. This is a must-exceed lower bound,
        // so a contended scheduler only increases the margin, never shrinks it.
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        laid_out_ = true;
        display_list_.viewport = viewport_;
        display_list_.content_width = viewport_.width;
        display_list_.content_height = viewport_.height;
    }
    const pagecore::DisplayList& display_list() const override { return display_list_; }
    std::optional<pagecore::ElementGeometry> element_geometry(std::string_view) override
    {
        if (!laid_out_) {
            return std::nullopt;
        }
        const float width = static_cast<float>(*layout_calls_ * 10);
        return pagecore::ElementGeometry{
            pagecore::Rect{0.0f, 0.0f, width, 10.0f},
            pagecore::Rect{0.0f, 0.0f, width, 10.0f},
        };
    }

private:
    std::shared_ptr<int> layout_calls_;
    pagecore::Viewport viewport_;
    pagecore::DisplayList display_list_;
    bool laid_out_ = false;
};

class SlowGeometryFactory final : public pagecore::LayoutEngineFactory {
public:
    SlowGeometryFactory()
        : layout_calls(std::make_shared<int>(0))
    {
    }

    std::unique_ptr<pagecore::LayoutEngine> create_layout_engine() override
    {
        return std::make_unique<SlowGeometryEngine>(layout_calls);
    }

    std::shared_ptr<int> layout_calls;
};

// A single layout() slow enough (> kExpensiveStyledDocumentUs = 250ms) to flag the
// styled document "expensive" on the very first pass.
class ExpensiveGeometryEngine final : public pagecore::LayoutEngine {
public:
    explicit ExpensiveGeometryEngine(std::shared_ptr<int> layout_calls)
        : layout_calls_(std::move(layout_calls))
    {
    }

    void set_resource_loader(std::shared_ptr<pagecore::ResourceLoader>) override { }
    void set_viewport(pagecore::Viewport viewport) override
    {
        viewport_ = viewport;
        display_list_.viewport = viewport_;
    }
    void load_html(std::string_view, std::string_view) override
    {
        laid_out_ = false;
        display_list_.clear();
        display_list_.viewport = viewport_;
    }
    void layout() override
    {
        ++(*layout_calls_);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        laid_out_ = true;
        display_list_.viewport = viewport_;
        display_list_.content_width = viewport_.width;
        display_list_.content_height = viewport_.height;
    }
    const pagecore::DisplayList& display_list() const override { return display_list_; }
    std::optional<pagecore::ElementGeometry> element_geometry(std::string_view) override
    {
        if (!laid_out_) {
            return std::nullopt;
        }
        return pagecore::ElementGeometry{
            pagecore::Rect{0.0f, 0.0f, 10.0f, 10.0f},
            pagecore::Rect{0.0f, 0.0f, 10.0f, 10.0f},
        };
    }

private:
    std::shared_ptr<int> layout_calls_;
    pagecore::Viewport viewport_;
    pagecore::DisplayList display_list_;
    bool laid_out_ = false;
};

class ExpensiveGeometryFactory final : public pagecore::LayoutEngineFactory {
public:
    ExpensiveGeometryFactory()
        : layout_calls(std::make_shared<int>(0))
    {
    }

    std::unique_ptr<pagecore::LayoutEngine> create_layout_engine() override
    {
        return std::make_unique<ExpensiveGeometryEngine>(layout_calls);
    }

    std::shared_ptr<int> layout_calls;
};

class SlowComputedStyleEngine final : public pagecore::LayoutEngine {
public:
    explicit SlowComputedStyleEngine(std::shared_ptr<int> compute_calls)
        : compute_calls_(std::move(compute_calls))
    {
    }

    void set_resource_loader(std::shared_ptr<pagecore::ResourceLoader>) override { }
    void set_viewport(pagecore::Viewport viewport) override
    {
        viewport_ = viewport;
        display_list_.viewport = viewport_;
    }
    void load_html(std::string_view, std::string_view) override
    {
        display_list_.clear();
        display_list_.viewport = viewport_;
    }
    void layout() override { }
    const pagecore::DisplayList& display_list() const override { return display_list_; }
    void compute_styles_only() override
    {
        ++(*compute_calls_);
        // Two forced layouts must exceed the 100ms cumulative geometry budget that
        // trips bounded mode (2 x 80ms = 160ms), while one op stays well under the
        // 250ms single-op "expensive" threshold. This is a must-exceed lower bound,
        // so a contended scheduler only increases the margin, never shrinks it.
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
    }
    std::optional<std::string> computed_style_property(
        std::string_view,
        std::string_view property) override
    {
        if (property == "width") {
            return std::to_string(*compute_calls_ * 10) + "px";
        }
        if (property == "display") {
            return "block";
        }
        return std::nullopt;
    }

private:
    std::shared_ptr<int> compute_calls_;
    pagecore::Viewport viewport_;
    pagecore::DisplayList display_list_;
};

class SlowComputedStyleFactory final : public pagecore::LayoutEngineFactory {
public:
    SlowComputedStyleFactory()
        : compute_calls(std::make_shared<int>(0))
    {
    }

    std::unique_ptr<pagecore::LayoutEngine> create_layout_engine() override
    {
        return std::make_unique<SlowComputedStyleEngine>(compute_calls);
    }

    std::shared_ptr<int> compute_calls;
};

// Slow cascade whose `display` is `none` and comes ONLY from a stylesheet, never
// from an inline style: exactly the shape of `.wrapper{display:none}` that a page
// script reveals with jQuery's .show().
class StylesheetHiddenComputedStyleEngine final : public pagecore::LayoutEngine {
public:
    explicit StylesheetHiddenComputedStyleEngine(std::shared_ptr<int> compute_calls)
        : compute_calls_(std::move(compute_calls))
    {
    }

    void set_resource_loader(std::shared_ptr<pagecore::ResourceLoader>) override { }
    void set_viewport(pagecore::Viewport viewport) override
    {
        viewport_ = viewport;
        display_list_.viewport = viewport_;
    }
    void load_html(std::string_view, std::string_view) override
    {
        display_list_.clear();
        display_list_.viewport = viewport_;
    }
    void layout() override { }
    const pagecore::DisplayList& display_list() const override { return display_list_; }
    void compute_styles_only() override
    {
        ++(*compute_calls_);
        // Two forced rebuilds must exceed the 100ms cumulative computed-style budget
        // that trips bounded mode (2 x 80ms = 160ms), while one op stays well under
        // the 250ms single-op "expensive" threshold. A must-exceed lower bound, so a
        // contended scheduler only widens the margin.
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
    }
    std::optional<pagecore::ComputedStyle> computed_style(std::string_view) override
    {
        return pagecore::ComputedStyle{{
            {"display", "none"},
            {"width", std::to_string(*compute_calls_ * 10) + "px"},
        }};
    }

private:
    std::shared_ptr<int> compute_calls_;
    pagecore::Viewport viewport_;
    pagecore::DisplayList display_list_;
};

class StylesheetHiddenComputedStyleFactory final : public pagecore::LayoutEngineFactory {
public:
    StylesheetHiddenComputedStyleFactory()
        : compute_calls(std::make_shared<int>(0))
    {
    }

    std::unique_ptr<pagecore::LayoutEngine> create_layout_engine() override
    {
        return std::make_unique<StylesheetHiddenComputedStyleEngine>(compute_calls);
    }

    std::shared_ptr<int> compute_calls;
};

void test_element_geometry_reuses_cached_layout()
{
    auto counting = std::make_shared<LayoutCallCountingFactory>(pagecore::create_litehtml_layout_engine_factory());

    pagecore::Page page;
    page.set_layout_engine_factory(counting);
    page.load_html(
        "<html><body><div id='x' style='width:50px;height:20px'>hi</div></body></html>",
        "https://example.test/");

    pagecore::RenderOptions options;
    options.viewport = pagecore::Viewport{320, 240, 1.0f};

    (void) page.display_list(options);
    require(*counting->layout_calls == 1, "display_list() should run layout() exactly once");

    const pagecore::NodeId box = page.document().query_selector(page.document().document_node(), "#x");
    require(box != pagecore::kInvalidNodeId, "expected to find #x");

    (void) page.element_geometry(box);
    require(
        *counting->layout_calls == 1,
        "element_geometry() after display_list() must reuse the cached layout, not rerun it");

    (void) page.display_list(options);
    require(
        *counting->layout_calls == 1,
        "display_list() after element_geometry() must reuse the cached layout, not rerun it");

    page.eval("document.getElementById('x').setAttribute('style', 'width:70px;height:20px')");
    (void) page.element_geometry(box);
    require(
        *counting->layout_calls == 2,
        "a DOM mutation must invalidate the cache and force a fresh layout() on next access");
}

// New bounded-geometry contract: geometry is exact whenever a current layout
// exists or the budget allows; otherwise the last exact measurement is returned
// as-is (no analytic patching), or nullopt when the node's own subtree changed.
// These caches never influence the final render.
// New bounded contract (approximate-nonzero): once bounded, a geometry read
// returns the last exact measurement AS-IS (never null when a value is known and
// never analytically patched), because JS grid libraries treat null/zero geometry
// as a real measurement and permanently collapse their layout. These caches never
// influence the final render.
void test_element_geometry_bounded_mode_reuses_last_known_after_own_sizing_change()
{
    auto factory = std::make_shared<SlowGeometryFactory>();

    pagecore::Page page;
    page.set_layout_engine_factory(factory);
    page.load_html("<html><body><div id='x'></div></body></html>", "https://example.test/");

    const pagecore::NodeId box = page.document().query_selector(page.document().document_node(), "#x");
    require(box != pagecore::kInvalidNodeId, "expected to find #x");

    auto first = page.element_geometry(box);
    require(first && first->border_box.width == 10.0f, "first geometry read should be exact");
    require(*factory->layout_calls == 1, "first geometry read should force one layout");

    page.document().set_attribute(box, "style", "width:20px");
    auto second = page.element_geometry(box);
    require(second && second->border_box.width == 20.0f, "second geometry read should still be exact");
    require(*factory->layout_calls == 2, "second geometry read should force a second layout");

    page.document().set_attribute(box, "style", "width:30px");
    auto third = page.element_geometry(box);
    require(third && third->border_box.width == 20.0f,
            "bounded mode returns the last exact measurement as approximate (never null) so JS grid libraries do not collapse");
    require(*factory->layout_calls == 2, "bounded mode must not force another full layout");
}

void test_element_geometry_bounded_mode_reuses_descendant_as_approximate_after_ancestor_mutation()
{
    auto factory = std::make_shared<SlowGeometryFactory>();

    pagecore::Page page;
    page.set_layout_engine_factory(factory);
    page.load_html("<html><body><div id='parent'><div id='x'></div></div></body></html>", "https://example.test/");

    const pagecore::NodeId root = page.document().document_node();
    const pagecore::NodeId parent = page.document().query_selector(root, "#parent");
    const pagecore::NodeId box = page.document().query_selector(root, "#x");
    require(parent != pagecore::kInvalidNodeId && box != pagecore::kInvalidNodeId, "expected nested fixture");

    auto first = page.element_geometry(box);
    require(first && first->border_box.width == 10.0f, "first geometry read should be exact");
    page.document().set_attribute(box, "style", "display:block");
    auto second = page.element_geometry(box);
    require(second && second->border_box.width == 20.0f, "second geometry read should enter bounded mode with exact geometry");
    require(*factory->layout_calls == 2, "expected bounded mode to be active after two slow layouts");

    page.document().set_attribute(parent, "style", "width:40px");
    auto third = page.element_geometry(box);
    require(
        third && third->border_box.width == 20.0f,
        "an ancestor-only mutation leaves the descendant's own subtree unchanged, so bounded mode reuses its last exact geometry as approximate (isolated from the paint)");
    require(*factory->layout_calls == 2, "ancestor mutation must not force another full layout in bounded mode");
}

void test_element_geometry_bounded_mode_does_not_analytically_patch_own_position_change()
{
    auto factory = std::make_shared<SlowGeometryFactory>();

    pagecore::Page page;
    page.set_layout_engine_factory(factory);
    page.load_html("<html><body><div id='x'></div></body></html>", "https://example.test/");

    const pagecore::NodeId box = page.document().query_selector(page.document().document_node(), "#x");
    require(box != pagecore::kInvalidNodeId, "expected #x");

    auto first = page.element_geometry(box);
    require(first && first->border_box.width == 10.0f, "first geometry read should be exact");
    page.document().set_attribute(box, "style", "display:block");
    auto second = page.element_geometry(box);
    require(second && second->border_box.width == 20.0f, "second geometry read should be exact");
    require(*factory->layout_calls == 2, "expected bounded mode to be active after two slow layouts");

    page.document().set_attribute(box, "style", "position:absolute;left:25px;top:7px");
    auto third = page.element_geometry(box);
    require(third && third->border_box.width == 20.0f && third->border_box.x == 0.0f && third->border_box.y == 0.0f,
            "bounded mode returns the last exact geometry as-is; it never analytically patches inline left/top into position");
    require(*factory->layout_calls == 2, "own position-only style changes should not force another layout in bounded mode");
}

void test_element_geometry_bounded_mode_reuses_last_known_after_own_relative_position_change()
{
    auto factory = std::make_shared<SlowGeometryFactory>();

    pagecore::Page page;
    page.set_layout_engine_factory(factory);
    page.load_html("<html><body><div id='x'></div></body></html>", "https://example.test/");

    const pagecore::NodeId box = page.document().query_selector(page.document().document_node(), "#x");
    require(box != pagecore::kInvalidNodeId, "expected #x");

    auto first = page.element_geometry(box);
    require(first && first->border_box.width == 10.0f, "first geometry read should be exact");
    page.document().set_attribute(box, "style", "display:block");
    auto second = page.element_geometry(box);
    require(second && second->border_box.width == 20.0f, "second geometry read should be exact");
    require(*factory->layout_calls == 2, "expected bounded mode to be active after two slow layouts");

    page.document().set_attribute(box, "style", "position:relative;left:20px;top:4px");
    auto third = page.element_geometry(box);
    require(third && third->border_box.width == 20.0f && third->border_box.x == 0.0f,
            "relative positioning: bounded mode reuses the last exact geometry as-is, never double-offset");
    require(*factory->layout_calls == 2, "bounded mode must not force another layout for an own-subtree change");
}

void test_element_geometry_bounded_mode_reuses_snapshot_for_unmutated_sibling()
{
    auto factory = std::make_shared<SlowGeometryFactory>();

    pagecore::Page page;
    page.set_layout_engine_factory(factory);
    page.load_html("<html><body><div id='a'></div><div id='b'></div></body></html>", "https://example.test/");

    const pagecore::NodeId root = page.document().document_node();
    const pagecore::NodeId a = page.document().query_selector(root, "#a");
    const pagecore::NodeId b = page.document().query_selector(root, "#b");
    require(a != pagecore::kInvalidNodeId && b != pagecore::kInvalidNodeId, "expected sibling geometry fixture");

    auto first = page.element_geometry(a);
    require(first && first->border_box.width == 10.0f, "first geometry read should be exact");
    page.document().set_attribute(a, "style", "display:block");
    auto second = page.element_geometry(a);
    require(second && second->border_box.width == 20.0f, "second geometry read should be exact and snapshot siblings");
    require(*factory->layout_calls == 2, "expected bounded mode to be active after two slow layouts");

    // Mutate 'a' again (not 'b'); 'b's own subtree is untouched, so a bounded read
    // of 'b' reuses the expensive-layout snapshot without another layout.
    page.document().set_attribute(a, "style", "color:red");
    auto third = page.element_geometry(b);
    require(
        third && third->border_box.width == 20.0f,
        "bounded mode should reuse the expensive-layout snapshot for a sibling whose own subtree did not change");
    require(*factory->layout_calls == 2, "snapshot-backed sibling geometry must not force another full layout");
}

// Regression: an expensive styled document must not skip straight to bounded-null
// geometry before anything is measured. The first read stays exact and snapshots
// every element, so a JS grid library (Isotope/Masonry) that reads each item's
// geometry during load gets real values instead of nulls that collapse the grid.
void test_element_geometry_expensive_document_seeds_snapshot_before_bounded()
{
    auto factory = std::make_shared<ExpensiveGeometryFactory>();

    pagecore::Page page;
    page.set_layout_engine_factory(factory);
    page.load_html("<html><body><div id='a'></div><div id='b'></div></body></html>", "https://example.test/");

    const pagecore::NodeId root = page.document().document_node();
    const pagecore::NodeId a = page.document().query_selector(root, "#a");
    const pagecore::NodeId b = page.document().query_selector(root, "#b");
    require(a != pagecore::kInvalidNodeId && b != pagecore::kInvalidNodeId, "expected sibling fixture");

    auto ga = page.element_geometry(a);
    require(ga && ga->border_box.width == 10.0f,
            "the first geometry read on an expensive document must be exact, not immediately bounded-null");
    require(*factory->layout_calls == 1, "the first read forces exactly one layout");

    // 'b' was never read exactly, but the expensive-layout snapshot must have cached
    // it so a bounded read returns a real (non-null) value.
    auto gb = page.element_geometry(b);
    require(gb.has_value() && gb->border_box.width == 10.0f,
            "a sibling never read exactly must still get non-null geometry from the expensive-layout snapshot");
    require(*factory->layout_calls == 1, "the snapshot must answer the sibling without another layout");
}

void test_element_geometry_bounded_mode_caps_uncached_exact_layouts()
{
    auto factory = std::make_shared<SlowGeometryFactory>();

    pagecore::Page page;
    page.set_layout_engine_factory(factory);
    page.load_html("<html><body><div id='a'></div></body></html>", "https://example.test/");

    const pagecore::NodeId root = page.document().document_node();
    const pagecore::NodeId body = page.document().body();
    const pagecore::NodeId a = page.document().query_selector(root, "#a");
    require(body != pagecore::kInvalidNodeId && a != pagecore::kInvalidNodeId, "expected uncached geometry cap fixture");

    auto first = page.element_geometry(a);
    require(first && first->border_box.width == 10.0f, "first geometry read should be exact");
    page.document().set_attribute(a, "style", "display:block");
    auto second = page.element_geometry(a);
    require(second && second->border_box.width == 20.0f, "second geometry read should still be exact");
    require(*factory->layout_calls == 2, "expected bounded mode to be active after two slow layouts");

    const pagecore::NodeId b = page.document().create_element("div");
    const pagecore::NodeId appended = page.document().append_child(body, b);
    auto third = page.element_geometry(appended);
    require(!third, "uncached geometry after the bounded threshold should use bounded null geometry");
    require(*factory->layout_calls == 2, "bounded uncached geometry must not force unbounded full layouts");
}

void test_element_geometry_bounded_mode_drops_disconnected_stale_geometry()
{
    auto factory = std::make_shared<SlowGeometryFactory>();

    pagecore::Page page;
    page.set_layout_engine_factory(factory);
    page.load_html("<html><body><div id='x'></div></body></html>", "https://example.test/");

    const pagecore::NodeId root = page.document().document_node();
    const pagecore::NodeId box = page.document().query_selector(root, "#x");
    const pagecore::NodeId parent = page.document().parent_node(box);
    require(box != pagecore::kInvalidNodeId && parent != pagecore::kInvalidNodeId, "expected connected #x");

    (void) page.element_geometry(box);
    page.document().set_attribute(box, "style", "width:20px");
    auto second = page.element_geometry(box);
    require(second && second->border_box.width == 20.0f, "second geometry read should populate the stale cache");
    require(*factory->layout_calls == 2, "expected bounded mode to be active after two slow layouts");

    page.document().remove_child(parent, box);
    auto removed = page.element_geometry(box);
    require(!removed, "bounded mode must not return stale geometry for a disconnected node");
    require(*factory->layout_calls == 2, "disconnected geometry read must not force another layout");
}

void test_element_geometry_after_heavy_append_child_runs_first_exact_layout()
{
    std::vector<pagecore::PerfEvent> events;
    auto factory = std::make_shared<SlowGeometryFactory>();

    pagecore::LoadOptions load_options;
    load_options.perf_trace = [&](const pagecore::PerfEvent& event) {
        events.push_back(event);
    };

    std::string filler(60'000, 'x');
    pagecore::Page page(load_options);
    page.set_layout_engine_factory(factory);
    page.load_html("<html><body><main>" + filler + "</main></body></html>", "https://example.test/");

    const pagecore::NodeId body = page.document().body();
    const pagecore::NodeId box = page.document().create_element("div");
    page.document().append_child(body, box);

    auto geometry = page.element_geometry(box);
    require(geometry && geometry->border_box.width == 10.0f, "first append-child geometry read should be exact");
    require(*factory->layout_calls == 1, "first append-child geometry read should run one layout");

    const bool saw_preflight_event = std::any_of(events.begin(), events.end(), [&](const pagecore::PerfEvent& event) {
        return event.phase == pagecore::PerfPhase::Geometry
            && event.name == "element_geometry"
            && event.node_id == box
            && event.styled_document_cache_reason == "preflight_append_child:empty";
    });
    require(!saw_preflight_event, "geometry reads must not preflight newly appended nodes to zero geometry");
}

void test_element_geometry_after_heavy_structural_mutation_runs_first_exact_layout()
{
    std::vector<pagecore::PerfEvent> events;
    auto factory = std::make_shared<SlowGeometryFactory>();

    pagecore::LoadOptions load_options;
    load_options.perf_trace = [&](const pagecore::PerfEvent& event) {
        events.push_back(event);
    };

    std::string filler(60'000, 'x');
    pagecore::Page page(load_options);
    page.set_layout_engine_factory(factory);
    page.load_html(
        "<html><body><main id='target'>" + filler + "</main><aside id='removed'></aside></body></html>",
        "https://example.test/");

    const pagecore::NodeId root = page.document().document_node();
    const pagecore::NodeId target = page.document().query_selector(root, "#target");
    const pagecore::NodeId removed = page.document().query_selector(root, "#removed");
    const pagecore::NodeId body = page.document().body();
    require(target != pagecore::kInvalidNodeId && removed != pagecore::kInvalidNodeId, "expected fixture nodes");

    page.document().remove_child(body, removed);
    auto geometry = page.element_geometry(target);
    require(geometry && geometry->border_box.width == 10.0f, "first structural geometry read should be exact");
    require(*factory->layout_calls == 1, "first structural geometry read should run one layout");

    const bool saw_preflight_event = std::any_of(events.begin(), events.end(), [&](const pagecore::PerfEvent& event) {
        return event.phase == pagecore::PerfPhase::Geometry
            && event.name == "element_geometry"
            && event.node_id == target
            && event.styled_document_cache_reason == "preflight_heavy_structural:empty";
    });
    require(!saw_preflight_event, "geometry reads must not preflight heavy structural mutations to zero geometry");
}

void test_computed_style_property_bounded_mode_returns_inline_without_rebuild()
{
    std::vector<pagecore::PerfEvent> events;
    auto factory = std::make_shared<SlowComputedStyleFactory>();

    pagecore::LoadOptions load_options;
    load_options.perf_trace = [&](const pagecore::PerfEvent& event) {
        events.push_back(event);
    };

    pagecore::Page page(load_options);
    page.set_layout_engine_factory(factory);
    page.load_html("<html><body><div id='x'></div></body></html>", "https://example.test/");

    const pagecore::NodeId box = page.document().query_selector(page.document().document_node(), "#x");
    require(box != pagecore::kInvalidNodeId, "expected to find #x");

    auto first = page.computed_style_property(box, "width");
    require(first && *first == "10px", "first computed style property read should be exact");
    require(*factory->compute_calls == 1, "first computed style property read should compute styles");

    page.document().set_attribute(box, "style", "width:20px");
    auto second = page.computed_style_property(box, "width");
    require(second && *second == "20px", "inline width should be read after the second exact rebuild");
    require(*factory->compute_calls == 2, "second post-mutation read should still be exact");

    page.document().set_attribute(box, "style", "width:30px");
    auto third = page.computed_style_property(box, "width");
    require(third && *third == "30px", "bounded mode should return inline style values");
    require(*factory->compute_calls == 2, "bounded mode must not force another computed style rebuild");

    const bool saw_bounded_event = std::any_of(events.begin(), events.end(), [&](const pagecore::PerfEvent& event) {
        return event.phase == pagecore::PerfPhase::ComputedStyle
            && event.name == "computed_style_property"
            && event.property == "width"
            && event.styled_document_cache_reason == "bounded_mode:layout_mutation_version_changed";
    });
    require(saw_bounded_event, "perf trace should identify bounded computed-style property reads");
}

// Regression: bounded mode must never fabricate a CSS initial value for a property
// the cascade actually sets. The last forced rebuild before bounded mode trips
// snapshots every element's cascade, so a stylesheet-hidden element keeps
// reporting `display: none`. Fabricating `block` here made jQuery's .show() treat
// `.wrapper` as already visible and left ukr.net rendering a blank page.
void test_computed_style_property_bounded_mode_keeps_stylesheet_display()
{
    auto factory = std::make_shared<StylesheetHiddenComputedStyleFactory>();

    pagecore::Page page;
    page.set_layout_engine_factory(factory);
    page.load_html("<html><body><div id='x'></div></body></html>", "https://example.test/");

    const pagecore::NodeId box = page.document().query_selector(page.document().document_node(), "#x");
    require(box != pagecore::kInvalidNodeId, "expected to find #x");

    auto first = page.computed_style_property(box, "width");
    require(first && *first == "10px", "first computed style property read should be exact");
    require(*factory->compute_calls == 1, "first read should build the cascade once");

    // Own-attribute mutations change #x's digest, so each read forces a rebuild.
    // The second one exhausts the budget (2 x 80ms > 100ms) and trips bounded mode.
    page.document().set_attribute(box, "class", "a");
    auto second = page.computed_style_property(box, "width");
    require(second && *second == "20px", "second post-mutation read should still be exact");
    require(*factory->compute_calls == 2, "second read should force the second rebuild");

    page.document().set_attribute(box, "class", "b");
    auto display = page.computed_style_property(box, "display");
    require(*factory->compute_calls == 2, "bounded mode must not force another computed-style rebuild");
    require(
        display && *display == "none",
        "bounded mode must report the snapshotted cascade value, not the `display: block` CSS initial value");
}

// Regression, second half: an element created AFTER bounded mode tripped is absent
// from every snapshot, so the backstop has nothing cached for it. It must still get
// one exact cascade rather than a CSS initial value. This is ukr.net's exact shape:
// scripts build `div.wrapper` late, then jQuery's .show() reads its `display`.
void test_computed_style_property_bounded_mode_resolves_element_created_after_trip()
{
    std::vector<pagecore::PerfEvent> events;
    auto factory = std::make_shared<StylesheetHiddenComputedStyleFactory>();

    pagecore::LoadOptions load_options;
    load_options.perf_trace = [&](const pagecore::PerfEvent& event) {
        events.push_back(event);
    };

    pagecore::Page page(load_options);
    page.set_layout_engine_factory(factory);
    page.load_html("<html><body><div id='x'></div></body></html>", "https://example.test/");

    const pagecore::NodeId box = page.document().query_selector(page.document().document_node(), "#x");
    require(box != pagecore::kInvalidNodeId, "expected to find #x");

    (void) page.computed_style_property(box, "width");
    page.document().set_attribute(box, "class", "a");
    (void) page.computed_style_property(box, "width");
    require(*factory->compute_calls == 2, "two forced rebuilds should exhaust the budget and trip bounded mode");

    // Created after the trip: no snapshot covers it, no cache entry exists.
    const pagecore::NodeId late = page.document().create_element("div");
    page.document().append_child(page.document().body(), late);

    auto display = page.computed_style_property(late, "display");
    require(
        display && *display == "none",
        "an element first seen in bounded mode must get an exact cascade, not the `display: block` initial value");
    require(*factory->compute_calls == 3, "resolving the unknown element should cost exactly one rebuild");

    // That rebuild snapshotted every connected element at this version, so an
    // element never read directly also resolves to its real cascade.
    auto sibling = page.computed_style_property(box, "display");
    require(sibling && *sibling == "none", "the snapshot should cover every connected element");

    const bool saw_snapshot_event = std::any_of(events.begin(), events.end(), [&](const pagecore::PerfEvent& event) {
        return event.phase == pagecore::PerfPhase::ComputedStyle
            && event.name == "computed_style_property"
            && event.property == "display"
            && event.styled_document_cache_reason.rfind("bounded_mode_snapshot:", 0) == 0;
    });
    require(saw_snapshot_event, "perf trace should identify the bounded-mode snapshot rebuild");
}

// The first computed-style read after appending a node is now exact (no
// preflight-to-default guessing on the common path): it builds the cascade once
// and returns the real value.
void test_computed_style_property_after_append_runs_first_exact()
{
    std::vector<pagecore::PerfEvent> events;
    auto factory = std::make_shared<SlowComputedStyleFactory>();

    pagecore::LoadOptions load_options;
    load_options.perf_trace = [&](const pagecore::PerfEvent& event) {
        events.push_back(event);
    };

    std::string filler(60'000, 'x');
    pagecore::Page page(load_options);
    page.set_layout_engine_factory(factory);
    page.load_html("<html><body><main>" + filler + "</main></body></html>", "https://example.test/");

    const pagecore::NodeId body = page.document().body();
    const pagecore::NodeId box = page.document().create_element("div");
    page.document().append_child(body, box);

    auto width = page.computed_style_property(box, "width");
    require(width && *width == "10px", "first computed style read after append should be exact, not a preflight default");
    require(*factory->compute_calls == 1, "first read after append should build the cascade exactly once");

    const bool saw_preflight_event = std::any_of(events.begin(), events.end(), [&](const pagecore::PerfEvent& event) {
        return event.phase == pagecore::PerfPhase::ComputedStyle
            && event.name == "computed_style_property"
            && event.property == "width"
            && event.styled_document_cache_reason == "preflight_append_child:empty";
    });
    require(!saw_preflight_event, "computed style reads must not preflight appended nodes to a default value");
}

// Sound digest reuse: a layout-version-bumping mutation to a cousin that does NOT
// change node N's cascade inputs must be answered from N's cached value without a
// rebuild.
void test_computed_style_property_reuses_digest_across_unrelated_mutation()
{
    std::vector<pagecore::PerfEvent> events;
    auto factory = std::make_shared<SlowComputedStyleFactory>();

    pagecore::LoadOptions load_options;
    load_options.perf_trace = [&](const pagecore::PerfEvent& event) {
        events.push_back(event);
    };

    pagecore::Page page(load_options);
    page.set_layout_engine_factory(factory);
    page.load_html(
        "<html><body><div id='x'></div><div id='container'><div id='y'></div></div></body></html>",
        "https://example.test/");

    const pagecore::NodeId root = page.document().document_node();
    const pagecore::NodeId x = page.document().query_selector(root, "#x");
    const pagecore::NodeId y = page.document().query_selector(root, "#y");
    require(x != pagecore::kInvalidNodeId && y != pagecore::kInvalidNodeId, "expected cousin fixture");

    auto first = page.computed_style_property(x, "width");
    require(first && *first == "10px", "first read should be exact");
    require(*factory->compute_calls == 1, "first read should build the cascade once");

    // A class change on a cousin bumps the layout version but does not touch x's
    // cascade inputs (x's sibling context only sees #container's own identity, not
    // its descendants), so x's digest is unchanged and reuse must avoid a rebuild.
    page.document().set_attribute(y, "class", "z");
    auto second = page.computed_style_property(x, "width");
    require(second && *second == "10px", "digest reuse should return the unchanged value");
    require(*factory->compute_calls == 1, "an unrelated cousin mutation must not force a computed-style rebuild");

    const bool saw_reuse = std::any_of(events.begin(), events.end(), [&](const pagecore::PerfEvent& event) {
        return event.phase == pagecore::PerfPhase::ComputedStyle
            && event.name == "computed_style_property"
            && event.property == "width"
            && event.styled_document_cache_reason.rfind("digest_reuse:", 0) == 0;
    });
    require(saw_reuse, "perf trace should identify sound digest-reuse reads");
}

namespace {

// Widest white (#fff) fill in a display list, used by the render regressions to
// read back an absolute element's rendered width.
float widest_white_fill(const pagecore::DisplayList& display)
{
    float widest = 0.0f;
    for (const auto& command : display.commands) {
        const auto* fill = std::get_if<pagecore::SolidFillCommand>(&command);
        if (fill && fill->color.r == 255 && fill->color.g == 255 && fill->color.b == 255) {
            widest = std::max(widest, fill->rect.width);
        }
    }
    return widest;
}

} // namespace

// Regression (i): a width measured at one viewport must never be injected into a
// render at a different viewport. The absolute-% correction is recomputed within
// each render, so the 640 paint reflects 640, not a value measured at 1280.
void test_render_never_injects_cross_viewport_measured_width()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html><head><style>
  body { margin:0; }
  .wrap { position:relative; width:100%; height:120px; }
  .cell { position:absolute; left:0; top:0; width:25%; box-sizing:border-box; }
  .card { width:100%; height:80px; background:#fff; }
</style></head><body>
  <div class="wrap"><div class="cell" id="cell"><div class="card" id="card"></div></div></div>
</body></html>
)HTML", "https://example.test/index.html");

    const pagecore::NodeId cell = page.document().query_selector(page.document().document_node(), "#cell");
    require(cell != pagecore::kInvalidNodeId, "expected #cell");

    // Prime read-time caches by measuring at the wide viewport.
    pagecore::RenderOptions wide;
    wide.viewport = pagecore::Viewport{1280, 720, 1.0f};
    const float wide_card = widest_white_fill(page.display_list(wide));
    (void) page.element_geometry(cell);
    require(wide_card > 280.0f, "at 1280 the 25% absolute cell's card should be ~320px");

    // Render at a narrower viewport: the card must reflect 640, never 1280.
    pagecore::RenderOptions narrow;
    narrow.viewport = pagecore::Viewport{640, 480, 1.0f};
    const float narrow_card = widest_white_fill(page.display_list(narrow));
    require(
        narrow_card > 130.0f && narrow_card < 200.0f,
        "the 640 render must use the 640-derived absolute-% width, never a width measured at 1280");
}

// Regression: the render-local absolute-%-width correction must pin litehtml's OWN
// laid-out width, never a re-derived (containing-block) formula that can be narrower
// and shrink a correctly-sized grid cell. Here litehtml lays the padded 50% cell out
// at ~240px, so its width:100% card should render at ~200px; a formula that pinned
// 50%*400 = 200px on the cell would shrink the card to ~160px.
void test_render_absolute_percent_correction_pins_native_width_not_narrower()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html><head><style>
  body { margin:0; }
  .wrap { position:relative; width:400px; height:100px; }
  .cell { position:absolute; left:0; top:0; width:50%; padding:0 20px; box-sizing:border-box; }
  .card { width:100%; height:40px; background:#fff; }
</style></head><body>
  <div class="wrap"><div class="cell" id="cell"><div class="card" id="card"></div></div></div>
</body></html>
)HTML", "https://example.test/index.html");

    pagecore::RenderOptions options;
    options.viewport = pagecore::Viewport{800, 200, 1.0f};
    const float card = widest_white_fill(page.display_list(options));
    require(
        card > 190.0f,
        "the abs-% correction must pin litehtml's own cell width so the width:100% child stays full-width, never narrow it with a re-derived formula");
}

// Lays the same document out through both engine inputs — load_html() over the
// layout serialization and load_dom() over the live DOM — and returns both
// display-list JSON dumps for byte comparison.
struct LayoutInputParity {
    std::string html_json;
    std::string dom_json;
    std::unique_ptr<pagecore::LayoutEngine> html_engine;
    std::unique_ptr<pagecore::LayoutEngine> dom_engine;
};

LayoutInputParity run_layout_input_parity(
    pagecore::DomDocument& doc,
    bool omit_js_disabled_content = false,
    const std::vector<pagecore::DomDocument::LayoutStyleOverride>& overrides = {})
{
    const pagecore::Viewport viewport{800, 600, 1.0f};
    const std::string base_url = "https://example.test/page/index.html";

    LayoutInputParity result;

    result.html_engine = pagecore::create_litehtml_layout_engine();
    result.html_engine->set_viewport(viewport);
    result.html_engine->load_html(
        doc.serialize_html_for_layout(omit_js_disabled_content, overrides), base_url);
    result.html_engine->layout();
    result.html_json = pagecore::display_list_to_json(result.html_engine->display_list());

    result.dom_engine = pagecore::create_litehtml_layout_engine();
    result.dom_engine->set_viewport(viewport);
    pagecore::LayoutEngine::DomLayoutRequest request;
    request.document = &doc;
    request.base_url = base_url;
    request.omit_js_disabled_content = omit_js_disabled_content;
    request.style_overrides = &overrides;
    require(result.dom_engine->load_dom(request), "litehtml engine must support direct DOM input");
    result.dom_engine->layout();
    result.dom_json = pagecore::display_list_to_json(result.dom_engine->display_list());

    return result;
}

void test_layout_engine_load_dom_matches_load_html_display_list()
{
    const std::vector<std::pair<std::string, std::string>> corpus = {
        {"headings_paragraphs_inline_styles",
         "<!DOCTYPE html><html><head><title>t</title></head><body>"
         "<h1>Header</h1><p style='color:rgb(200,10,10)'>styled <b>bold</b> tail</p>"
         "<p>plain</p></body></html>"},
        {"floats",
         "<!DOCTYPE html><html><body>"
         "<div style='float:left;width:100px;height:40px;background:rgb(10,10,200)'></div>"
         "<div style='float:right;width:80px;height:30px;background:rgb(10,200,10)'></div>"
         "<p>flow text around floats</p></body></html>"},
        {"table",
         "<!DOCTYPE html><html><body><table border='1'>"
         "<tr><td>a</td><td>b</td></tr><tr><td colspan='2'>wide</td></tr>"
         "</table></body></html>"},
        {"absolute_positioning",
         "<!DOCTYPE html><html><body style='margin:0'>"
         "<div style='position:relative;width:300px;height:100px'>"
         "<div style='position:absolute;left:20px;top:10px;width:50px;height:20px;"
         "background:rgb(250,120,10)'></div></div></body></html>"},
        {"style_element",
         "<!DOCTYPE html><html><head><style>"
         ".card{width:150px;height:60px;background:rgb(90,90,90);margin:4px}"
         "p{color:rgb(20,20,120)}"
         "</style></head><body><div class='card'></div><p>styled paragraph</p></body></html>"},
        {"entities",
         "<!DOCTYPE html><html><body><p>&amp; &lt; &gt; and&nbsp;nbsp</p></body></html>"},
        {"cjk_and_whitespace_runs",
         "<!DOCTYPE html><html><body><p>Latin 漢字テスト mixed\t runs\n here</p></body></html>"},
        {"comments_in_text",
         "<!DOCTYPE html><html><body><p>before<!-- split -->after</p></body></html>"},
        {"base_href",
         "<!DOCTYPE html><html><head><base href='https://other.test/root/'></head>"
         "<body><p>based</p></body></html>"},
        {"template_content",
         "<!DOCTYPE html><html><body><template><p>tpl</p></template><p>visible</p></body></html>"},
        {"svg_island",
         "<!DOCTYPE html><html><body><svg width='40' height='40'>"
         "<rect width='30' height='20'></rect></svg><p>after svg</p></body></html>"},
    };

    for (const auto& [name, html] : corpus) {
        pagecore::DomDocument doc;
        doc.parse(html);
        const auto parity = run_layout_input_parity(doc);
        require(parity.html_json == parity.dom_json,
                "load_dom display list must match load_html byte-for-byte for corpus case: " + name);
        require(parity.dom_json.find("\"commands\":[]") == std::string::npos,
                "corpus case must produce a non-empty display list: " + name);
    }
}

void test_layout_engine_load_dom_quirks_class_matching()
{
    // No doctype: quirks mode, where class selectors match ASCII
    // case-insensitively. The direct path must seed litehtml's document mode
    // before creating elements or .abc would not match class="ABC".
    pagecore::DomDocument doc;
    doc.parse(
        "<html><head><style>.abc{width:200px;height:50px;background:rgb(200,30,30)}</style></head>"
        "<body><div class='ABC'></div></body></html>");
    require(doc.quirks_mode() == pagecore::DomDocument::QuirksMode::Quirks,
            "doctype-less fixture must parse in quirks mode");

    const auto parity = run_layout_input_parity(doc);
    require(parity.html_json == parity.dom_json, "quirks display lists must match byte-for-byte");
    require(parity.dom_json.find("solidFill") != std::string::npos,
            "quirks-mode class selector must match the uppercase class in the direct path");
}

void test_layout_engine_load_dom_computed_style_and_geometry()
{
    pagecore::DomDocument doc;
    doc.parse(
        "<!DOCTYPE html><html><head><style>"
        "#x{width:120px;height:44px;padding:6px;border:2px solid rgb(0,0,0);margin:10px;"
        "color:rgb(10,110,210);font-size:18px;float:left}"
        "</style></head><body><div id='x'>probe</div></body></html>");

    auto parity = run_layout_input_parity(doc);
    const std::string key = std::to_string(doc.get_element_by_id("x"));

    const auto html_style = parity.html_engine->computed_style(key);
    const auto dom_style = parity.dom_engine->computed_style(key);
    require(html_style.has_value() && dom_style.has_value(),
            "both engine inputs must resolve the tagged element's computed style");
    require(html_style->properties == dom_style->properties,
            "computed style properties must be identical across engine inputs");

    const auto html_geometry = parity.html_engine->element_geometry(key);
    const auto dom_geometry = parity.dom_engine->element_geometry(key);
    require(html_geometry.has_value() && dom_geometry.has_value(),
            "both engine inputs must resolve the tagged element's geometry");
    auto rect_equal = [](const pagecore::Rect& a, const pagecore::Rect& b) {
        return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
    };
    require(rect_equal(html_geometry->border_box, dom_geometry->border_box),
            "border boxes must be identical across engine inputs");
    require(rect_equal(html_geometry->padding_box, dom_geometry->padding_box),
            "padding boxes must be identical across engine inputs");
}

void test_layout_engine_load_dom_collects_absolute_percent_overrides()
{
    pagecore::DomDocument doc;
    doc.parse(
        "<!DOCTYPE html><html><head><style>"
        "body{margin:0}"
        ".wrap{position:relative;width:400px;height:100px}"
        ".cell{position:absolute;left:0;top:0;width:50%;box-sizing:border-box}"
        ".card{width:100%;height:40px;background:rgb(255,255,255)}"
        "</style></head><body>"
        "<div class='wrap'><div class='cell'><div class='card'></div></div></div>"
        "</body></html>");

    auto parity = run_layout_input_parity(doc);

    const auto html_overrides = parity.html_engine->collect_absolute_percent_width_overrides();
    const auto dom_overrides = parity.dom_engine->collect_absolute_percent_width_overrides();
    require(!dom_overrides.empty(), "the abs-% fixture must produce overrides in the direct path");
    require(html_overrides.size() == dom_overrides.size(),
            "override counts must match across engine inputs");
    for (std::size_t i = 0; i < html_overrides.size(); ++i) {
        require(html_overrides[i].node_key == dom_overrides[i].node_key,
                "override node keys must match across engine inputs");
        require(html_overrides[i].border_box_width_px == dom_overrides[i].border_box_width_px,
                "override widths must match across engine inputs");
    }
}

// Loads a fresh direct-DOM engine on the current state of `doc`, lays it out,
// and returns the display-list JSON — the ground truth an in-place patch must
// reproduce.
std::string fresh_direct_dom_display_list_json(
    pagecore::DomDocument& doc,
    const pagecore::Viewport& viewport,
    std::string_view base_url)
{
    auto engine = pagecore::create_litehtml_layout_engine();
    engine->set_viewport(viewport);
    pagecore::LayoutEngine::DomLayoutRequest request;
    request.document = &doc;
    request.base_url = base_url;
    require(engine->load_dom(request), "litehtml engine must support direct DOM input");
    engine->layout();
    return pagecore::display_list_to_json(engine->display_list());
}

std::unique_ptr<pagecore::LayoutEngine> laid_out_direct_dom_engine(
    pagecore::DomDocument& doc,
    const pagecore::Viewport& viewport,
    std::string_view base_url)
{
    auto engine = pagecore::create_litehtml_layout_engine();
    engine->set_viewport(viewport);
    pagecore::LayoutEngine::DomLayoutRequest request;
    request.document = &doc;
    request.base_url = base_url;
    require(engine->load_dom(request), "litehtml engine must support direct DOM input");
    engine->layout();
    return engine;
}

pagecore::LayoutEngine::InlineStylePatch style_patch_from_dom(
    pagecore::DomDocument& doc,
    pagecore::NodeId node)
{
    const auto style = doc.get_attribute(node, "style");
    return pagecore::LayoutEngine::InlineStylePatch{std::to_string(node), style.value_or(std::string())};
}

void test_layout_engine_inline_style_patch_matches_fresh_build()
{
    const pagecore::Viewport viewport{600, 400, 1.0f};
    const std::string base_url = "https://example.test/";
    const char* html =
        "<!DOCTYPE html><html><body style='margin:0'>"
        "<div id='a' style='width:100px;height:30px;background:rgb(200,10,10)'>a</div>"
        "<div id='b' style='width:80px;height:20px;background:rgb(10,10,200)'>b</div>"
        "</body></html>";

    pagecore::DomDocument doc;
    doc.parse(html);
    const pagecore::NodeId a = doc.get_element_by_id("a");
    const pagecore::NodeId b = doc.get_element_by_id("b");

    auto engine = laid_out_direct_dom_engine(doc, viewport, base_url);

    // Mutate inline styles on the DOM, then apply the same changes in place.
    doc.set_attribute(a, "style", "width:160px;height:50px;background:rgb(200,10,10)");
    doc.set_attribute(b, "style", "width:40px;height:60px;background:rgb(10,10,200)");

    std::vector<pagecore::LayoutEngine::InlineStylePatch> patches = {
        style_patch_from_dom(doc, a),
        style_patch_from_dom(doc, b),
    };
    require(engine->apply_inline_style_patches(patches), "inline style patches must apply to live render items");
    engine->layout();
    const std::string patched_json = pagecore::display_list_to_json(engine->display_list());

    const std::string fresh_json = fresh_direct_dom_display_list_json(doc, viewport, base_url);
    require(patched_json == fresh_json,
            "an in-place inline-style patch must reproduce a fresh build byte-for-byte");
}

void test_layout_engine_inline_style_patch_unknown_key_is_noop()
{
    const pagecore::Viewport viewport{400, 300, 1.0f};
    const std::string base_url = "https://example.test/";

    pagecore::DomDocument doc;
    doc.parse("<html><body><div id='a' style='width:50px;height:20px'>a</div></body></html>");
    const pagecore::NodeId a = doc.get_element_by_id("a");

    auto engine = laid_out_direct_dom_engine(doc, viewport, base_url);
    const std::string before = pagecore::display_list_to_json(engine->display_list());

    std::vector<pagecore::LayoutEngine::InlineStylePatch> patches = {
        {std::to_string(a), "width:90px;height:20px"},
        {"999999", "width:10px"},  // no such element
    };
    require(!engine->apply_inline_style_patches(patches),
            "a patch batch with an unknown key must fail");

    engine->layout();
    const std::string after = pagecore::display_list_to_json(engine->display_list());
    require(before == after,
            "a failed patch batch must not have mutated any element (validate-all-before-mutate)");
}

void test_layout_engine_inline_style_patch_inherited_properties()
{
    const pagecore::Viewport viewport{500, 300, 1.0f};
    const std::string base_url = "https://example.test/";
    const char* html =
        "<!DOCTYPE html><html><body>"
        "<div id='parent' style='font-size:16px;color:rgb(0,0,0)'>"
        "<span id='child'>inherited text</span></div>"
        "</body></html>";

    pagecore::DomDocument doc;
    doc.parse(html);
    const pagecore::NodeId parent = doc.get_element_by_id("parent");
    const pagecore::NodeId child = doc.get_element_by_id("child");

    auto engine = laid_out_direct_dom_engine(doc, viewport, base_url);

    // Change an inherited property on the parent only.
    doc.set_attribute(parent, "style", "font-size:32px;color:rgb(220,0,0)");
    require(engine->apply_inline_style_patches({style_patch_from_dom(doc, parent)}),
            "the inherited-property patch must apply");
    engine->layout();
    const std::string patched_json = pagecore::display_list_to_json(engine->display_list());

    const std::string fresh_json = fresh_direct_dom_display_list_json(doc, viewport, base_url);
    require(patched_json == fresh_json,
            "an inherited-property patch must propagate to descendant text metrics like a fresh build");

    // The child's computed font-size must reflect the inherited change.
    const auto child_font = engine->computed_style_property(std::to_string(child), "font-size");
    require(child_font && child_font->find("32") != std::string::npos,
            "the child must inherit the parent's new font-size after the patch");
}

void test_layout_engine_load_dom_drops_refused_style_child()
{
    // A JS-created element child of <style> is refused by litehtml (el_style
    // accepts only text). It must not be registered by NodeId, matching the
    // gumbo/serialized path which never materializes it, so computed_style
    // resolves nullopt rather than an un-cascaded orphan.
    pagecore::Page page;
    page.load_html("<html><head><style id='s'>.a{}</style></head><body></body></html>", "https://example.test/");
    page.eval(
        "var sp = document.createElement('span');"
        "sp.id = 'orphan';"
        "sp.textContent = 'x';"
        "document.getElementById('s').appendChild(sp);");

    pagecore::RenderOptions options;
    options.viewport = pagecore::Viewport{400, 300, 1.0f};
    (void) page.display_list(options);

    const pagecore::NodeId orphan = page.document().get_element_by_id("orphan");
    require(orphan != pagecore::kInvalidNodeId, "the JS-created span must exist in the DOM");
    require(!page.computed_style(orphan).has_value(),
            "a style-refused element must not resolve a computed style (no un-cascaded orphan)");
}

void test_layout_engine_inline_style_patch_display_none_target_refuses()
{
    const pagecore::Viewport viewport{400, 300, 1.0f};
    const std::string base_url = "https://example.test/";

    pagecore::DomDocument doc;
    doc.parse("<html><body><div id='a' style='display:none'>a</div></body></html>");
    const pagecore::NodeId a = doc.get_element_by_id("a");

    auto engine = laid_out_direct_dom_engine(doc, viewport, base_url);

    // A display:none element has no render item, so an in-place patch cannot
    // stand in for a rebuild.
    require(!engine->apply_inline_style_patches({{std::to_string(a), "display:block;width:20px;height:10px"}}),
            "a display:none target has no render item and must refuse the patch");
}

// Full-pipeline fixture for the layout-tree-input A/B comparison: external
// stylesheet, image, inline <style>, noscript, and JS-driven DOM mutations.
std::string page_display_list_json_for_layout_input(pagecore::LayoutTreeInput input, bool enable_js = true)
{
    pagecore::LoadOptions load_options;
    load_options.layout_tree_input = input;
    load_options.enable_js = enable_js;

    auto loader = std::make_shared<RecordingResourceLoader>();
    loader->add("https://example.test/app.css",
                ".linked{width:140px;height:36px;background:rgb(20,120,220)}",
                "text/css");
    loader->add("https://example.test/img.png", png_body(pagecore::Color{200, 40, 40, 255}), "image/png");

    pagecore::Page page(load_options);
    page.set_resource_loader(loader);
    page.load_html(R"HTML(
<!DOCTYPE html>
<html><head>
<link rel="stylesheet" href="app.css">
<style>#anchor{color:rgb(10,20,30)}</style>
</head><body>
<div class="linked"></div>
<img src="img.png" width="20" height="20">
<p id="anchor">static text</p>
<noscript><div style="width:44px;height:22px;background:rgb(240,20,30)">fallback</div></noscript>
<script>
  var extra = document.createElement('div');
  extra.setAttribute('style', 'width:60px;height:12px;background:rgb(5,150,60)');
  extra.textContent = 'created';
  document.body.appendChild(extra);
  document.getElementById('anchor').style.fontSize = '20px';
</script>
</body></html>
)HTML", "https://example.test/index.html");

    pagecore::RenderOptions options;
    options.viewport = pagecore::Viewport{640, 480, 1.0f};
    return pagecore::display_list_to_json(page.display_list(options));
}

void test_attr_function_makes_data_attribute_layout_sensitive()
{
    // Regression: content:attr(data-*) reads an attribute directly, outside any
    // selector. Writing that attribute must invalidate the memoized display list;
    // otherwise the service-attribute fast path drops the mutation and the cached
    // (stale) render is returned.
    pagecore::Page page;
    page.load_html(R"HTML(
<html><head><style>.badge::before{content:attr(data-count)}</style></head>
<body><span id="b" class="badge" data-count="3">x</span></body></html>
)HTML", "https://example.test/index.html");

    pagecore::RenderOptions options;
    options.viewport = pagecore::Viewport{640, 480, 1.0f};

    const std::string before = pagecore::display_list_to_json(page.display_list(options));
    require(before.find("\"text\":\"3\"") != std::string::npos,
            "generated content from content:attr() should render the initial attribute value");

    page.eval("document.getElementById('b').setAttribute('data-count','7');");
    const std::string after = pagecore::display_list_to_json(page.display_list(options));

    require(before != after,
            "changing an attribute read by content:attr() must invalidate the memoized render "
            "(not serve a stale display list)");
    require(after.find("\"text\":\"7\"") != std::string::npos,
            "the updated content:attr() value must appear in the re-rendered display list");
}

void test_cdata_layout_parity_direct_vs_serialized()
{
    // A CDATA section in foreign (SVG) content must be handled identically by the
    // direct-DOM and serialized layout-input paths, so neither leaks CDATA text
    // into visible layout that the other omits.
    const char* html = R"HTML(<!DOCTYPE html>
<html><body>
<svg width="100" height="40"><text x="0" y="20"><![CDATA[cdata-payload]]></text></svg>
<p>visible</p>
</body></html>)HTML";

    const auto render = [&](pagecore::LayoutTreeInput input) {
        pagecore::LoadOptions opts;
        opts.layout_tree_input = input;
        opts.enable_js = false;
        pagecore::Page page(opts);
        page.load_html(html, "https://example.test/");
        pagecore::RenderOptions ro;
        ro.viewport = pagecore::Viewport{200, 100, 1.0f};
        return pagecore::display_list_to_json(page.display_list(ro));
    };

    require(render(pagecore::LayoutTreeInput::DirectDom)
                == render(pagecore::LayoutTreeInput::SerializedHtml),
            "CDATA sections must produce an identical display list on the direct-DOM "
            "and serialized layout-input paths");
}

void test_page_direct_dom_layout_display_list_parity()
{
    const std::string serialized =
        page_display_list_json_for_layout_input(pagecore::LayoutTreeInput::SerializedHtml);
    const std::string direct =
        page_display_list_json_for_layout_input(pagecore::LayoutTreeInput::DirectDom);

    require(direct.find("created") != std::string::npos,
            "JS-created content must render through the direct-DOM path");
    require(direct.find("\"r\":240,\"g\":20,\"b\":30") == std::string::npos,
            "noscript fallback must not render when JS is enabled");
    require(serialized == direct,
            "the direct-DOM page display list must match the serialized path byte-for-byte");
}

void test_page_direct_dom_noscript_and_enable_js()
{
    const std::string serialized =
        page_display_list_json_for_layout_input(pagecore::LayoutTreeInput::SerializedHtml, /*enable_js=*/false);
    const std::string direct =
        page_display_list_json_for_layout_input(pagecore::LayoutTreeInput::DirectDom, /*enable_js=*/false);

    require(direct.find("\"r\":240,\"g\":20,\"b\":30") != std::string::npos,
            "noscript fallback must render through the direct-DOM path when JS is disabled");
    require(direct.find("created") == std::string::npos,
            "scripts must not run when JS is disabled");
    require(serialized == direct,
            "the JS-disabled direct-DOM display list must match the serialized path byte-for-byte");
}

void test_page_direct_dom_absolute_percent_second_pass()
{
    const char* html = R"HTML(
<html><head><style>
  body { margin:0; }
  .wrap { position:relative; width:400px; height:100px; }
  .cell { position:absolute; left:0; top:0; width:50%; box-sizing:border-box; }
  .card { width:100%; height:40px; background:#fff; }
</style></head><body>
  <div class="wrap"><div class="cell"><div class="card"></div></div></div>
</body></html>
)HTML";

    pagecore::RenderOptions options;
    options.viewport = pagecore::Viewport{800, 200, 1.0f};

    pagecore::Page serialized_page;
    serialized_page.load_html(html, "https://example.test/index.html");
    const std::string serialized_json = pagecore::display_list_to_json(serialized_page.display_list(options));

    pagecore::LoadOptions direct_options;
    direct_options.layout_tree_input = pagecore::LayoutTreeInput::DirectDom;
    pagecore::Page direct_page(direct_options);
    direct_page.load_html(html, "https://example.test/index.html");

    std::vector<pagecore::PerfEvent> events;
    pagecore::RenderOptions traced = options;
    traced.perf_trace = [&](const pagecore::PerfEvent& event) { events.push_back(event); };
    const std::string direct_json = pagecore::display_list_to_json(direct_page.display_list(traced));

    require(serialized_json == direct_json,
            "the corrected absolute-% display list must match across layout tree inputs");

    const auto corrected_load_dom = std::any_of(events.begin(), events.end(), [](const pagecore::PerfEvent& e) {
        return e.phase == pagecore::PerfPhase::LitehtmlLoadHtml
            && e.name == "load_dom"
            && e.property.find("absolute_percent_corrected:1") != std::string::npos;
    });
    require(corrected_load_dom, "the corrected second pass must load through load_dom");

    const auto any_serialize = std::any_of(events.begin(), events.end(), [](const pagecore::PerfEvent& e) {
        return e.phase == pagecore::PerfPhase::SerializeHtml || e.name == "load_html";
    });
    require(!any_serialize, "the direct-DOM path must never serialize the document for layout");
}

class EngineBuildCountingFactory final : public pagecore::LayoutEngineFactory {
public:
    explicit EngineBuildCountingFactory(std::shared_ptr<pagecore::LayoutEngineFactory> inner)
        : builds(std::make_shared<int>(0))
        , inner_(std::move(inner))
    {
    }

    std::unique_ptr<pagecore::LayoutEngine> create_layout_engine() override
    {
        ++(*builds);
        return inner_->create_layout_engine();
    }

    std::shared_ptr<int> builds;

private:
    std::shared_ptr<pagecore::LayoutEngineFactory> inner_;
};

void test_page_direct_dom_memoization()
{
    auto counting = std::make_shared<EngineBuildCountingFactory>(pagecore::create_litehtml_layout_engine_factory());

    pagecore::LoadOptions load_options;
    load_options.layout_tree_input = pagecore::LayoutTreeInput::DirectDom;
    pagecore::Page page(load_options);
    page.set_layout_engine_factory(counting);
    page.load_html(
        "<html><body><div id='x' style='width:50px;height:20px'>hi</div></body></html>",
        "https://example.test/");

    pagecore::RenderOptions options;
    options.viewport = pagecore::Viewport{320, 240, 1.0f};

    (void) page.display_list(options);
    require(*counting->builds == 1, "the first direct-DOM render must build one styled document");

    (void) page.display_list(options);
    require(*counting->builds == 1, "an unchanged page must reuse the memoized styled document");

    options.viewport = pagecore::Viewport{640, 240, 1.0f};
    (void) page.display_list(options);
    require(*counting->builds == 2, "a viewport change must rebuild the styled document");

    // A structural mutation (not a bare inline-style change, which Phase 8 patches
    // in place) must rebuild the styled document.
    page.document().append_child(
        page.document().get_element_by_id("x"), page.document().create_element("span"));
    (void) page.display_list(options);
    require(*counting->builds == 3, "a structural DOM mutation must rebuild the styled document");
}

void test_page_direct_dom_fallback_emits_no_spurious_load_dom_event()
{
    // SlowGeometryEngine does not override load_dom, so under the DirectDom
    // default the load falls back to load_html. Only the load_html event must
    // be emitted — not a spurious zero-work load_dom event.
    auto factory = std::make_shared<SlowGeometryFactory>();
    pagecore::Page page;  // DirectDom default
    page.set_layout_engine_factory(factory);
    page.load_html("<html><body><div id='x'>hi</div></body></html>", "https://example.test/");

    std::vector<pagecore::PerfEvent> events;
    pagecore::RenderOptions options;
    options.viewport = pagecore::Viewport{320, 240, 1.0f};
    options.perf_trace = [&](const pagecore::PerfEvent& e) { events.push_back(e); };
    (void) page.display_list(options);

    const bool emitted_load_dom = std::any_of(events.begin(), events.end(), [](const pagecore::PerfEvent& e) {
        return e.name == "load_dom";
    });
    const bool emitted_load_html = std::any_of(events.begin(), events.end(), [](const pagecore::PerfEvent& e) {
        return e.name == "load_html";
    });
    require(!emitted_load_dom, "an engine without direct-DOM support must not emit a spurious load_dom event");
    require(emitted_load_html, "the serialized fallback must emit a load_html event");
}

void test_page_direct_dom_geometry_and_computed_style_equivalence()
{
    const char* html = R"HTML(
<html><head><style>#probe{width:120px;height:44px;padding:6px;margin:10px;color:rgb(10,110,210)}</style></head>
<body><div id="probe">probe</div>
<script>
  var probe = document.getElementById('probe');
  probe.style.borderLeft = '3px solid rgb(0,0,0)';
  probe.style.fontSize = '19px';
</script>
</body></html>
)HTML";

    pagecore::Page serialized_page;
    serialized_page.load_html(html, "https://example.test/");

    pagecore::LoadOptions direct_options;
    direct_options.layout_tree_input = pagecore::LayoutTreeInput::DirectDom;
    pagecore::Page direct_page(direct_options);
    direct_page.load_html(html, "https://example.test/");

    pagecore::RenderOptions options;
    options.viewport = pagecore::Viewport{500, 300, 1.0f};
    (void) serialized_page.display_list(options);
    (void) direct_page.display_list(options);

    const pagecore::NodeId serialized_probe = serialized_page.document().get_element_by_id("probe");
    const pagecore::NodeId direct_probe = direct_page.document().get_element_by_id("probe");
    require(serialized_probe != pagecore::kInvalidNodeId && direct_probe != pagecore::kInvalidNodeId,
            "both pages must resolve #probe");

    for (const char* property : {"width", "height", "font-size", "color", "padding-left", "display"}) {
        const auto serialized_value = serialized_page.computed_style_property(serialized_probe, property);
        const auto direct_value = direct_page.computed_style_property(direct_probe, property);
        require(serialized_value == direct_value,
                std::string("computed ") + property + " must match across layout tree inputs");
    }

    const auto serialized_geometry = serialized_page.element_geometry(serialized_probe);
    const auto direct_geometry = direct_page.element_geometry(direct_probe);
    require(serialized_geometry.has_value() && direct_geometry.has_value(),
            "both pages must resolve #probe geometry");
    require(serialized_geometry->border_box.width == direct_geometry->border_box.width
                && serialized_geometry->border_box.height == direct_geometry->border_box.height
                && serialized_geometry->border_box.x == direct_geometry->border_box.x
                && serialized_geometry->border_box.y == direct_geometry->border_box.y,
            "border boxes must match across layout tree inputs");
}

// Build+layout counting harness: counts styled-document builds (engine
// creations) and layout() calls sharing one litehtml factory underneath.
struct PatchCounters {
    std::shared_ptr<EngineBuildCountingFactory> factory;
    std::shared_ptr<int> builds;
    std::shared_ptr<int> layouts;
};

PatchCounters make_patch_counting_factory()
{
    auto layout_counting =
        std::make_shared<LayoutCallCountingFactory>(pagecore::create_litehtml_layout_engine_factory());
    auto build_counting = std::make_shared<EngineBuildCountingFactory>(layout_counting);
    return PatchCounters{build_counting, build_counting->builds, layout_counting->layout_calls};
}

void test_page_inline_style_patch_avoids_full_rebuild()
{
    auto counters = make_patch_counting_factory();

    pagecore::Page page;  // DirectDom by default
    page.set_layout_engine_factory(counters.factory);
    page.load_html(
        "<html><body><div id='x' style='width:50px;height:20px;background:rgb(9,9,9)'>x</div></body></html>",
        "https://example.test/");

    pagecore::RenderOptions options;
    options.viewport = pagecore::Viewport{320, 240, 1.0f};

    (void) page.display_list(options);
    require(*counters.builds == 1, "the first render builds the styled document once");
    const int layouts_after_first = *counters.layouts;

    std::vector<pagecore::PerfEvent> events;
    options.perf_trace = [&](const pagecore::PerfEvent& event) { events.push_back(event); };

    page.document().set_attribute(
        page.document().get_element_by_id("x"), "style", "width:120px;height:20px;background:rgb(9,9,9)");
    const auto dl = page.display_list(options);

    require(*counters.builds == 1, "an inline-style change must be patched in place, not rebuilt");
    require(*counters.layouts > layouts_after_first, "the patched document must be laid out again");

    const auto json = pagecore::display_list_to_json(dl);
    require(json.find("\"width\":120") != std::string::npos,
            "the display list must reflect the patched width");

    const auto patched_event = std::any_of(events.begin(), events.end(), [](const pagecore::PerfEvent& e) {
        return e.name == "patch_inline_styles" && e.property == "outcome:patched";
    });
    require(patched_event, "the perf trace must record the patched outcome");
    const auto reloaded = std::any_of(events.begin(), events.end(), [](const pagecore::PerfEvent& e) {
        return e.name == "load_dom" || e.name == "load_html";
    });
    require(!reloaded, "a patched render must not reload the document");
}

void test_page_inline_style_patch_display_list_matches_fresh_page()
{
    const char* html =
        "<html><head><style>#x{color:rgb(0,0,0)}</style></head><body>"
        "<div id='x' style='width:50px;height:20px;background:rgb(9,120,9)'>"
        "<span id='c'>child</span></div>"
        "<div id='y' style='width:30px;height:15px;background:rgb(120,9,9)'>y</div>"
        "</body></html>";
    const std::string url = "https://example.test/";
    pagecore::RenderOptions options;
    options.viewport = pagecore::Viewport{400, 300, 1.0f};

    // Patched: render, mutate two inline styles, render again (patched).
    pagecore::Page patched;
    patched.load_html(html, url);
    (void) patched.display_list(options);
    patched.document().set_attribute(
        patched.document().get_element_by_id("x"), "style", "width:180px;height:44px;background:rgb(9,120,9)");
    patched.document().set_attribute(
        patched.document().get_element_by_id("y"), "style", "width:60px;height:15px;background:rgb(120,9,9)");
    const auto patched_json = pagecore::display_list_to_json(patched.display_list(options));

    // Fresh: apply the same mutations before the first render, forcing a build.
    pagecore::Page fresh;
    fresh.load_html(html, url);
    fresh.document().set_attribute(
        fresh.document().get_element_by_id("x"), "style", "width:180px;height:44px;background:rgb(9,120,9)");
    fresh.document().set_attribute(
        fresh.document().get_element_by_id("y"), "style", "width:60px;height:15px;background:rgb(120,9,9)");
    const auto fresh_json = pagecore::display_list_to_json(fresh.display_list(options));

    require(patched_json == fresh_json,
            "a patched page's display list must match a freshly built page on the same DOM byte-for-byte");
}

void test_page_inline_style_patch_gating_fallbacks()
{
    pagecore::RenderOptions options;
    options.viewport = pagecore::Viewport{400, 300, 1.0f};

    auto expect_rebuild = [&](const std::string& fixture,
                              const std::function<void(pagecore::Page&)>& mutate,
                              const char* why) {
        auto counters = make_patch_counting_factory();
        pagecore::Page page;
        page.set_layout_engine_factory(counters.factory);
        page.load_html(fixture, "https://example.test/");
        (void) page.display_list(options);
        require(*counters.builds == 1, "first render builds once");
        mutate(page);
        (void) page.display_list(options);
        require(*counters.builds == 2, why);
    };

    // Structure-affecting inline property (display) must rebuild.
    expect_rebuild(
        "<html><body><div id='x' style='width:20px;height:20px'>x</div></body></html>",
        [](pagecore::Page& page) {
            page.document().set_attribute(page.document().get_element_by_id("x"), "style", "display:none");
        },
        "a display change must fall back to a full rebuild");

    // Structural mutation (journal has a non-inline-style record) must rebuild.
    expect_rebuild(
        "<html><body><div id='x' style='width:20px;height:20px'>x</div></body></html>",
        [](pagecore::Page& page) {
            page.document().append_child(
                page.document().get_element_by_id("x"), page.document().create_element("span"));
        },
        "a structural mutation must fall back to a full rebuild");

    // A [style] attribute selector makes inline-style changes affect matching.
    expect_rebuild(
        "<html><head><style>div[style*='w']{outline:1px solid rgb(0,0,0)}</style></head>"
        "<body><div id='x' style='width:20px;height:20px'>x</div></body></html>",
        [](pagecore::Page& page) {
            page.document().set_attribute(
                page.document().get_element_by_id("x"), "style", "width:40px;height:20px");
        },
        "a [style] selector must force a rebuild on inline-style change");

    // Overflowing the journal past its cap loses completeness -> rebuild.
    expect_rebuild(
        "<html><body><div id='x' style='width:0px;height:20px'>x</div></body></html>",
        [](pagecore::Page& page) {
            const pagecore::NodeId x = page.document().get_element_by_id("x");
            for (int i = 0; i < 65; ++i) {
                page.document().set_attribute(x, "style", "width:" + std::to_string(i) + "px;height:20px");
            }
        },
        "an overflowed mutation journal must force a rebuild");

    // A mixed style+class batch has a non-inline-style record -> rebuild.
    expect_rebuild(
        "<html><head><style>.big{height:40px}</style></head>"
        "<body><div id='x' style='width:20px;height:20px'>x</div></body></html>",
        [](pagecore::Page& page) {
            page.document().set_attribute(
                page.document().get_element_by_id("x"), "style", "width:40px;height:20px");
            page.document().set_attribute(page.document().get_element_by_id("x"), "class", "big");
        },
        "a mixed style+class mutation must force a rebuild");
}

void test_page_inline_style_patch_computed_style_and_geometry()
{
    const char* html =
        "<html><body><div id='x' style='width:50px;height:20px;padding:4px;color:rgb(0,0,0)'>x</div></body></html>";
    const std::string url = "https://example.test/";
    pagecore::RenderOptions options;
    options.viewport = pagecore::Viewport{400, 300, 1.0f};

    pagecore::Page patched;
    patched.load_html(html, url);
    (void) patched.display_list(options);
    patched.document().set_attribute(
        patched.document().get_element_by_id("x"), "style", "width:130px;height:60px;padding:4px;color:rgb(10,20,30)");
    (void) patched.display_list(options);

    pagecore::Page fresh;
    fresh.load_html(html, url);
    fresh.document().set_attribute(
        fresh.document().get_element_by_id("x"), "style", "width:130px;height:60px;padding:4px;color:rgb(10,20,30)");
    (void) fresh.display_list(options);

    const pagecore::NodeId patched_x = patched.document().get_element_by_id("x");
    const pagecore::NodeId fresh_x = fresh.document().get_element_by_id("x");

    for (const char* property : {"width", "height", "color", "padding-left"}) {
        require(patched.computed_style_property(patched_x, property)
                    == fresh.computed_style_property(fresh_x, property),
                std::string("patched computed ") + property + " must match a fresh build");
    }

    const auto patched_geometry = patched.element_geometry(patched_x);
    const auto fresh_geometry = fresh.element_geometry(fresh_x);
    require(patched_geometry.has_value() && fresh_geometry.has_value(), "both pages must resolve geometry");
    require(patched_geometry->border_box.width == fresh_geometry->border_box.width
                && patched_geometry->border_box.height == fresh_geometry->border_box.height,
            "patched geometry must match a fresh build");
}

void test_page_inline_style_patch_then_absolute_percent_second_pass()
{
    const char* html = R"HTML(
<html><head><style>
  body { margin:0; }
  .wrap { position:relative; width:400px; height:100px; }
  .cell { position:absolute; left:0; top:0; width:50%; box-sizing:border-box; }
  .card { width:100%; height:40px; background:#fff; }
</style></head><body>
  <div class="wrap"><div class="cell" id="cell"><div class="card">c</div></div></div>
  <div id="probe" style="width:20px;height:10px;background:rgb(3,3,3)">p</div>
</body></html>
)HTML";
    const std::string url = "https://example.test/";
    pagecore::RenderOptions options;
    options.viewport = pagecore::Viewport{800, 200, 1.0f};

    pagecore::Page patched;
    patched.load_html(html, url);
    (void) patched.display_list(options);
    patched.document().set_attribute(
        patched.document().get_element_by_id("probe"), "style", "width:70px;height:24px;background:rgb(3,3,3)");
    const auto patched_json = pagecore::display_list_to_json(patched.display_list(options));

    pagecore::Page fresh;
    fresh.load_html(html, url);
    fresh.document().set_attribute(
        fresh.document().get_element_by_id("probe"), "style", "width:70px;height:24px;background:rgb(3,3,3)");
    const auto fresh_json = pagecore::display_list_to_json(fresh.display_list(options));

    require(patched_json == fresh_json,
            "a patched page that still runs the abs-% second pass must match a fresh build byte-for-byte");
}

void test_page_inline_style_patch_serialized_mode_never_patches()
{
    auto counters = make_patch_counting_factory();

    pagecore::LoadOptions load_options;
    load_options.layout_tree_input = pagecore::LayoutTreeInput::SerializedHtml;
    pagecore::Page page(load_options);
    page.set_layout_engine_factory(counters.factory);
    page.load_html(
        "<html><body><div id='x' style='width:50px;height:20px'>x</div></body></html>",
        "https://example.test/");

    pagecore::RenderOptions options;
    options.viewport = pagecore::Viewport{320, 240, 1.0f};

    (void) page.display_list(options);
    require(*counters.builds == 1, "first render builds once");

    page.document().set_attribute(page.document().get_element_by_id("x"), "style", "width:120px;height:20px");
    (void) page.display_list(options);
    require(*counters.builds == 2,
            "the serialized layout input must always rebuild, never patch");
}

// Documented divergence of the direct-DOM path: DOM anomalies JavaScript can
// create but an HTML parser would normalize away (a <p> nested inside another
// <p>) now reach the layout engine as the real tree — the behavior of real
// browsers — instead of being flattened by the serialize/re-parse round trip.
void test_page_direct_dom_preserves_js_created_nesting()
{
    const char* html = R"HTML(
<html><body>
<p id="outer" style="padding-left:30px">outer</p>
<script>
  var inner = document.createElement('p');
  inner.id = 'inner';
  inner.textContent = 'inner';
  document.getElementById('outer').appendChild(inner);
</script>
</body></html>
)HTML";

    pagecore::RenderOptions options;
    options.viewport = pagecore::Viewport{400, 300, 1.0f};

    pagecore::Page direct_page;  // DirectDom is the default
    direct_page.load_html(html, "https://example.test/");
    (void) direct_page.display_list(options);

    const pagecore::NodeId direct_outer = direct_page.document().get_element_by_id("outer");
    const pagecore::NodeId direct_inner = direct_page.document().get_element_by_id("inner");
    const auto direct_outer_geometry = direct_page.element_geometry(direct_outer);
    const auto direct_inner_geometry = direct_page.element_geometry(direct_inner);
    require(direct_outer_geometry.has_value() && direct_inner_geometry.has_value(),
            "both paragraphs must lay out in the direct path");
    require(direct_inner_geometry->border_box.x
                == direct_outer_geometry->border_box.x + 30.0f,
            "the JS-nested <p> must lay out inside its real parent (inheriting its padding offset)");

    // The serialized round trip re-parses the markup, so the HTML parser
    // splits the nested <p> out to a sibling with no padding offset.
    pagecore::LoadOptions serialized_options;
    serialized_options.layout_tree_input = pagecore::LayoutTreeInput::SerializedHtml;
    pagecore::Page serialized_page(serialized_options);
    serialized_page.load_html(html, "https://example.test/");
    (void) serialized_page.display_list(options);

    const pagecore::NodeId serialized_outer = serialized_page.document().get_element_by_id("outer");
    const pagecore::NodeId serialized_inner = serialized_page.document().get_element_by_id("inner");
    const auto serialized_outer_geometry = serialized_page.element_geometry(serialized_outer);
    const auto serialized_inner_geometry = serialized_page.element_geometry(serialized_inner);
    require(serialized_outer_geometry.has_value() && serialized_inner_geometry.has_value(),
            "both paragraphs must lay out in the serialized path");
    require(serialized_inner_geometry->border_box.x == serialized_outer_geometry->border_box.x,
            "the serialized round trip flattens the JS-created nesting (documented divergence)");
}

// Permanent A/B regression hook: the serialized layout input stays available
// behind LoadOptions and keeps its serialize+load_html perf signature.
void test_page_serialized_layout_input_still_available()
{
    pagecore::LoadOptions load_options;
    load_options.layout_tree_input = pagecore::LayoutTreeInput::SerializedHtml;

    std::vector<pagecore::PerfEvent> events;
    pagecore::Page page(load_options);
    page.load_html(
        "<html><body><div style='width:50px;height:20px;background:rgb(9,9,9)'>x</div></body></html>",
        "https://example.test/");

    pagecore::RenderOptions options;
    options.viewport = pagecore::Viewport{320, 240, 1.0f};
    options.perf_trace = [&](const pagecore::PerfEvent& event) { events.push_back(event); };

    const auto display_list = page.display_list(options);
    require(!display_list.commands.empty(), "the serialized path must still render");

    const auto serialized = std::any_of(events.begin(), events.end(), [](const pagecore::PerfEvent& e) {
        return e.phase == pagecore::PerfPhase::SerializeHtml;
    });
    const auto loaded_html = std::any_of(events.begin(), events.end(), [](const pagecore::PerfEvent& e) {
        return e.phase == pagecore::PerfPhase::LitehtmlLoadHtml && e.name == "load_html";
    });
    const auto loaded_dom = std::any_of(events.begin(), events.end(), [](const pagecore::PerfEvent& e) {
        return e.name == "load_dom";
    });
    require(serialized, "the serialized path must trace HTML serialization");
    require(loaded_html, "the serialized path must trace load_html");
    require(!loaded_dom, "the serialized path must never call load_dom");
}

// Regression (Phase 3): the absolute-% correction is now collected by the engine
// with typed getters, but must still trigger the corrective second layout pass and
// produce a deterministic (byte-identical) display list.
void test_render_absolute_percent_correction_runs_second_pass()
{
    const char* html = R"HTML(
<html><head><style>
  body { margin:0; }
  .wrap { position:relative; width:400px; height:100px; }
  .cell { position:absolute; left:0; top:0; width:50%; box-sizing:border-box; }
  .card { width:100%; height:40px; background:#fff; }
</style></head><body>
  <div class="wrap"><div class="cell" id="cell"><div class="card" id="card"></div></div></div>
</body></html>
)HTML";

    std::vector<pagecore::PerfEvent> events;
    pagecore::Page page;
    page.load_html(html, "https://example.test/index.html");

    pagecore::RenderOptions options;
    options.viewport = pagecore::Viewport{800, 200, 1.0f};
    options.perf_trace = [&](const pagecore::PerfEvent& event) {
        events.push_back(event);
    };

    const std::string json_first = pagecore::display_list_to_json(page.display_list(options));

    // Both layout tree inputs tag the document-load event with the pass kind:
    // SerializeHtml for the serialized path, LitehtmlLoadHtml/load_dom for the
    // direct-DOM path.
    auto has_pass = [&](std::string_view marker) {
        return std::any_of(events.begin(), events.end(), [&](const pagecore::PerfEvent& e) {
            return (e.phase == pagecore::PerfPhase::SerializeHtml
                    || e.phase == pagecore::PerfPhase::LitehtmlLoadHtml)
                && e.property.find(marker) != std::string::npos;
        });
    };
    require(has_pass("absolute_percent_corrected:1"),
            "an absolute-% border-box page must run the second, corrected layout pass");
    require(has_pass("absolute_percent_corrected:0"), "the uncorrected first pass must still run");

    // An independent render of the same page must reproduce the corrected display
    // list exactly: the typed override collection is deterministic.
    pagecore::Page page2;
    page2.load_html(html, "https://example.test/index.html");
    const std::string json_second = pagecore::display_list_to_json(page2.display_list(options));
    require(json_first == json_second,
            "the corrected absolute-% display list must be deterministic (byte-identical)");
}

// Regression (Phase 3): a page with no position:absolute; box-sizing:border-box;
// width:% elements must short-circuit — the engine reports no overrides and the
// corrective second layout pass never runs.
void test_render_without_absolute_elements_skips_second_pass()
{
    std::vector<pagecore::PerfEvent> events;
    pagecore::Page page;
    page.load_html(R"HTML(
<html><head><style>
  body { margin:0; }
  .box { width:120px; height:40px; background:#fff; }
</style></head><body>
  <div class="box"></div>
  <p>No absolutely-positioned percentage-width elements here.</p>
</body></html>
)HTML", "https://example.test/index.html");

    pagecore::RenderOptions options;
    options.viewport = pagecore::Viewport{400, 200, 1.0f};
    options.perf_trace = [&](const pagecore::PerfEvent& event) {
        events.push_back(event);
    };

    (void) page.display_list(options);

    const auto ran_second_pass = std::any_of(events.begin(), events.end(), [](const pagecore::PerfEvent& e) {
        return e.phase == pagecore::PerfPhase::SerializeHtml
            && e.property.find("absolute_percent_corrected:1") != std::string::npos;
    });
    require(!ran_second_pass,
            "a page with no absolute-% elements must not run the corrective second layout pass");
}

// Regression (ii): the final paint is a pure function of the settled DOM and the
// render viewport, independent of any prior scripting reads.
void test_render_reflects_settled_dom_regardless_of_read_history()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html><body style="margin:0">
  <div id="a" style="width:50px;height:20px;background:#fff"></div>
  <script>
    const a = document.getElementById('a');
    a.offsetWidth;              // read at the initial width
    a.style.width = '120px';    // mutate
    a.offsetWidth;              // read again, mid-script
  </script>
</body></html>
)HTML", "https://example.test/index.html");

    pagecore::RenderOptions options;
    options.viewport = pagecore::Viewport{400, 200, 1.0f};
    const float rendered = widest_white_fill(page.display_list(options));

    // Independent exact layout of the settled serialized DOM, JS disabled.
    pagecore::LoadOptions independent_options;
    independent_options.enable_js = false;
    pagecore::Page independent(independent_options);
    independent.load_html(page.serialize_html(), "https://example.test/index.html");
    const float independent_rendered = widest_white_fill(independent.display_list(options));

    require(rendered > 110.0f, "the settled width (120px) must be what gets painted, not a mid-script read");
    require(
        std::abs(rendered - independent_rendered) < 1.0f,
        "the paint must equal an independent exact layout of the settled DOM, regardless of read history");
}

// Regression (iii): a class-driven width change must invalidate the digest and
// recompute — the "loses style transformations" bug.
void test_computed_style_reacts_to_class_driven_width_change()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html><head><style>
  .narrow { width: 40px; }
  .wide { width: 200px; }
</style></head><body>
  <div id="x" class="narrow"></div>
</body></html>
)HTML", "https://example.test/index.html");

    const pagecore::NodeId x = page.document().query_selector(page.document().document_node(), "#x");
    require(x != pagecore::kInvalidNodeId, "expected #x");

    auto narrow = page.computed_style_property(x, "width");
    require(narrow && narrow->find("40") != std::string::npos,
            "class-driven width should resolve from the stylesheet");

    page.document().set_attribute(x, "class", "wide");
    auto wide = page.computed_style_property(x, "width");
    require(wide && wide->find("200") != std::string::npos,
            "a class change must invalidate the digest and recompute the width");
    require(narrow != wide, "the computed width must change with the class");
}

// document.elementFromPoint()/elementsFromPoint()/caretPositionFromPoint(): real
// hit-testing against litehtml's render tree (LiteHtmlLayoutEngine::elements_at_point,
// backed by litehtml's own get_element_by_point), replacing the old hardcoded
// null/[] stubs. Every test element is position:absolute with explicit
// left/top/width/height so hit-test coordinates are exact document-pixel math
// (PageCore has no scroll model, so document and client coordinates coincide).
void test_hit_testing()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html><body style="margin:0">
  <div id="back" style="position:absolute;left:0;top:0;width:100px;height:100px;z-index:1;background:red;"></div>
  <div id="front" style="position:absolute;left:0;top:0;width:100px;height:100px;z-index:2;background:blue;"></div>

  <div id="visibleBehind" style="position:absolute;left:0;top:150px;width:100px;height:100px;z-index:1;background:green;"></div>
  <div id="hiddenAbove" style="position:absolute;left:0;top:150px;width:100px;height:100px;z-index:2;display:none;background:black;"></div>

  <div id="visibleBehind2" style="position:absolute;left:0;top:300px;width:100px;height:100px;z-index:1;background:green;"></div>
  <div id="invisibleAbove" style="position:absolute;left:0;top:300px;width:100px;height:100px;z-index:2;visibility:hidden;background:black;"></div>

  <div id="clipper" style="position:absolute;left:0;top:450px;width:100px;height:100px;overflow:hidden;">
    <div id="clipped" style="position:absolute;left:150px;top:0;width:100px;height:100px;background:orange;"></div>
  </div>

  <div id="textbox" style="position:absolute;left:0;top:600px;width:200px;height:50px;">Hello world</div>

  <script>
    function ids(list) { return Array.from(list).map((el) => el.id); }

    window.__topId = (document.elementFromPoint(50, 50) || {}).id || '';
    window.__overlapOrder = ids(document.elementsFromPoint(50, 50)).join(',');

    window.__hiddenTop = (document.elementFromPoint(50, 200) || {}).id || '';
    window.__hiddenExcluded = !ids(document.elementsFromPoint(50, 200)).includes('hiddenAbove');

    window.__invisibleTop = (document.elementFromPoint(50, 350) || {}).id || '';
    window.__invisibleExcluded = !ids(document.elementsFromPoint(50, 350)).includes('invisibleAbove');

    window.__nothingFromPoint = document.elementFromPoint(-50, -50);
    window.__nothingElementsLength = document.elementsFromPoint(-50, -50).length;

    window.__clippedExcluded = !ids(document.elementsFromPoint(200, 500)).includes('clipped');

    const caret = document.caretPositionFromPoint(50, 620);
    window.__caretOk = !!(caret && caret.offsetNode && caret.offsetNode.nodeType === 3
      && caret.offsetNode.data === 'Hello world' && caret.offset === 0);
  </script>
</body></html>
)HTML", "https://example.test/index.html");

    require(page.eval("String(window.__topId)") == "front",
            "elementFromPoint should return the topmost element by z-index");
    const std::string overlap_order = page.eval("String(window.__overlapOrder)");
    require(
        overlap_order.find("front") != std::string::npos
            && overlap_order.find("back") != std::string::npos
            && overlap_order.find("front") < overlap_order.find("back"),
        "elementsFromPoint should list overlapping elements front-to-back, got: " + overlap_order);

    require(page.eval("String(window.__hiddenTop)") == "visibleBehind",
            "a display:none element must not be hit-testable; the visible element behind it should be returned");
    require(page.eval("String(window.__hiddenExcluded)") == "true",
            "elementsFromPoint must exclude display:none elements");

    require(page.eval("String(window.__invisibleTop)") == "visibleBehind2",
            "a visibility:hidden element must not be hit-testable; the visible element behind it should be returned");
    require(page.eval("String(window.__invisibleExcluded)") == "true",
            "elementsFromPoint must exclude visibility:hidden elements");

    require(page.eval("String(window.__nothingFromPoint)") == "null",
            "elementFromPoint should return null when nothing is under the point");
    require(page.eval("String(window.__nothingElementsLength)") == "0",
            "elementsFromPoint should return an empty list when nothing is under the point");

    require(page.eval("String(window.__clippedExcluded)") == "true",
            "elementFromPoint/elementsFromPoint must respect overflow:hidden clipping");

    require(page.eval("String(window.__caretOk)") == "true",
            "caretPositionFromPoint should resolve to the text-containing element's first Text descendant");
}

// P0.1: test_driver_internal.action_sequence() with a single pointer source
// (pointerMove over an element, pointerDown, pointerUp -- the exact shape
// used by all three in-scope test_driver.Actions() corpus files) must
// resolve the pointerDown/pointerUp target through real hit-testing
// (document.elementFromPoint(), like real WebDriver Actions do) and fire a
// click once the down and up targets match. Needs rendering, same as
// elementFromPoint/elementsFromPoint themselves.
void test_wpt_driver_action_sequence_pointer_source()
{
    pagecore::Page page;
    page.load_html(R"HTML(
<html><body style="margin:0">
  <div id="target" style="position:absolute;left:10px;top:10px;width:50px;height:50px;"></div>
  <script>
    const target = document.getElementById('target');
    window.__clicked = false;
    target.addEventListener('click', () => { window.__clicked = true; });

    test_driver_internal = {};
    const actionsByInput = [{
      type: 'pointer',
      id: '0',
      parameters: { pointerType: 'mouse' },
      actions: [
        { type: 'pointerMove', x: 0, y: 0, origin: target },
        { type: 'pointerDown', button: 0 },
        { type: 'pointerUp', button: 0 }
      ]
    }];
    test_driver_internal.action_sequence(actionsByInput).then(() => {
      window.__done = true;
    });
  </script>
</body></html>
)HTML", "https://example.test/index.html");

    require(page.eval("String(window.__done)") == "true", "test_driver_internal.action_sequence() promise should resolve");
    require(page.eval("String(window.__clicked)") == "true",
            "a pointerMove+pointerDown+pointerUp Actions sequence over an element should fire a click on it");
}
#endif

} // namespace

int main()
{
#if !defined(_WIN32)
    // The loopback test servers write to sockets the client (libcurl) may close
    // first (e.g. when it aborts an oversized-header response), which would raise
    // SIGPIPE and terminate the process. Ignore it; the send loops handle the
    // resulting EPIPE return.
    std::signal(SIGPIPE, SIG_IGN);
#endif
    try {
        test_inline_script_mutates_lexbor_dom();
        test_timers_and_events();
        test_js_console_log_callback();
        test_dom_shim_spec_regressions();
        test_unhandled_promise_rejection_is_logged();
        test_same_origin_policy_fails_closed_without_document_origin();
        test_inner_html_fragment_parsing();
        test_tree_operations_and_clone();
        test_dataset_attributes_and_cached_facades();
        test_inner_html_invalidates_stale_wrappers();
        test_wrapper_cache_prunes_only_on_forget();
        test_timer_wait_budget();
        test_zero_wait_runs_ready_tasks_but_keeps_timers_pending();
        test_run_until_idle_logs_throwing_task_hook();
        test_event_loop_ordering_contract();
        test_browser_like_web_api_shims();
        test_media_query_list_event_target_shims();
        test_navigator_user_agent_data();
        test_event_constructor_ignores_prototype_accessors();
        test_document_lifecycle_ignores_ready_state_overrides();
        test_get_computed_style_reads_display_from_stylesheets();
        test_layer_at_rule_rules_are_applied();
        test_cssom_stylesheets_rules_declarations_and_cascade();
        test_cssom_dynamic_sheets_media_disabled_and_adopted();
        test_page_computed_style_cpp_api();
        test_get_computed_style_matches_real_cascade_for_cases_js_engine_got_wrong();
        test_dom_fragment_range_serializer_and_mutation_observer();
        test_page_enforces_dom_node_budget();
        test_page_enforces_aggregate_load_deadline();
        test_text_content_mutation_observer_records_nodes();
        test_document_write_fragment_insertion();
        test_document_write_external_script_and_open_close();
        test_document_write_escaped_script_text_remains_text();
        test_comment_nodes_wrap_for_sibling_traversal();
        test_character_data_interface();
        test_pre_insertion_validity();
        test_dom_token_list_is_an_ordered_set();
        test_idl_attribute_reflection();
        test_live_collections();
        test_dom_interface_globals();
        test_url_search_params_robustness();
        test_url_hostname_idna();
        test_text_decoder_encoding_support();
        test_node_move_before();
        test_range_content_methods();
        test_dom_implementation_create_methods();
        test_detached_html_document_appendchild_and_doctype();
        test_create_comment_nodes_are_not_visible_text();
        test_event_options_bubbling_and_wpt_driver_shim();
        test_wpt_completion_callback_registration_waits_for_harness_initialization();
        test_wpt_driver_click_dispatches_real_event_sequence();
        test_wpt_driver_send_keys_dispatches_keydown_input_keyup();
        test_custom_elements_registry_shim();
        test_shadow_root_and_element_internals_shims();
        test_shadow_root_builds_tree_from_inner_html();
        test_shadow_dom_hides_markers_and_container_from_js();
        test_custom_elements_with_private_fields_construct_instances();
        test_external_script_via_resource_loader();
        test_current_script_and_reflected_url_attributes();
        test_module_script_imports_relative_dependencies();
        test_inline_module_uses_document_url_for_relative_imports();
        test_html_element_specific_constructors();
        test_create_element_ns_and_template_content_clone();
        test_global_event_listener_aliases_bind_to_window();
        test_document_domain_and_cookie_jar();
        test_document_location_aliases_window_location();
        test_dom_implementation_create_html_document();
        test_message_channel_and_crypto_shims();
        test_text_encoder_decoder_utf8_shims();
        test_escaped_colon_class_selector_fallback();
        test_target_pseudo_class_selector_fallback();
        test_request_response_fetch_object_shims();
        test_xhr_and_fetch_load_through_resource_loader();
        test_shared_cookie_jar_document_scripts_fetch_and_xhr();
        test_fetch_xhr_credentials_control_cookie_injection();
        test_cookie_attributes_path_domain_expires_secure_samesite();
        test_base64_codec_roundtrip_and_edge_cases();
        test_script_type_classification();
        test_css_scan_url_target();
        test_css_scan_extract_urls();
        test_css_scan_parse_percentage();
        test_css_declarations_require_tree_rebuild();
        test_css_scan_attribute_selectors();
        test_css_scan_default_computed_style();
        test_page_activity_tracker_counters_and_stability();
        test_cookie_public_suffix_and_injection_rejected();
        test_cookie_domain_rejected_on_ip_host();
        test_public_suffix_list_covers_modern_hosting_platforms();
        test_cookie_secure_not_overwritten_by_insecure();
        test_cookie_jar_growth_is_bounded();
        test_failed_external_script_does_not_abort_page();
        test_nomodule_suppresses_only_classic_scripts();
        test_scripts_inside_template_do_not_execute();
        test_page_readiness_wait_until_load_skips_timers();
        test_page_readiness_ready_waits_for_timer_fetch_and_dom_stable();
        test_page_readiness_dom_stable_runs_pending_page_tasks();
        test_page_readiness_image_and_stylesheet_load_events();
        test_js_resource_policy_block_all_keeps_parser_scripts();
        test_js_resource_policy_same_origin_blocks_cross_origin_loads();
        test_js_resource_budgets_limit_count_bytes_and_time();
        test_xhr_event_handler_exceptions_are_reported();
        test_non_javascript_script_types_are_not_executed();
        test_static_classic_async_defer_scripts_follow_spec_order();
        test_defer_scripts_run_in_order_before_dcl_and_async_holds_load();
        test_dynamic_script_insertion_executes_classic_scripts();
        test_dynamic_module_scripts_are_not_executed();
        test_resource_request_kind_and_cache();
        test_caching_resource_loader_is_lru();
        test_resource_url_resolution_normalizes_dot_segments();
        test_resource_loader_decodes_data_urls();
        test_resource_policy_errors();
        test_resource_policy_blocks_private_hosts();
        test_resource_scheme_not_allowed();
        test_resource_file_sandbox();
        test_resource_relative_file_url();
        test_resource_blocks_file_from_network_origin();
#if !defined(_WIN32)
        test_curl_loader_preserves_set_cookie_headers_across_redirects();
        test_curl_loader_rejects_oversized_response_headers();
        test_curl_loader_rejects_cross_scheme_redirect();
        test_curl_loader_sends_request_cookie_across_same_host_redirect();
        test_curl_loader_sends_user_agent_and_sanitized_referer_on_network_paths();
#endif
        test_async_loader_data_and_file_urls();
#if !defined(_WIN32)
        test_async_loader_http_transfer_and_cancel();
        test_page_fetch_truly_async_over_http();
        test_page_fetch_abort_and_teardown_with_inflight_transfer();
#endif
        test_task_queue_async_loader_wraps_blocking_loader();
        test_request_animation_frame_real_frames();
        test_animation_frame_loop_does_not_block_idle_or_ready();
        test_caching_loader_bounds_and_skips_errors();
        test_resource_load_all_returns_in_order();
        test_resource_load_all_lenient_tolerates_failures();
        test_resource_load_all_propagates_first_error();
        test_caching_loader_load_all_serves_hits_and_caches();
        test_external_scripts_load_in_document_order();
        test_native_bridge_not_exposed_to_page();
#if defined(PAGECORE_ENABLE_RENDERING)
        test_cairo_raster_backend();
        test_cairo_raster_rounded_border_uses_inner_curve();
        test_cairo_raster_rounded_border_supports_uneven_widths();
        test_cairo_pdf_writer_emits_pdf_file();
        test_viewport_culling_is_byte_identical();
        test_viewport_culling_preserves_expanded_viewport_content();
        test_write_pdf_full_page_keeps_below_fold_content();
#endif
        test_display_list_json_dump();
        test_png_encoder_rgba();
#if defined(PAGECORE_ENABLE_RENDERING)
        test_png_decoder_rgba();
        test_png_decoder_handles_16_bit_channels();
        test_jpeg_decoder_rgba();
#if PAGECORE_ENABLE_WEBP
        test_webp_decoder_rgba();
#endif
        test_jpeg_decoder_rejects_huge_dimensions();
        test_png_decoder_rejects_huge_dimensions();
        test_gif_decoder_rejects_huge_dimensions();
#if PAGECORE_ENABLE_WEBP
        test_webp_decoder_rejects_huge_dimensions();
#endif
        test_gif_decoder_rgba();
#if PAGECORE_ENABLE_SVG
        test_svg_decoder_rgba();
        test_svg_path_parser_terminates_on_malformed_input();
        test_svg_decoder_rejects_huge_dimensions();
        test_svg_decoder_renders_gradients();
        test_svg_decoder_renders_use_and_defs_reference();
        test_svg_decoder_renders_nested_clip_path();
#endif
        test_cairo_raster_handles_nonfinite_coordinates();
        test_background_tiling_is_bounded();
        test_cairo_raster_shares_decoded_image_surface();
        test_cairo_raster_opaque_image_roundtrip();
        test_cairo_raster_zero_area_background_paints_nothing();
        test_litehtml_text_width_is_deterministic();
        test_image_decoder_rejects_malformed_input();
        test_cairo_raster_and_io_error_paths();
#endif
        test_deep_dom_traversal_is_iterative();
        test_set_inner_html_forgets_old_subtree_ids();
        test_deep_clone_assigns_fresh_subtree_ids();
        test_dom_layout_mutation_version_ignores_service_attributes();
        test_dom_layout_mutation_journal_records_inline_style();
        test_dom_layout_mutation_journal_records_inline_style_from_js();
        test_dom_layout_mutation_journal_other_kinds();
        test_dom_layout_mutation_journal_ignores_non_layout_mutations();
        test_dom_layout_mutation_journal_ring_completeness();
        test_dom_is_layout_sensitive_attribute();
        test_dom_visit_layout_tree_structure_and_attributes();
        test_dom_visit_layout_tree_omits_noscript_and_head_text();
        test_dom_visit_layout_tree_merges_style_overrides();
        test_dom_visit_layout_tree_coalesces_adjacent_text_runs();
        test_dom_visit_layout_tree_skips_template_subtrees();
        test_dom_visit_layout_tree_does_not_mutate();
        test_dom_quirks_mode_from_doctype();
        test_document_compat_mode_reflects_dom_quirks_mode();
        test_layout_input_digest_superset_invariants();
        test_subtree_dirty_epoch_tracks_descendant_mutations();
        test_query_selector_cache_returns_all_and_first();
        test_described_traversal_wraps_children_correctly();
        test_child_node_list_cache_reflects_mutations();
        test_eval_api();
        test_dom_methods_report_as_native();
        test_scope_selector_support();
        test_console_error_includes_error_header();
        test_event_capture_bubble_phases();
        test_mutation_observer_old_value();
        test_js_runtime_robust_exception_paths();
        test_web_shim_crypto_url_input();
        test_streams_writable_controller_and_tee();
        test_js_exception_message_includes_source_name();
#if defined(PAGECORE_ENABLE_RENDERING)
        test_page_display_list_is_memoized();
        test_page_display_list_ignores_service_attribute_mutations();
        test_page_display_list_rebuilds_for_service_attribute_selectors();
        test_page_display_list_rebuilds_for_external_service_attribute_selectors();
        test_page_display_list_ignores_script_text_mutations();
        test_page_display_list_pipeline_with_external_css();
        test_page_display_list_hides_head_text_nodes();
        test_page_display_list_resolves_litehtml_relative_base_urls();
        test_page_display_list_resolves_relative_base_element_against_document_url();
        test_page_display_list_resolves_protocol_relative_and_host_like_css_urls();
        test_shared_cookie_jar_render_resources();
        test_render_prefetches_subresources_into_cache();
        test_perf_trace_records_render_prefetch_waves();
        test_render_resource_budget_blocks_prefetch_and_layout_misses();
        test_render_resource_cache_persists_across_rebuilds();
        test_render_prefetches_css_background_images();
        test_page_render_uses_cairo_raster_backend();
        test_tiny_tiled_background_does_not_explode();
        test_shadow_dom_paints_real_content_and_hides_light_dom();
        test_page_render_uses_web_font_formats();
        test_page_render_rejects_malformed_web_font();
        test_page_render_decodes_and_draws_png_images();
        test_page_render_decodes_and_draws_data_url_images();
        test_page_render_decodes_css_data_url_background_images();
        test_page_render_decodes_and_draws_jpeg_images();
#if PAGECORE_ENABLE_WEBP
        test_page_render_decodes_and_draws_webp_images();
#endif
        test_page_render_decodes_and_draws_gif_images();
#if PAGECORE_ENABLE_SVG
        test_page_render_decodes_and_draws_svg_images();
#endif
        test_page_render_background_image_size_position_and_repeat();
        test_page_render_linear_gradient_background();
        test_page_render_radial_gradient_background();
        test_page_render_conic_gradient_background();
        test_conic_gradient_clamps_oversized_element();
        test_conic_radial_gradient_raster_robustness();
        test_page_render_clips_background_to_border_radius();
        test_page_render_clips_background_image_to_border_radius();
        test_page_render_hides_noscript_when_javascript_is_enabled();
        test_layout_serialization_preserves_user_layout_id_attribute();
        test_visual_fixture_regression();
        test_geometry_box_model_apis_reflect_real_layout();
        test_input_client_geometry_uses_content_box_inline_axis();
        test_geometry_get_client_rects_returns_domrectlist();
        test_window_named_element_access_exposes_document_ids();
        test_root_client_geometry_uses_viewport();
        test_html_image_element_x_y_reflect_layout_position();
        test_layout_serialization_materializes_cached_absolute_width();
        test_layout_serialization_materializes_absolute_percentage_width_without_js_measure();
        test_layout_serialization_skips_stale_cached_width_after_history_rollover();
        test_layout_serialization_skips_stale_cached_absolute_width_after_parent_resize();
        test_geometry_absolute_percentage_width_resolves_against_positioned_parent();
        test_geometry_reads_load_stylesheets_but_skip_images_until_render();
        test_geometry_offset_top_left_relative_to_offset_parent();
        test_geometry_offset_parent_finds_nearest_positioned_ancestor();
        test_geometry_offset_parent_cache_invalidates_after_style_mutation();
        test_window_viewport_reflects_last_render_options();
        test_geometry_apis_return_zero_for_display_none();
        test_perf_trace_records_render_geometry_and_png_phases();
        test_perf_trace_records_document_and_initial_script_resources();
        test_computed_style_property_uses_layout_mutation_version_cache();
        test_element_geometry_reuses_cached_layout();
        test_element_geometry_bounded_mode_reuses_last_known_after_own_sizing_change();
        test_element_geometry_bounded_mode_reuses_descendant_as_approximate_after_ancestor_mutation();
        test_element_geometry_bounded_mode_does_not_analytically_patch_own_position_change();
        test_element_geometry_bounded_mode_reuses_last_known_after_own_relative_position_change();
        test_element_geometry_bounded_mode_reuses_snapshot_for_unmutated_sibling();
        test_element_geometry_expensive_document_seeds_snapshot_before_bounded();
        test_element_geometry_bounded_mode_caps_uncached_exact_layouts();
        test_element_geometry_bounded_mode_drops_disconnected_stale_geometry();
        test_element_geometry_after_heavy_append_child_runs_first_exact_layout();
        test_element_geometry_after_heavy_structural_mutation_runs_first_exact_layout();
        test_computed_style_property_bounded_mode_returns_inline_without_rebuild();
        test_computed_style_property_bounded_mode_keeps_stylesheet_display();
        test_computed_style_property_bounded_mode_resolves_element_created_after_trip();
        test_computed_style_property_after_append_runs_first_exact();
        test_computed_style_property_reuses_digest_across_unrelated_mutation();
        test_render_never_injects_cross_viewport_measured_width();
        test_render_absolute_percent_correction_pins_native_width_not_narrower();
        test_layout_engine_load_dom_matches_load_html_display_list();
        test_layout_engine_load_dom_quirks_class_matching();
        test_layout_engine_load_dom_computed_style_and_geometry();
        test_layout_engine_load_dom_collects_absolute_percent_overrides();
        test_layout_engine_inline_style_patch_matches_fresh_build();
        test_layout_engine_inline_style_patch_unknown_key_is_noop();
        test_layout_engine_inline_style_patch_inherited_properties();
        test_layout_engine_load_dom_drops_refused_style_child();
        test_layout_engine_inline_style_patch_display_none_target_refuses();
        test_attr_function_makes_data_attribute_layout_sensitive();
        test_cdata_layout_parity_direct_vs_serialized();
        test_page_direct_dom_layout_display_list_parity();
        test_page_direct_dom_noscript_and_enable_js();
        test_page_direct_dom_absolute_percent_second_pass();
        test_page_direct_dom_memoization();
        test_page_direct_dom_fallback_emits_no_spurious_load_dom_event();
        test_page_direct_dom_geometry_and_computed_style_equivalence();
        test_page_direct_dom_preserves_js_created_nesting();
        test_page_serialized_layout_input_still_available();
        test_page_inline_style_patch_avoids_full_rebuild();
        test_page_inline_style_patch_display_list_matches_fresh_page();
        test_page_inline_style_patch_gating_fallbacks();
        test_page_inline_style_patch_computed_style_and_geometry();
        test_page_inline_style_patch_then_absolute_percent_second_pass();
        test_page_inline_style_patch_serialized_mode_never_patches();
        test_render_absolute_percent_correction_runs_second_pass();
        test_render_without_absolute_elements_skips_second_pass();
        test_render_reflects_settled_dom_regardless_of_read_history();
        test_computed_style_reacts_to_class_driven_width_change();
        test_hit_testing();
        test_wpt_driver_action_sequence_pointer_source();
#endif
    } catch (const std::exception& error) {
        std::cerr << "test failed: " << error.what() << "\n";
        return EXIT_FAILURE;
    }

    std::cout << "pagecore tests passed\n";
    return EXIT_SUCCESS;
}
