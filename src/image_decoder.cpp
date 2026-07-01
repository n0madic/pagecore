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
#include <unordered_map>
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

struct SvgPaint {
    bool enabled = true;
    Color color{0, 0, 0, 255};
};

struct SvgStyle {
    SvgPaint fill{true, Color{0, 0, 0, 255}};
    SvgPaint stroke{false, Color{0, 0, 0, 255}};
    double stroke_width = 1.0;
    double opacity = 1.0;
    double fill_opacity = 1.0;
    double stroke_opacity = 1.0;
    cairo_line_cap_t line_cap = CAIRO_LINE_CAP_BUTT;
    cairo_line_join_t line_join = CAIRO_LINE_JOIN_MITER;
};

struct SvgViewport {
    int width = 300;
    int height = 150;
    double viewbox_x = 0.0;
    double viewbox_y = 0.0;
    double viewbox_width = 300.0;
    double viewbox_height = 150.0;
    bool has_viewbox = false;
};

struct SvgTag {
    std::string name;
    std::unordered_map<std::string, std::string> attributes;
    bool closing = false;
    bool self_closing = false;
    bool special = false;
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

std::optional<std::string> attr(const std::unordered_map<std::string, std::string>& attrs, std::string_view name)
{
    const auto found = attrs.find(std::string(name));
    if (found == attrs.end()) {
        return std::nullopt;
    }
    return found->second;
}

std::string strip_namespace(std::string value)
{
    const auto colon = value.find(':');
    if (colon != std::string::npos) {
        value.erase(0, colon + 1);
    }
    return lower_copy(value);
}

SvgTag parse_svg_tag(std::string_view markup)
{
    SvgTag tag;
    if (markup.size() < 2 || markup.front() != '<') {
        tag.special = true;
        return tag;
    }

    std::size_t begin = 1;
    std::size_t end = markup.size();
    if (end > begin && markup[end - 1] == '>') {
        --end;
    }

    std::string inside = trim_copy(markup.substr(begin, end - begin));
    if (inside.empty()) {
        tag.special = true;
        return tag;
    }
    if (inside.rfind("!--", 0) == 0 || inside.front() == '?' || inside.front() == '!') {
        tag.special = true;
        return tag;
    }
    if (inside.front() == '/') {
        tag.closing = true;
        inside = trim_copy(std::string_view(inside).substr(1));
    }
    if (!inside.empty() && inside.back() == '/') {
        tag.self_closing = true;
        inside.pop_back();
        inside = trim_copy(inside);
    }

    std::size_t pos = 0;
    while (pos < inside.size() && !std::isspace(static_cast<unsigned char>(inside[pos]))) {
        ++pos;
    }
    tag.name = strip_namespace(inside.substr(0, pos));

    while (pos < inside.size()) {
        while (pos < inside.size() && std::isspace(static_cast<unsigned char>(inside[pos]))) {
            ++pos;
        }
        if (pos >= inside.size()) {
            break;
        }

        const std::size_t name_begin = pos;
        while (pos < inside.size()
               && inside[pos] != '='
               && !std::isspace(static_cast<unsigned char>(inside[pos]))) {
            ++pos;
        }
        std::string name = strip_namespace(inside.substr(name_begin, pos - name_begin));
        while (pos < inside.size() && std::isspace(static_cast<unsigned char>(inside[pos]))) {
            ++pos;
        }

        std::string value;
        if (pos < inside.size() && inside[pos] == '=') {
            ++pos;
            while (pos < inside.size() && std::isspace(static_cast<unsigned char>(inside[pos]))) {
                ++pos;
            }
            if (pos < inside.size() && (inside[pos] == '"' || inside[pos] == '\'')) {
                const char quote = inside[pos++];
                const std::size_t value_begin = pos;
                while (pos < inside.size() && inside[pos] != quote) {
                    ++pos;
                }
                value = inside.substr(value_begin, pos - value_begin);
                if (pos < inside.size()) {
                    ++pos;
                }
            } else {
                const std::size_t value_begin = pos;
                while (pos < inside.size() && !std::isspace(static_cast<unsigned char>(inside[pos]))) {
                    ++pos;
                }
                value = inside.substr(value_begin, pos - value_begin);
            }
        }

        if (!name.empty()) {
            tag.attributes[std::move(name)] = std::move(value);
        }
    }

    return tag;
}

std::optional<Color> named_color(std::string_view lower)
{
    if (lower == "black" || lower == "currentcolor") return Color{0, 0, 0, 255};
    if (lower == "white") return Color{255, 255, 255, 255};
    if (lower == "red") return Color{255, 0, 0, 255};
    if (lower == "green") return Color{0, 128, 0, 255};
    if (lower == "blue") return Color{0, 0, 255, 255};
    if (lower == "yellow") return Color{255, 255, 0, 255};
    if (lower == "gray" || lower == "grey") return Color{128, 128, 128, 255};
    if (lower == "silver") return Color{192, 192, 192, 255};
    if (lower == "maroon") return Color{128, 0, 0, 255};
    if (lower == "purple") return Color{128, 0, 128, 255};
    if (lower == "orange") return Color{255, 165, 0, 255};
    if (lower == "transparent") return Color{0, 0, 0, 0};
    return std::nullopt;
}

int hex_value(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

std::optional<Color> parse_color(std::string_view value)
{
    std::string text = trim_copy(value);
    if (text.empty()) {
        return std::nullopt;
    }

    const std::string lower = lower_copy(text);
    if (lower == "none") {
        return std::nullopt;
    }
    if (auto named = named_color(lower)) {
        return named;
    }

    if (text.front() == '#') {
        text.erase(text.begin());
        if (text.size() == 3 || text.size() == 4) {
            Color color;
            const int r = hex_value(text[0]);
            const int g = hex_value(text[1]);
            const int b = hex_value(text[2]);
            const int a = text.size() == 4 ? hex_value(text[3]) : 15;
            if (r < 0 || g < 0 || b < 0 || a < 0) {
                return std::nullopt;
            }
            color.r = static_cast<std::uint8_t>(r * 17);
            color.g = static_cast<std::uint8_t>(g * 17);
            color.b = static_cast<std::uint8_t>(b * 17);
            color.a = static_cast<std::uint8_t>(a * 17);
            return color;
        }
        if (text.size() == 6 || text.size() == 8) {
            Color color;
            const int r0 = hex_value(text[0]);
            const int r1 = hex_value(text[1]);
            const int g0 = hex_value(text[2]);
            const int g1 = hex_value(text[3]);
            const int b0 = hex_value(text[4]);
            const int b1 = hex_value(text[5]);
            const int a0 = text.size() == 8 ? hex_value(text[6]) : 15;
            const int a1 = text.size() == 8 ? hex_value(text[7]) : 15;
            if (r0 < 0 || r1 < 0 || g0 < 0 || g1 < 0 || b0 < 0 || b1 < 0 || a0 < 0 || a1 < 0) {
                return std::nullopt;
            }
            color.r = static_cast<std::uint8_t>(r0 * 16 + r1);
            color.g = static_cast<std::uint8_t>(g0 * 16 + g1);
            color.b = static_cast<std::uint8_t>(b0 * 16 + b1);
            color.a = static_cast<std::uint8_t>(a0 * 16 + a1);
            return color;
        }
    }

    const auto open = lower.find('(');
    const auto close = lower.rfind(')');
    if (open != std::string::npos && close != std::string::npos && close > open) {
        const std::string function_name = lower.substr(0, open);
        if (function_name == "rgb" || function_name == "rgba") {
            const auto numbers = parse_number_list(std::string_view(text).substr(open + 1, close - open - 1));
            if (numbers.size() >= 3) {
                const auto clamp_byte = [](double number) {
                    return static_cast<std::uint8_t>(std::clamp(static_cast<int>(std::round(number)), 0, 255));
                };
                Color color{
                    clamp_byte(numbers[0]),
                    clamp_byte(numbers[1]),
                    clamp_byte(numbers[2]),
                    255,
                };
                if (numbers.size() >= 4) {
                    color.a = numbers[3] <= 1.0
                        ? static_cast<std::uint8_t>(std::clamp(static_cast<int>(std::round(numbers[3] * 255.0)), 0, 255))
                        : clamp_byte(numbers[3]);
                }
                return color;
            }
        }
    }

    return std::nullopt;
}

Color with_opacity(Color color, double opacity)
{
    color.a = static_cast<std::uint8_t>(
        std::clamp(static_cast<int>(std::round(static_cast<double>(color.a) * std::clamp(opacity, 0.0, 1.0))), 0, 255));
    return color;
}

void apply_style_property(SvgStyle& style, std::string_view raw_name, std::string_view raw_value)
{
    const std::string name = lower_copy(trim_copy(raw_name));
    const std::string value = trim_copy(raw_value);
    if (name == "fill") {
        if (lower_copy(value) == "none") {
            style.fill.enabled = false;
        } else if (auto color = parse_color(value)) {
            style.fill = SvgPaint{true, *color};
        }
    } else if (name == "stroke") {
        if (lower_copy(value) == "none") {
            style.stroke.enabled = false;
        } else if (auto color = parse_color(value)) {
            style.stroke = SvgPaint{true, *color};
        }
    } else if (name == "stroke-width") {
        style.stroke_width = std::max(0.0, parse_length(value, style.stroke_width));
    } else if (name == "opacity") {
        style.opacity = std::clamp(parse_length(value, style.opacity), 0.0, 1.0);
    } else if (name == "fill-opacity") {
        style.fill_opacity = std::clamp(parse_length(value, style.fill_opacity), 0.0, 1.0);
    } else if (name == "stroke-opacity") {
        style.stroke_opacity = std::clamp(parse_length(value, style.stroke_opacity), 0.0, 1.0);
    } else if (name == "stroke-linecap") {
        const std::string lower = lower_copy(value);
        if (lower == "round") style.line_cap = CAIRO_LINE_CAP_ROUND;
        else if (lower == "square") style.line_cap = CAIRO_LINE_CAP_SQUARE;
        else style.line_cap = CAIRO_LINE_CAP_BUTT;
    } else if (name == "stroke-linejoin") {
        const std::string lower = lower_copy(value);
        if (lower == "round") style.line_join = CAIRO_LINE_JOIN_ROUND;
        else if (lower == "bevel") style.line_join = CAIRO_LINE_JOIN_BEVEL;
        else style.line_join = CAIRO_LINE_JOIN_MITER;
    }
}

SvgStyle style_from_attributes(const SvgStyle& parent, const std::unordered_map<std::string, std::string>& attrs)
{
    SvgStyle style = parent;
    if (auto style_attr = attr(attrs, "style")) {
        std::size_t begin = 0;
        while (begin < style_attr->size()) {
            const auto end = style_attr->find(';', begin);
            const auto declaration = std::string_view(*style_attr).substr(
                begin,
                end == std::string::npos ? std::string::npos : end - begin);
            const auto colon = declaration.find(':');
            if (colon != std::string_view::npos) {
                apply_style_property(style, declaration.substr(0, colon), declaration.substr(colon + 1));
            }
            if (end == std::string::npos) {
                break;
            }
            begin = end + 1;
        }
    }

    for (const auto& [name, value] : attrs) {
        apply_style_property(style, name, value);
    }
    return style;
}

void set_cairo_color(cairo_t* cr, Color color)
{
    cairo_set_source_rgba(
        cr,
        static_cast<double>(color.r) / 255.0,
        static_cast<double>(color.g) / 255.0,
        static_cast<double>(color.b) / 255.0,
        static_cast<double>(color.a) / 255.0);
}

void paint_current_path(cairo_t* cr, const SvgStyle& style)
{
    const bool fill = style.fill.enabled && style.fill.color.a != 0;
    const bool stroke = style.stroke.enabled && style.stroke.color.a != 0 && style.stroke_width > 0.0;

    if (fill) {
        set_cairo_color(cr, with_opacity(style.fill.color, style.opacity * style.fill_opacity));
        if (stroke) {
            cairo_fill_preserve(cr);
        } else {
            cairo_fill(cr);
        }
    }
    if (stroke) {
        set_cairo_color(cr, with_opacity(style.stroke.color, style.opacity * style.stroke_opacity));
        cairo_set_line_width(cr, style.stroke_width);
        cairo_set_line_cap(cr, style.line_cap);
        cairo_set_line_join(cr, style.line_join);
        cairo_stroke(cr);
    } else if (!fill) {
        cairo_new_path(cr);
    }
}

void add_rounded_rect_path(cairo_t* cr, double x, double y, double width, double height, double rx, double ry)
{
    if (width <= 0.0 || height <= 0.0) {
        return;
    }
    rx = std::clamp(rx, 0.0, width / 2.0);
    ry = std::clamp(ry, 0.0, height / 2.0);
    if (rx <= 0.0 || ry <= 0.0) {
        cairo_rectangle(cr, x, y, width, height);
        return;
    }

    constexpr double pi = 3.14159265358979323846;
    cairo_new_sub_path(cr);
    cairo_move_to(cr, x + rx, y);
    cairo_line_to(cr, x + width - rx, y);
    cairo_save(cr);
    cairo_translate(cr, x + width - rx, y + ry);
    cairo_scale(cr, rx, ry);
    cairo_arc(cr, 0.0, 0.0, 1.0, -pi / 2.0, 0.0);
    cairo_restore(cr);
    cairo_line_to(cr, x + width, y + height - ry);
    cairo_save(cr);
    cairo_translate(cr, x + width - rx, y + height - ry);
    cairo_scale(cr, rx, ry);
    cairo_arc(cr, 0.0, 0.0, 1.0, 0.0, pi / 2.0);
    cairo_restore(cr);
    cairo_line_to(cr, x + rx, y + height);
    cairo_save(cr);
    cairo_translate(cr, x + rx, y + height - ry);
    cairo_scale(cr, rx, ry);
    cairo_arc(cr, 0.0, 0.0, 1.0, pi / 2.0, pi);
    cairo_restore(cr);
    cairo_line_to(cr, x, y + ry);
    cairo_save(cr);
    cairo_translate(cr, x + rx, y + ry);
    cairo_scale(cr, rx, ry);
    cairo_arc(cr, 0.0, 0.0, 1.0, pi, 3.0 * pi / 2.0);
    cairo_restore(cr);
    cairo_close_path(cr);
}

class SvgPathParser {
public:
    explicit SvgPathParser(std::string_view path)
        : text_(path)
        , current_(text_.c_str())
        , end_(text_.c_str() + text_.size())
    {
    }

    bool done()
    {
        skip_separators();
        return current_ >= end_;
    }

    bool next_is_command()
    {
        skip_separators();
        return current_ < end_ && std::isalpha(static_cast<unsigned char>(*current_));
    }

    char read_command()
    {
        skip_separators();
        return current_ < end_ ? *current_++ : '\0';
    }

    bool next_is_number()
    {
        skip_separators();
        if (current_ >= end_) {
            return false;
        }
        const char ch = *current_;
        return std::isdigit(static_cast<unsigned char>(ch)) || ch == '-' || ch == '+' || ch == '.';
    }

    bool read_number(double& out)
    {
        skip_separators();
        if (current_ >= end_) {
            return false;
        }
        char* parsed_end = nullptr;
        errno = 0;
        const double parsed = std::strtod(current_, &parsed_end);
        if (parsed_end == current_ || errno == ERANGE || !std::isfinite(parsed)) {
            return false;
        }
        out = parsed;
        current_ = parsed_end;
        skip_comma();
        return true;
    }

    // Consume a single byte. Used to skip an unrecognized token so the parser
    // is guaranteed to make forward progress on malformed input.
    void advance_one()
    {
        if (current_ < end_) {
            ++current_;
        }
    }

private:
    void skip_separators()
    {
        while (current_ < end_
               && (std::isspace(static_cast<unsigned char>(*current_)) || *current_ == ',')) {
            ++current_;
        }
    }

    void skip_comma()
    {
        while (current_ < end_ && std::isspace(static_cast<unsigned char>(*current_))) {
            ++current_;
        }
        if (current_ < end_ && *current_ == ',') {
            ++current_;
        }
    }

    std::string text_;
    const char* current_;
    const char* end_;
};

double vector_angle(double ux, double uy, double vx, double vy)
{
    const double dot = ux * vx + uy * vy;
    const double length = std::sqrt((ux * ux + uy * uy) * (vx * vx + vy * vy));
    if (length <= std::numeric_limits<double>::epsilon()) {
        return 0.0;
    }
    const double clamped = std::clamp(dot / length, -1.0, 1.0);
    const double angle = std::acos(clamped);
    return (ux * vy - uy * vx) < 0.0 ? -angle : angle;
}

void arc_to_path(
    cairo_t* cr,
    double x1,
    double y1,
    double rx,
    double ry,
    double x_axis_rotation,
    bool large_arc,
    bool sweep,
    double x2,
    double y2)
{
    if (rx == 0.0 || ry == 0.0 || (std::abs(x1 - x2) < 1e-9 && std::abs(y1 - y2) < 1e-9)) {
        cairo_line_to(cr, x2, y2);
        return;
    }

    constexpr double pi = 3.14159265358979323846;
    rx = std::abs(rx);
    ry = std::abs(ry);
    const double phi = x_axis_rotation * pi / 180.0;
    const double cos_phi = std::cos(phi);
    const double sin_phi = std::sin(phi);
    const double dx = (x1 - x2) / 2.0;
    const double dy = (y1 - y2) / 2.0;
    const double x1p = cos_phi * dx + sin_phi * dy;
    const double y1p = -sin_phi * dx + cos_phi * dy;

    const double lambda = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry);
    if (lambda > 1.0) {
        const double scale = std::sqrt(lambda);
        rx *= scale;
        ry *= scale;
    }

    const double rx2 = rx * rx;
    const double ry2 = ry * ry;
    const double x1p2 = x1p * x1p;
    const double y1p2 = y1p * y1p;
    const double numerator = std::max(0.0, rx2 * ry2 - rx2 * y1p2 - ry2 * x1p2);
    const double denominator = rx2 * y1p2 + ry2 * x1p2;
    const double factor = denominator <= std::numeric_limits<double>::epsilon()
        ? 0.0
        : ((large_arc == sweep ? -1.0 : 1.0) * std::sqrt(numerator / denominator));
    const double cxp = factor * (rx * y1p / ry);
    const double cyp = factor * (-ry * x1p / rx);
    const double cx = cos_phi * cxp - sin_phi * cyp + (x1 + x2) / 2.0;
    const double cy = sin_phi * cxp + cos_phi * cyp + (y1 + y2) / 2.0;

    const double ux = (x1p - cxp) / rx;
    const double uy = (y1p - cyp) / ry;
    const double vx = (-x1p - cxp) / rx;
    const double vy = (-y1p - cyp) / ry;
    const double start_angle = vector_angle(1.0, 0.0, ux, uy);
    double delta_angle = vector_angle(ux, uy, vx, vy);
    if (!sweep && delta_angle > 0.0) {
        delta_angle -= 2.0 * pi;
    } else if (sweep && delta_angle < 0.0) {
        delta_angle += 2.0 * pi;
    }

    cairo_save(cr);
    cairo_translate(cr, cx, cy);
    cairo_rotate(cr, phi);
    cairo_scale(cr, rx, ry);
    if (sweep) {
        cairo_arc(cr, 0.0, 0.0, 1.0, start_angle, start_angle + delta_angle);
    } else {
        cairo_arc_negative(cr, 0.0, 0.0, 1.0, start_angle, start_angle + delta_angle);
    }
    cairo_restore(cr);
}

void add_path_data(cairo_t* cr, std::string_view path)
{
    SvgPathParser parser(path);
    char command = '\0';
    char previous_command = '\0';
    double x = 0.0;
    double y = 0.0;
    double subpath_x = 0.0;
    double subpath_y = 0.0;
    double last_cubic_x = 0.0;
    double last_cubic_y = 0.0;
    double last_quad_x = 0.0;
    double last_quad_y = 0.0;

    auto read_pair = [&](double& out_x, double& out_y) {
        return parser.read_number(out_x) && parser.read_number(out_y);
    };

    while (!parser.done()) {
        if (parser.next_is_command()) {
            command = parser.read_command();
        } else if (command == '\0') {
            break;
        }

        const bool relative = std::islower(static_cast<unsigned char>(command)) != 0;
        const char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(command)));
        if (lower == 'z') {
            cairo_close_path(cr);
            x = subpath_x;
            y = subpath_y;
            previous_command = command;
            // Skip any unrecognized token so a stuck 'z' + junk cannot loop.
            if (!parser.done() && !parser.next_is_command() && !parser.next_is_number()) {
                parser.advance_one();
            }
            continue;
        }

