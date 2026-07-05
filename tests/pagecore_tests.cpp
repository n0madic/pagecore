#include "base64_codec.hpp"
#include "css_scan.hpp"
#include "page_activity_tracker.hpp"
#include "script_type.hpp"

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
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
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
    list.setAttribute('data-order', list.children.map((node) => node.textContent).join(''));
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
    pagecore::LoadOptions options;
    options.wait_until = pagecore::WaitUntil::Load;
    options.wait_time = std::chrono::milliseconds(5);

    pagecore::Page page(options);
    page.load_html(R"HTML(
<html><body>
  <div id="timer"></div>
  <script>
    setTimeout(() => document.getElementById('timer').setAttribute('data-fired', 'yes'), 10);
  </script>
</body></html>
)HTML");

    require(!page.outer_html("#timer[data-fired='yes']").has_value(),
            "wait-until=load should not run timer callbacks during load");

    page.run_until_idle();
    require(!page.outer_html("#timer[data-fired='yes']").has_value(),
            "timer should not fire before the configured wait budget");

    page.run_until_idle();
    require(page.outer_html("#timer[data-fired='yes']").has_value(),
            "timer should fire after enough wait budget has been advanced");
}

void test_zero_wait_does_not_run_timer_callbacks()
{
    pagecore::LoadOptions options;
    options.wait_time = std::chrono::milliseconds(0);

    pagecore::Page page(options);
    page.load_html(R"HTML(
<html><body>
  <div id="timer" data-sync="no"></div>
  <script>
    document.getElementById('timer').setAttribute('data-sync', 'yes');
    setTimeout(() => document.getElementById('timer').setAttribute('data-fired', 'yes'), 0);
  </script>
</body></html>
)HTML");

    require(page.outer_html("#timer[data-sync='yes']").has_value(),
            "wait_time=0 must still execute synchronous scripts");
    require(!page.outer_html("#timer[data-fired='yes']").has_value(),
            "wait_time=0 must not run zero-delay timer callbacks during load");

    page.run_until_idle();
    require(!page.outer_html("#timer[data-fired='yes']").has_value(),
            "wait_time=0 run_until_idle must keep timer callbacks pending");
}

void test_run_until_idle_logs_throwing_event_loop_snapshot()
{
    std::vector<std::pair<std::string, std::string>> console_logs;
    pagecore::LoadOptions options;
    options.wait_until = pagecore::WaitUntil::Load;
    options.wait_time = std::chrono::milliseconds(10);
    options.console_log = [&](std::string_view severity, std::string_view message) {
        console_logs.emplace_back(severity, message);
    };

    pagecore::Page page(options);
    page.load_html("<html><body></body></html>");
    page.eval(R"JS(
      window.__pagecore_event_loop_snapshot = () => { throw new Error('snapshot boom'); };
      setTimeout(() => document.body.setAttribute('data-timer', 'ran'), 0);
    )JS");

    bool threw = false;
    try {
        page.run_until_idle();
    } catch (...) {
        threw = true;
    }

    require(!threw, "run_until_idle should log throwing event-loop snapshot hooks instead of propagating");
    require(
        std::any_of(console_logs.begin(), console_logs.end(), [](const auto& entry) {
            return entry.first == "error" && entry.second.find("snapshot boom") != std::string::npos;
        }),
        "run_until_idle should report event-loop snapshot exceptions through console_log");
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
          // Shadow DOM is a JS-only simulation here (its nodes are never
          // attached to the real lexbor tree litehtml renders from), so
          // getComputedStyle() can't resolve rule-based styles for it —
          // same known limitation as adoptedStyleSheets on the document.
          getComputedStyle(child).color === '' &&
          child.innerText === 'Shadow text' &&
          child.checkVisibility() === true &&
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
    });

    const dynamic = document.createElement('script');
    dynamic.src = '/dynamic.js';
    document.head.appendChild(dynamic);
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

void test_static_classic_async_defer_scripts_use_pagecore_dom_order()
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

    require(page.eval("window.__staticOrder") == "1AD4",
            "static classic async/defer scripts should follow PageCore DOM-order execution");
    require(
        page.outer_html("body[data-static-order-at-dcl='1AD4']").has_value(),
        "DOMContentLoaded should fire after PageCore static script execution");
    require(has_request_kind(*loader, "https://example.test/async.js", pagecore::ResourceKind::Script),
            "static async classic script should still load as a script resource");
    require(has_request_kind(*loader, "https://example.test/defer.js", pagecore::ResourceKind::Script),
            "static defer classic script should still load as a script resource");
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
        "SVG decoder should enforce the decode byte budget before allocating the cairo surface");
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