        if (!parser.next_is_number()) {
            // The token is neither a number for this command nor a new command;
            // consume one byte to guarantee forward progress on malformed paths.
            if (!parser.done() && !parser.next_is_command()) {
                parser.advance_one();
            }
            previous_command = command;
            continue;
        }

        switch (lower) {
        case 'm': {
            bool first = true;
            double px = 0.0;
            double py = 0.0;
            while (read_pair(px, py)) {
                if (relative) {
                    px += x;
                    py += y;
                }
                if (first) {
                    cairo_move_to(cr, px, py);
                    subpath_x = px;
                    subpath_y = py;
                    first = false;
                } else {
                    cairo_line_to(cr, px, py);
                }
                x = px;
                y = py;
                if (!parser.next_is_number()) break;
            }
            break;
        }
        case 'l': {
            double px = 0.0;
            double py = 0.0;
            while (read_pair(px, py)) {
                if (relative) {
                    px += x;
                    py += y;
                }
                cairo_line_to(cr, px, py);
                x = px;
                y = py;
                if (!parser.next_is_number()) break;
            }
            break;
        }
        case 'h': {
            double px = 0.0;
            while (parser.read_number(px)) {
                if (relative) {
                    px += x;
                }
                cairo_line_to(cr, px, y);
                x = px;
                if (!parser.next_is_number()) break;
            }
            break;
        }
        case 'v': {
            double py = 0.0;
            while (parser.read_number(py)) {
                if (relative) {
                    py += y;
                }
                cairo_line_to(cr, x, py);
                y = py;
                if (!parser.next_is_number()) break;
            }
            break;
        }
        case 'c': {
            double x1 = 0.0;
            double y1 = 0.0;
            double x2 = 0.0;
            double y2 = 0.0;
            double x3 = 0.0;
            double y3 = 0.0;
            while (parser.read_number(x1) && parser.read_number(y1)
                   && parser.read_number(x2) && parser.read_number(y2)
                   && parser.read_number(x3) && parser.read_number(y3)) {
                if (relative) {
                    x1 += x; y1 += y;
                    x2 += x; y2 += y;
                    x3 += x; y3 += y;
                }
                cairo_curve_to(cr, x1, y1, x2, y2, x3, y3);
                last_cubic_x = x2;
                last_cubic_y = y2;
                x = x3;
                y = y3;
                if (!parser.next_is_number()) break;
            }
            break;
        }
        case 's': {
            double x2 = 0.0;
            double y2 = 0.0;
            double x3 = 0.0;
            double y3 = 0.0;
            while (parser.read_number(x2) && parser.read_number(y2)
                   && parser.read_number(x3) && parser.read_number(y3)) {
                double x1 = x;
                double y1 = y;
                const char prev = static_cast<char>(std::tolower(static_cast<unsigned char>(previous_command)));
                if (prev == 'c' || prev == 's') {
                    x1 = 2.0 * x - last_cubic_x;
                    y1 = 2.0 * y - last_cubic_y;
                }
                if (relative) {
                    x2 += x; y2 += y;
                    x3 += x; y3 += y;
                }
                cairo_curve_to(cr, x1, y1, x2, y2, x3, y3);
                last_cubic_x = x2;
                last_cubic_y = y2;
                x = x3;
                y = y3;
                if (!parser.next_is_number()) break;
            }
            break;
        }
        case 'q': {
            double qx = 0.0;
            double qy = 0.0;
            double ex = 0.0;
            double ey = 0.0;
            while (parser.read_number(qx) && parser.read_number(qy)
                   && parser.read_number(ex) && parser.read_number(ey)) {
                if (relative) {
                    qx += x; qy += y;
                    ex += x; ey += y;
                }
                cairo_curve_to(
                    cr,
                    x + (2.0 / 3.0) * (qx - x),
                    y + (2.0 / 3.0) * (qy - y),
                    ex + (2.0 / 3.0) * (qx - ex),
                    ey + (2.0 / 3.0) * (qy - ey),
                    ex,
                    ey);
                last_quad_x = qx;
                last_quad_y = qy;
                x = ex;
                y = ey;
                if (!parser.next_is_number()) break;
            }
            break;
        }
        case 't': {
            double ex = 0.0;
            double ey = 0.0;
            while (parser.read_number(ex) && parser.read_number(ey)) {
                double qx = x;
                double qy = y;
                const char prev = static_cast<char>(std::tolower(static_cast<unsigned char>(previous_command)));
                if (prev == 'q' || prev == 't') {
                    qx = 2.0 * x - last_quad_x;
                    qy = 2.0 * y - last_quad_y;
                }
                if (relative) {
                    ex += x;
                    ey += y;
                }
                cairo_curve_to(
                    cr,
                    x + (2.0 / 3.0) * (qx - x),
                    y + (2.0 / 3.0) * (qy - y),
                    ex + (2.0 / 3.0) * (qx - ex),
                    ey + (2.0 / 3.0) * (qy - ey),
                    ex,
                    ey);
                last_quad_x = qx;
                last_quad_y = qy;
                x = ex;
                y = ey;
                if (!parser.next_is_number()) break;
            }
            break;
        }
        case 'a': {
            double rx = 0.0;
            double ry = 0.0;
            double angle = 0.0;
            double large = 0.0;
            double sweep = 0.0;
            double ex = 0.0;
            double ey = 0.0;
            while (parser.read_number(rx) && parser.read_number(ry)
                   && parser.read_number(angle)
                   && parser.read_number(large) && parser.read_number(sweep)
                   && parser.read_number(ex) && parser.read_number(ey)) {
                if (relative) {
                    ex += x;
                    ey += y;
                }
                arc_to_path(cr, x, y, rx, ry, angle, large != 0.0, sweep != 0.0, ex, ey);
                x = ex;
                y = ey;
                if (!parser.next_is_number()) break;
            }
            break;
        }
        default:
            while (parser.next_is_number()) {
                double ignored = 0.0;
                if (!parser.read_number(ignored)) {
                    break;
                }
            }
            break;
        }