void test_dom_visit_layout_tree_includes_template_content()
{
    pagecore::DomDocument doc;
    doc.parse("<html><body><template id='t'><p>tpl text</p></template></body></html>");

    RecordingLayoutTreeVisitor visitor;
    doc.visit_layout_tree(visitor);

    const auto* template_enter = visitor.find_enter("template");
    require(template_enter != nullptr, "template element must be visited");
    require(visitor.find_enter("p") != nullptr, "template content elements must be visited as children");
    require(visitor.has_text("tpl text"), "template content text must be visited");

    // Content must be nested inside the template's enter/leave bracket.
    std::size_t template_enter_index = 0;
    std::size_t template_leave_index = 0;
    std::size_t p_enter_index = 0;
    for (std::size_t i = 0; i < visitor.events.size(); ++i) {
        const auto& event = visitor.events[i];
        if (event.kind == "enter" && event.tag == "template") {
            template_enter_index = i;
        } else if (event.kind == "leave" && event.id == template_enter->id) {
            template_leave_index = i;
        } else if (event.kind == "enter" && event.tag == "p") {
            p_enter_index = i;
        }
    }
    require(template_enter_index < p_enter_index && p_enter_index < template_leave_index,
            "template content must be walked between the template's enter and leave");
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
    require(page.eval(
                "(() => { const k = document.getElementById('host').childNodes;"
                " return k.length + ':' + k.map(n => n.nodeType).join(','); })()") == "4:3,1,8,1",
            "childNodes returns every child with the correct node types in order");

    // children must surface only the element children, with correct tags.
    require(page.eval(
                "document.getElementById('host').children.map(e => e.tagName).join(',')") == "SPAN,P",
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

    require(has_phase(pagecore::PerfPhase::SerializeHtml), "perf trace should record HTML serialization");
    require(has_phase(pagecore::PerfPhase::SubresourceScan), "perf trace should record subresource scanning");
    require(has_phase(pagecore::PerfPhase::LitehtmlLoadHtml), "perf trace should record litehtml load_html");
    require(has_phase(pagecore::PerfPhase::LitehtmlLayout), "perf trace should record litehtml layout");
    require(has_phase(pagecore::PerfPhase::ComputedStyle), "perf trace should record computed style reads");
    require(has_phase(pagecore::PerfPhase::Geometry), "perf trace should record geometry reads");
    require(has_phase(pagecore::PerfPhase::Raster), "perf trace should record rasterization");
    require(has_phase(pagecore::PerfPhase::PngEncode), "perf trace should record PNG encoding");
    require(count_for(pagecore::PerfPhase::SerializeHtml) > 0, "serialize trace count should report bytes");
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

    auto script_batch = std::find_if(events.begin(), events.end(), [](const pagecore::PerfEvent& event) {
        return event.phase == pagecore::PerfPhase::ResourceLoad && event.name == "initial_script_load_all";
    });
    require(script_batch != events.end(), "perf trace should record the initial script batch load");
    require(script_batch->property == "script", "initial script batch trace should identify script resources");
    require(script_batch->count > 0, "initial script batch trace should report loaded bytes");

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
    void layout() override
    {
        ++(*layout_calls_);
        inner_->layout();
    }
    const pagecore::DisplayList& display_list() const override { return inner_->display_list(); }
    void compute_styles_only() override { inner_->compute_styles_only(); }
    std::optional<pagecore::ComputedStyle> computed_style(std::string_view node_key) override
    {
        return inner_->computed_style(node_key);
    }
    std::optional<pagecore::ElementGeometry> element_geometry(std::string_view node_key) override
    {
        return inner_->element_geometry(node_key);
    }

private:
    std::unique_ptr<pagecore::LayoutEngine> inner_;
    std::shared_ptr<int> layout_calls_;
};

class LayoutCallCountingFactory final : public pagecore::LayoutEngineFactory {
public:
    explicit LayoutCallCountingFactory(std::shared_ptr<pagecore::LayoutEngineFactory> inner)
        : inner_(std::move(inner))
        , layout_calls(std::make_shared<int>(0))
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
        std::this_thread::sleep_for(std::chrono::milliseconds(65));
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
        std::this_thread::sleep_for(std::chrono::milliseconds(65));
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

    const auto corrected_pass = std::any_of(events.begin(), events.end(), [](const pagecore::PerfEvent& e) {
        return e.phase == pagecore::PerfPhase::SerializeHtml
            && e.property.find("absolute_percent_corrected:1") != std::string::npos;
    });
    require(corrected_pass,
            "an absolute-% border-box page must run the second, corrected layout pass");

    const auto first_pass = std::any_of(events.begin(), events.end(), [](const pagecore::PerfEvent& e) {
        return e.phase == pagecore::PerfPhase::SerializeHtml
            && e.property.find("absolute_percent_corrected:0") != std::string::npos;
    });
    require(first_pass, "the uncorrected first pass must still run");

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
#endif

} // namespace

int main()
{
    try {
        test_inline_script_mutates_lexbor_dom();
        test_timers_and_events();
        test_js_console_log_callback();
        test_inner_html_fragment_parsing();
        test_tree_operations_and_clone();
        test_dataset_attributes_and_cached_facades();
        test_inner_html_invalidates_stale_wrappers();
        test_wrapper_cache_prunes_only_on_forget();
        test_timer_wait_budget();
        test_zero_wait_does_not_run_timer_callbacks();
        test_run_until_idle_logs_throwing_event_loop_snapshot();
        test_event_loop_ordering_contract();
        test_browser_like_web_api_shims();
        test_event_constructor_ignores_prototype_accessors();
        test_document_lifecycle_ignores_ready_state_overrides();
        test_get_computed_style_reads_display_from_stylesheets();
        test_cssom_stylesheets_rules_declarations_and_cascade();
        test_cssom_dynamic_sheets_media_disabled_and_adopted();
        test_page_computed_style_cpp_api();
        test_get_computed_style_matches_real_cascade_for_cases_js_engine_got_wrong();
        test_dom_fragment_range_serializer_and_mutation_observer();
        test_text_content_mutation_observer_records_nodes();
        test_document_write_fragment_insertion();
        test_document_write_external_script_and_open_close();
        test_document_write_escaped_script_text_remains_text();
        test_comment_nodes_wrap_for_sibling_traversal();
        test_create_comment_nodes_are_not_visible_text();
        test_event_options_bubbling_and_wpt_driver_shim();
        test_custom_elements_registry_shim();
        test_shadow_root_and_element_internals_shims();
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
        test_css_scan_attribute_selectors();
        test_css_scan_default_computed_style();
        test_page_activity_tracker_counters_and_stability();
        test_cookie_public_suffix_and_injection_rejected();
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
        test_static_classic_async_defer_scripts_use_pagecore_dom_order();
        test_dynamic_script_insertion_executes_classic_scripts();
        test_dynamic_module_scripts_are_not_executed();
        test_resource_request_kind_and_cache();
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
        test_curl_loader_sends_request_cookie_across_same_host_redirect();
        test_curl_loader_sends_user_agent_and_sanitized_referer_on_network_paths();
#endif
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
#endif
        test_display_list_json_dump();
        test_png_encoder_rgba();
#if defined(PAGECORE_ENABLE_RENDERING)
        test_png_decoder_rgba();
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
#endif
        test_cairo_raster_handles_nonfinite_coordinates();
        test_background_tiling_is_bounded();
        test_cairo_raster_shares_decoded_image_surface();
        test_cairo_raster_opaque_image_roundtrip();
        test_litehtml_text_width_is_deterministic();
        test_image_decoder_rejects_malformed_input();
        test_cairo_raster_and_io_error_paths();
#endif
        test_deep_dom_traversal_is_iterative();
        test_deep_clone_assigns_fresh_subtree_ids();
        test_dom_layout_mutation_version_ignores_service_attributes();
        test_dom_visit_layout_tree_structure_and_attributes();
        test_dom_visit_layout_tree_omits_noscript_and_head_text();
        test_dom_visit_layout_tree_merges_style_overrides();
        test_dom_visit_layout_tree_coalesces_adjacent_text_runs();
        test_dom_visit_layout_tree_includes_template_content();
        test_dom_visit_layout_tree_does_not_mutate();
        test_dom_quirks_mode_from_doctype();
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
        test_page_render_uses_web_font_formats();
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
        test_page_render_clips_background_to_border_radius();
        test_page_render_clips_background_image_to_border_radius();
        test_page_render_hides_noscript_when_javascript_is_enabled();
        test_layout_serialization_preserves_user_layout_id_attribute();
        test_visual_fixture_regression();
        test_geometry_box_model_apis_reflect_real_layout();
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
        test_computed_style_property_after_append_runs_first_exact();
        test_computed_style_property_reuses_digest_across_unrelated_mutation();
        test_render_never_injects_cross_viewport_measured_width();
        test_render_absolute_percent_correction_pins_native_width_not_narrower();
        test_render_absolute_percent_correction_runs_second_pass();
        test_render_without_absolute_elements_skips_second_pass();
        test_render_reflects_settled_dom_regardless_of_read_history();
        test_computed_style_reacts_to_class_driven_width_change();
#endif
    } catch (const std::exception& error) {
        std::cerr << "test failed: " << error.what() << "\n";
        return EXIT_FAILURE;
    }

    std::cout << "pagecore tests passed\n";
    return EXIT_SUCCESS;
}