        previous_command = command;
    }
}

void apply_svg_transform(cairo_t* cr, std::string_view transform)
{
    std::string text(transform);
    std::size_t pos = 0;
    while (pos < text.size()) {
        while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) {
            ++pos;
        }
        const std::size_t name_begin = pos;
        while (pos < text.size() && std::isalpha(static_cast<unsigned char>(text[pos]))) {
            ++pos;
        }
        const std::string name = lower_copy(std::string_view(text).substr(name_begin, pos - name_begin));
        while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) {
            ++pos;
        }
        if (pos >= text.size() || text[pos] != '(') {
            break;
        }
        const std::size_t args_begin = ++pos;
        int depth = 1;
        while (pos < text.size() && depth > 0) {
            if (text[pos] == '(') ++depth;
            else if (text[pos] == ')') --depth;
            if (depth > 0) ++pos;
        }
        const auto numbers = parse_number_list(std::string_view(text).substr(args_begin, pos - args_begin));
        if (pos < text.size() && text[pos] == ')') {
            ++pos;
        }

        if (name == "translate" && !numbers.empty()) {
            cairo_translate(cr, numbers[0], numbers.size() >= 2 ? numbers[1] : 0.0);
        } else if (name == "scale" && !numbers.empty()) {
            cairo_scale(cr, numbers[0], numbers.size() >= 2 ? numbers[1] : numbers[0]);
        } else if (name == "rotate" && !numbers.empty()) {
            constexpr double pi = 3.14159265358979323846;
            if (numbers.size() >= 3) {
                cairo_translate(cr, numbers[1], numbers[2]);
                cairo_rotate(cr, numbers[0] * pi / 180.0);
                cairo_translate(cr, -numbers[1], -numbers[2]);
            } else {
                cairo_rotate(cr, numbers[0] * pi / 180.0);
            }
        } else if (name == "matrix" && numbers.size() >= 6) {
            cairo_matrix_t matrix{numbers[0], numbers[1], numbers[2], numbers[3], numbers[4], numbers[5]};
            cairo_transform(cr, &matrix);
        }
    }
}

void draw_svg_shape(cairo_t* cr, const SvgTag& tag, const SvgStyle& style)
{
    if (auto transform = attr(tag.attributes, "transform")) {
        cairo_save(cr);
        apply_svg_transform(cr, *transform);
    }

    const std::string& name = tag.name;
    if (name == "rect") {
        const double x = attr(tag.attributes, "x").has_value() ? parse_length(*attr(tag.attributes, "x")) : 0.0;
        const double y = attr(tag.attributes, "y").has_value() ? parse_length(*attr(tag.attributes, "y")) : 0.0;
        const double width = attr(tag.attributes, "width").has_value() ? parse_length(*attr(tag.attributes, "width")) : 0.0;
        const double height = attr(tag.attributes, "height").has_value() ? parse_length(*attr(tag.attributes, "height")) : 0.0;
        double rx = attr(tag.attributes, "rx").has_value() ? parse_length(*attr(tag.attributes, "rx")) : 0.0;
        double ry = attr(tag.attributes, "ry").has_value() ? parse_length(*attr(tag.attributes, "ry")) : 0.0;
        if (rx == 0.0) rx = ry;
        if (ry == 0.0) ry = rx;
        add_rounded_rect_path(cr, x, y, width, height, rx, ry);
        paint_current_path(cr, style);
    } else if (name == "circle") {
        const double cx = attr(tag.attributes, "cx").has_value() ? parse_length(*attr(tag.attributes, "cx")) : 0.0;
        const double cy = attr(tag.attributes, "cy").has_value() ? parse_length(*attr(tag.attributes, "cy")) : 0.0;
        const double r = attr(tag.attributes, "r").has_value() ? parse_length(*attr(tag.attributes, "r")) : 0.0;
        if (r > 0.0) {
            cairo_arc(cr, cx, cy, r, 0.0, 2.0 * 3.14159265358979323846);
            paint_current_path(cr, style);
        }
    } else if (name == "ellipse") {
        const double cx = attr(tag.attributes, "cx").has_value() ? parse_length(*attr(tag.attributes, "cx")) : 0.0;
        const double cy = attr(tag.attributes, "cy").has_value() ? parse_length(*attr(tag.attributes, "cy")) : 0.0;
        const double rx = attr(tag.attributes, "rx").has_value() ? parse_length(*attr(tag.attributes, "rx")) : 0.0;
        const double ry = attr(tag.attributes, "ry").has_value() ? parse_length(*attr(tag.attributes, "ry")) : 0.0;
        if (rx > 0.0 && ry > 0.0) {
            cairo_save(cr);
            cairo_translate(cr, cx, cy);
            cairo_scale(cr, rx, ry);
            cairo_arc(cr, 0.0, 0.0, 1.0, 0.0, 2.0 * 3.14159265358979323846);
            cairo_restore(cr);
            paint_current_path(cr, style);
        }
    } else if (name == "line") {
        const double x1 = attr(tag.attributes, "x1").has_value() ? parse_length(*attr(tag.attributes, "x1")) : 0.0;
        const double y1 = attr(tag.attributes, "y1").has_value() ? parse_length(*attr(tag.attributes, "y1")) : 0.0;
        const double x2 = attr(tag.attributes, "x2").has_value() ? parse_length(*attr(tag.attributes, "x2")) : 0.0;
        const double y2 = attr(tag.attributes, "y2").has_value() ? parse_length(*attr(tag.attributes, "y2")) : 0.0;
        cairo_move_to(cr, x1, y1);
        cairo_line_to(cr, x2, y2);
        paint_current_path(cr, style);
    } else if (name == "polyline" || name == "polygon") {
        if (auto points_attr = attr(tag.attributes, "points")) {
            const auto points = parse_number_list(*points_attr);
            if (points.size() >= 4) {
                cairo_move_to(cr, points[0], points[1]);
                for (std::size_t i = 2; i + 1 < points.size(); i += 2) {
                    cairo_line_to(cr, points[i], points[i + 1]);
                }
                if (name == "polygon") {
                    cairo_close_path(cr);
                }
                paint_current_path(cr, style);
            }
        }
    } else if (name == "path") {
        if (auto d = attr(tag.attributes, "d")) {
            add_path_data(cr, *d);
            paint_current_path(cr, style);
        }
    }

    if (attr(tag.attributes, "transform")) {
        cairo_restore(cr);
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

    const SvgTag root = parse_svg_tag(bytes.substr(svg_pos, tag_end - svg_pos + 1));
    if (auto viewbox = attr(root.attributes, "viewbox")) {
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
    // narrowing to int — casting an out-of-range double to int is UB.
    const auto to_dimension = [](double value) {
        if (!std::isfinite(value)) {
            return 1;
        }
        return static_cast<int>(std::round(std::clamp(value, 1.0, 8192.0)));
    };
    if (auto width = attr(root.attributes, "width")) {
        viewport.width = to_dimension(parse_length(*width, viewport.width));
    } else if (viewport.has_viewbox) {
        viewport.width = to_dimension(viewport.viewbox_width);
    }
    if (auto height = attr(root.attributes, "height")) {
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

    const SvgViewport viewport = parse_svg_viewport(bytes);
    // Enforce the decoded-image byte budget before allocating the cairo surface,
    // so a large viewport cannot force a multi-hundred-MB transient allocation
    // that the post-decode check would only reject after the fact.
    (void) checked_rgba_size(viewport.width, viewport.height, "decode SVG");
    cairo_surface_t* raw_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, viewport.width, viewport.height);
    std::unique_ptr<cairo_surface_t, decltype(&cairo_surface_destroy)> surface(raw_surface, cairo_surface_destroy);
    const cairo_status_t surface_status = cairo_surface_status(surface.get());
    if (surface_status != CAIRO_STATUS_SUCCESS) {
        throw std::runtime_error(std::string("decode SVG: ") + cairo_status_to_string(surface_status));
    }

    cairo_t* raw_cr = cairo_create(surface.get());
    std::unique_ptr<cairo_t, decltype(&cairo_destroy)> cr(raw_cr, cairo_destroy);
    const cairo_status_t cr_status = cairo_status(cr.get());
    if (cr_status != CAIRO_STATUS_SUCCESS) {
        throw std::runtime_error(std::string("decode SVG: ") + cairo_status_to_string(cr_status));
    }

    cairo_save(cr.get());
    cairo_set_operator(cr.get(), CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr.get());
    cairo_restore(cr.get());

    if (viewport.viewbox_width > 0.0 && viewport.viewbox_height > 0.0) {
        cairo_scale(
            cr.get(),
            static_cast<double>(viewport.width) / viewport.viewbox_width,
            static_cast<double>(viewport.height) / viewport.viewbox_height);
        cairo_translate(cr.get(), -viewport.viewbox_x, -viewport.viewbox_y);
    }

    std::vector<SvgStyle> style_stack;
    style_stack.push_back(SvgStyle{});
    std::vector<bool> pushed_context;

    std::size_t pos = 0;
    while (pos < bytes.size()) {
        const auto tag_begin = bytes.find('<', pos);
        if (tag_begin == std::string_view::npos) {
            break;
        }
        const auto tag_end = bytes.find('>', tag_begin);
        if (tag_end == std::string_view::npos) {
            break;
        }
        SvgTag tag = parse_svg_tag(bytes.substr(tag_begin, tag_end - tag_begin + 1));
        pos = tag_end + 1;
        if (tag.special || tag.name.empty()) {
            continue;
        }

        const bool container = tag.name == "svg" || tag.name == "g";
        if (tag.closing) {
            if (container && style_stack.size() > 1) {
                style_stack.pop_back();
                if (!pushed_context.empty()) {
                    if (pushed_context.back()) {
                        cairo_restore(cr.get());
                    }
                    pushed_context.pop_back();
                }
            }
            continue;
        }

        const SvgStyle style = style_from_attributes(style_stack.back(), tag.attributes);
        if (container) {
            if (!tag.self_closing) {
                bool saved = false;
                if (auto transform = attr(tag.attributes, "transform")) {
                    cairo_save(cr.get());
                    apply_svg_transform(cr.get(), *transform);
                    saved = true;
                }
                style_stack.push_back(style);
                pushed_context.push_back(saved);
            }
            continue;
        }

        draw_svg_shape(cr.get(), tag, style);
    }

    const cairo_status_t final_status = cairo_status(cr.get());
    if (final_status != CAIRO_STATUS_SUCCESS) {
        throw std::runtime_error(std::string("decode SVG: ") + cairo_status_to_string(final_status));
    }

    return surface_to_decoded_image(surface.get(), "decode SVG");
#else
    (void) bytes;
    throw std::runtime_error("decode SVG: SVG support is disabled");
#endif
}

} // namespace pagecore
