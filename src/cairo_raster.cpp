#include "pagecore/render.hpp"

#include <cairo.h>
#include <cairo-pdf.h>
#include <pango/pangocairo.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace pagecore {
namespace {

struct IntRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

struct ScaledRadii {
    double tl_x = 0.0;
    double tl_y = 0.0;
    double tr_x = 0.0;
    double tr_y = 0.0;
    double br_x = 0.0;
    double br_y = 0.0;
    double bl_x = 0.0;
    double bl_y = 0.0;
};

// Layout coordinates originate from CSS and may be NaN, infinite, or far
// outside the int range. Converting such a double to int is undefined behavior,
// so clamp to a finite, representable window first.
constexpr double kCoordLimit = 1.0e7;

int to_pixel_floor(double value)
{
    if (!std::isfinite(value)) {
        return 0;
    }
    return static_cast<int>(std::floor(std::clamp(value, -kCoordLimit, kCoordLimit)));
}

int to_pixel_ceil(double value)
{
    if (!std::isfinite(value)) {
        return 0;
    }
    return static_cast<int>(std::ceil(std::clamp(value, -kCoordLimit, kCoordLimit)));
}

IntRect scale_rect(Rect rect, float scale)
{
    const double s = static_cast<double>(scale);
    const int x0 = to_pixel_floor(static_cast<double>(rect.x) * s);
    const int y0 = to_pixel_floor(static_cast<double>(rect.y) * s);
    const int x1 = to_pixel_ceil((static_cast<double>(rect.x) + rect.width) * s);
    const int y1 = to_pixel_ceil((static_cast<double>(rect.y) + rect.height) * s);
    return IntRect{x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0)};
}

ScaledRadii scale_radii(BorderRadii radii, float scale)
{
    return ScaledRadii{
        std::max(0.0, static_cast<double>(radii.top_left.x) * scale),
        std::max(0.0, static_cast<double>(radii.top_left.y) * scale),
        std::max(0.0, static_cast<double>(radii.top_right.x) * scale),
        std::max(0.0, static_cast<double>(radii.top_right.y) * scale),
        std::max(0.0, static_cast<double>(radii.bottom_right.x) * scale),
        std::max(0.0, static_cast<double>(radii.bottom_right.y) * scale),
        std::max(0.0, static_cast<double>(radii.bottom_left.x) * scale),
        std::max(0.0, static_cast<double>(radii.bottom_left.y) * scale),
    };
}

bool empty(IntRect rect)
{
    return rect.width <= 0 || rect.height <= 0;
}

bool empty(ScaledRadii radii)
{
    return radii.tl_x <= 0.0 && radii.tl_y <= 0.0
        && radii.tr_x <= 0.0 && radii.tr_y <= 0.0
        && radii.br_x <= 0.0 && radii.br_y <= 0.0
        && radii.bl_x <= 0.0 && radii.bl_y <= 0.0;
}

void normalize_radii(ScaledRadii& radii, IntRect rect)
{
    radii.tl_x = std::min(radii.tl_x, static_cast<double>(rect.width) / 2.0);
    radii.tr_x = std::min(radii.tr_x, static_cast<double>(rect.width) / 2.0);
    radii.br_x = std::min(radii.br_x, static_cast<double>(rect.width) / 2.0);
    radii.bl_x = std::min(radii.bl_x, static_cast<double>(rect.width) / 2.0);
    radii.tl_y = std::min(radii.tl_y, static_cast<double>(rect.height) / 2.0);
    radii.tr_y = std::min(radii.tr_y, static_cast<double>(rect.height) / 2.0);
    radii.br_y = std::min(radii.br_y, static_cast<double>(rect.height) / 2.0);
    radii.bl_y = std::min(radii.bl_y, static_cast<double>(rect.height) / 2.0);
}

void check_status(cairo_status_t status, const char* operation)
{
    if (status != CAIRO_STATUS_SUCCESS) {
        throw std::runtime_error(std::string(operation) + ": " + cairo_status_to_string(status));
    }
}

void add_corner(cairo_t* cr, double cx, double cy, double rx, double ry, double start, double end)
{
    if (rx <= 0.0 || ry <= 0.0) {
        cairo_line_to(cr, cx, cy);
        return;
    }

    cairo_save(cr);
    cairo_translate(cr, cx, cy);
    cairo_scale(cr, rx, ry);
    cairo_arc(cr, 0.0, 0.0, 1.0, start, end);
    cairo_restore(cr);
}

void rounded_rectangle_path_scaled(cairo_t* cr, IntRect rect, ScaledRadii radii)
{
    if (empty(rect)) {
        return;
    }

    normalize_radii(radii, rect);
    if (empty(radii)) {
        cairo_rectangle(cr, rect.x, rect.y, rect.width, rect.height);
        return;
    }

    const double x = rect.x;
    const double y = rect.y;
    const double w = rect.width;
    const double h = rect.height;
    constexpr double pi = 3.14159265358979323846;

    cairo_new_sub_path(cr);
    cairo_move_to(cr, x + radii.tl_x, y);
    cairo_line_to(cr, x + w - radii.tr_x, y);
    add_corner(cr, x + w - radii.tr_x, y + radii.tr_y, radii.tr_x, radii.tr_y, -pi / 2.0, 0.0);
    cairo_line_to(cr, x + w, y + h - radii.br_y);
    add_corner(cr, x + w - radii.br_x, y + h - radii.br_y, radii.br_x, radii.br_y, 0.0, pi / 2.0);
    cairo_line_to(cr, x + radii.bl_x, y + h);
    add_corner(cr, x + radii.bl_x, y + h - radii.bl_y, radii.bl_x, radii.bl_y, pi / 2.0, pi);
    cairo_line_to(cr, x, y + radii.tl_y);
    add_corner(cr, x + radii.tl_x, y + radii.tl_y, radii.tl_x, radii.tl_y, pi, 3.0 * pi / 2.0);
    cairo_close_path(cr);
}

void rounded_rectangle_path(cairo_t* cr, IntRect rect, BorderRadii source_radii, float scale)
{
    rounded_rectangle_path_scaled(cr, rect, scale_radii(source_radii, scale));
}

void clip_rounded_rect(cairo_t* cr, IntRect rect, BorderRadii radii, float scale)
{
    if (empty(rect)) {
        return;
    }
    rounded_rectangle_path(cr, rect, radii, scale);
    cairo_clip(cr);
}

void set_source(cairo_t* cr, Color color)
{
    cairo_set_source_rgba(
        cr,
        static_cast<double>(color.r) / 255.0,
        static_cast<double>(color.g) / 255.0,
        static_cast<double>(color.b) / 255.0,
        static_cast<double>(color.a) / 255.0);
}

void fill_rect(cairo_t* cr, IntRect rect, Color color)
{
    if (empty(rect) || color.a == 0) {
        return;
    }
    set_source(cr, color);
    cairo_rectangle(cr, rect.x, rect.y, rect.width, rect.height);
    cairo_fill(cr);
}

void fill_rounded_rect(cairo_t* cr, IntRect rect, BorderRadii radii, Color color, float scale)
{
    if (empty(rect) || color.a == 0) {
        return;
    }
    set_source(cr, color);
    rounded_rectangle_path(cr, rect, radii, scale);
    cairo_fill(cr);
}

bool visible_border(const BorderSide& side)
{
    return side.width > 0.0f && side.style != BorderStyle::None && side.color.a != 0;
}

bool same_color(Color lhs, Color rhs)
{
    return lhs.r == rhs.r && lhs.g == rhs.g && lhs.b == rhs.b && lhs.a == rhs.a;
}

bool same_border_paint(const BorderSide& lhs, const BorderSide& rhs)
{
    return lhs.style == rhs.style && same_color(lhs.color, rhs.color);
}

bool has_rounded_corners(BorderRadii radii, float scale)
{
    return !empty(scale_radii(radii, scale));
}

int scaled_border_width(const BorderSide& side, float scale)
{
    if (!visible_border(side)) {
        return 0;
    }
    return std::max(1, static_cast<int>(std::round(side.width * scale)));
}

bool can_draw_single_paint_rounded_border(const BorderCommand& command, float scale)
{
    return has_rounded_corners(command.radii, scale)
        && visible_border(command.top)
        && visible_border(command.right)
        && visible_border(command.bottom)
        && visible_border(command.left)
        && same_border_paint(command.top, command.right)
        && same_border_paint(command.top, command.bottom)
        && same_border_paint(command.top, command.left);
}

ScaledRadii inner_radii_for(ScaledRadii outer, int left, int top, int right, int bottom)
{
    return ScaledRadii{
        std::max(0.0, outer.tl_x - left),
        std::max(0.0, outer.tl_y - top),
        std::max(0.0, outer.tr_x - right),
        std::max(0.0, outer.tr_y - top),
        std::max(0.0, outer.br_x - right),
        std::max(0.0, outer.br_y - bottom),
        std::max(0.0, outer.bl_x - left),
        std::max(0.0, outer.bl_y - bottom),
    };
}

void draw_single_paint_rounded_border(cairo_t* cr, const BorderCommand& command, float scale)
{
    const IntRect rect = scale_rect(command.rect, scale);
    if (empty(rect)) {
        return;
    }

    const int left = scaled_border_width(command.left, scale);
    const int top = scaled_border_width(command.top, scale);
    const int right = scaled_border_width(command.right, scale);
    const int bottom = scaled_border_width(command.bottom, scale);

    const IntRect inner{
        rect.x + left,
        rect.y + top,
        std::max(0, rect.width - left - right),
        std::max(0, rect.height - top - bottom),
    };

    ScaledRadii outer_radii = scale_radii(command.radii, scale);
    normalize_radii(outer_radii, rect);
    ScaledRadii inner_radii = inner_radii_for(outer_radii, left, top, right, bottom);

    cairo_save(cr);
    set_source(cr, command.top.color);
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
    cairo_new_path(cr);
    rounded_rectangle_path_scaled(cr, rect, outer_radii);
    if (!empty(inner)) {
        rounded_rectangle_path_scaled(cr, inner, inner_radii);
    }
    cairo_fill(cr);
    cairo_restore(cr);
}

void draw_border(cairo_t* cr, const BorderCommand& command, float scale)
{
    const IntRect rect = scale_rect(command.rect, scale);
    if (empty(rect)) {
        return;
    }

    if (can_draw_single_paint_rounded_border(command, scale)) {
        draw_single_paint_rounded_border(cr, command, scale);
        return;
    }

    cairo_save(cr);
    clip_rounded_rect(cr, rect, command.radii, scale);

    if (visible_border(command.top)) {
        fill_rect(cr, IntRect{rect.x, rect.y, rect.width, scaled_border_width(command.top, scale)}, command.top.color);
    }
    if (visible_border(command.bottom)) {
        const int height = scaled_border_width(command.bottom, scale);
        fill_rect(cr, IntRect{rect.x, rect.y + rect.height - height, rect.width, height}, command.bottom.color);
    }
    if (visible_border(command.left)) {
        fill_rect(cr, IntRect{rect.x, rect.y, scaled_border_width(command.left, scale), rect.height}, command.left.color);
    }
    if (visible_border(command.right)) {
        const int width = scaled_border_width(command.right, scale);
        fill_rect(cr, IntRect{rect.x + rect.width - width, rect.y, width, rect.height}, command.right.color);
    }

    cairo_restore(cr);
}

std::string font_family(const Font& font)
{
    return font.family.empty() ? "sans-serif" : font.family;
}

PangoWeight pango_weight(int weight)
{
    return static_cast<PangoWeight>(std::clamp(weight, 100, 1000));
}

int scaled_font_pango_size(const Font& font, float scale)
{
    return static_cast<int>(
        std::round(std::max(1.0, static_cast<double>(font.size_px) * scale) * PANGO_SCALE));
}

// Per-render-pass text resources. One Pango layout is created lazily and reused
// for every text run, and font descriptions are cached by their resolved
// properties, so a page with many text runs no longer pays a layout/description
// allocation per draw. The destructor releases everything (exception-safe).
struct TextRenderState {
    PangoLayout* layout = nullptr;
    std::unordered_map<std::string, PangoFontDescription*> descriptions;

    TextRenderState() = default;
    TextRenderState(const TextRenderState&) = delete;
    TextRenderState& operator=(const TextRenderState&) = delete;

    ~TextRenderState()
    {
        for (auto& [key, description] : descriptions) {
            pango_font_description_free(description);
        }
        if (layout != nullptr) {
            g_object_unref(layout);
        }
    }

    PangoFontDescription* description_for(const Font& font, float scale)
    {
        const int size = scaled_font_pango_size(font, scale);
        const int weight = std::clamp(font.weight, 100, 1000);
        std::string key = font_family(font);
        key += '\x1f';
        key += std::to_string(size);
        key += '\x1f';
        key += std::to_string(weight);
        key += font.italic ? "\x1fi" : "\x1fn";

        auto cached = descriptions.find(key);
        if (cached != descriptions.end()) {
            return cached->second;
        }

        PangoFontDescription* description = pango_font_description_new();
        pango_font_description_set_family(description, font_family(font).c_str());
        pango_font_description_set_absolute_size(description, size);
        pango_font_description_set_weight(description, static_cast<PangoWeight>(weight));
        pango_font_description_set_style(description, font.italic ? PANGO_STYLE_ITALIC : PANGO_STYLE_NORMAL);
        descriptions.emplace(std::move(key), description);
        return description;
    }
};

void draw_text(cairo_t* cr, const TextCommand& command, float scale, TextRenderState& text)
{
    if (command.text.empty() || command.color.a == 0) {
        return;
    }

    const IntRect rect = scale_rect(command.rect, scale);
    if (empty(rect)) {
        return;
    }

    if (text.layout == nullptr) {
        text.layout = pango_cairo_create_layout(cr);
        if (text.layout == nullptr) {
            throw std::runtime_error("failed to create Pango layout");
        }
    }
    PangoLayout* layout = text.layout;

    // pango_layout_set_font_description copies the description, so the cached
    // instance stays owned by TextRenderState.
    pango_layout_set_font_description(layout, text.description_for(command.font, scale));
    pango_layout_set_text(layout, command.text.c_str(), static_cast<int>(command.text.size()));
    pango_layout_set_width(layout, rect.width * PANGO_SCALE);
    pango_layout_set_height(layout, rect.height * PANGO_SCALE);

    set_source(cr, command.color);
    cairo_move_to(cr, rect.x, rect.y);
    pango_cairo_show_layout(cr, layout);
}

std::uint8_t unpremultiply(std::uint8_t value, std::uint8_t alpha)
{
    if (alpha == 0) {
        return 0;
    }
    return static_cast<std::uint8_t>(std::clamp((static_cast<int>(value) * 255 + alpha / 2) / alpha, 0, 255));
}

std::uint8_t premultiply(std::uint8_t value, std::uint8_t alpha)
{
    return static_cast<std::uint8_t>((static_cast<int>(value) * alpha + 127) / 255);
}

RenderedImage surface_to_rgba(cairo_surface_t* surface)
{
    cairo_surface_flush(surface);
    check_status(cairo_surface_status(surface), "flush Cairo image surface");

    RenderedImage image;
    image.width = cairo_image_surface_get_width(surface);
    image.height = cairo_image_surface_get_height(surface);
    image.rgba.resize(static_cast<std::size_t>(image.width) * image.height * 4);

    const int stride = cairo_image_surface_get_stride(surface);
    const auto* data = cairo_image_surface_get_data(surface);
    for (int y = 0; y < image.height; ++y) {
        const auto* row = data + static_cast<std::ptrdiff_t>(y) * stride;
        for (int x = 0; x < image.width; ++x) {
            std::uint32_t native_argb = 0;
            std::memcpy(&native_argb, row + static_cast<std::ptrdiff_t>(x) * 4, sizeof(native_argb));

            const auto alpha = static_cast<std::uint8_t>((native_argb >> 24) & 0xff);
            const auto red = static_cast<std::uint8_t>((native_argb >> 16) & 0xff);
            const auto green = static_cast<std::uint8_t>((native_argb >> 8) & 0xff);
            const auto blue = static_cast<std::uint8_t>(native_argb & 0xff);

            auto* pixel = &image.rgba[(static_cast<std::size_t>(y) * image.width + x) * 4];
            pixel[0] = unpremultiply(red, alpha);
            pixel[1] = unpremultiply(green, alpha);
            pixel[2] = unpremultiply(blue, alpha);
            pixel[3] = alpha;
        }
    }

    return image;
}

void draw_decoded_image(cairo_t* cr, const ImageCommand& command, float scale)
{
    if (!command.image || command.image->width <= 0 || command.image->height <= 0) {
        cairo_save(cr);
        clip_rounded_rect(cr, scale_rect(command.rect, scale), command.radii, scale);
        fill_rect(cr, scale_rect(command.tile, scale), Color{220, 220, 220, 255});
        cairo_restore(cr);
        return;
    }

    const auto& image = *command.image;
    const std::size_t expected = static_cast<std::size_t>(image.width) * image.height * 4;
    if (image.rgba.size() != expected) {
        cairo_save(cr);
        clip_rounded_rect(cr, scale_rect(command.rect, scale), command.radii, scale);
        fill_rect(cr, scale_rect(command.tile, scale), Color{220, 220, 220, 255});
        cairo_restore(cr);
        return;
    }

    const int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, image.width);
    if (stride <= 0) {
        cairo_save(cr);
        clip_rounded_rect(cr, scale_rect(command.rect, scale), command.radii, scale);
        fill_rect(cr, scale_rect(command.tile, scale), Color{220, 220, 220, 255});
        cairo_restore(cr);
        return;
    }

    std::vector<unsigned char> argb(static_cast<std::size_t>(stride) * image.height);
    for (int y = 0; y < image.height; ++y) {
        auto* row = argb.data() + static_cast<std::ptrdiff_t>(y) * stride;
        for (int x = 0; x < image.width; ++x) {
            const auto* src = &image.rgba[(static_cast<std::size_t>(y) * image.width + x) * 4];
            const auto alpha = src[3];
            const auto red = premultiply(src[0], alpha);
            const auto green = premultiply(src[1], alpha);
            const auto blue = premultiply(src[2], alpha);
            const std::uint32_t native_argb = (static_cast<std::uint32_t>(alpha) << 24)
                | (static_cast<std::uint32_t>(red) << 16)
                | (static_cast<std::uint32_t>(green) << 8)
                | static_cast<std::uint32_t>(blue);
            std::memcpy(row + static_cast<std::ptrdiff_t>(x) * 4, &native_argb, sizeof(native_argb));
        }
    }

    cairo_surface_t* raw_surface = cairo_image_surface_create_for_data(
        argb.data(),
        CAIRO_FORMAT_ARGB32,
        image.width,
        image.height,
        stride);
    check_status(cairo_surface_status(raw_surface), "create Cairo image surface for decoded image");
    std::unique_ptr<cairo_surface_t, decltype(&cairo_surface_destroy)> surface(raw_surface, cairo_surface_destroy);

    const IntRect tile = scale_rect(command.tile, scale);
    if (empty(tile)) {
        return;
    }

    cairo_save(cr);
    const IntRect bounds = scale_rect(command.rect, scale);
    if (!empty(bounds)) {
        clip_rounded_rect(cr, bounds, command.radii, scale);
    }
    const IntRect clip = scale_rect(command.clip, scale);
    if (!empty(clip)) {
        cairo_rectangle(cr, clip.x, clip.y, clip.width, clip.height);
        cairo_clip(cr);
    }

    const bool repeat_x = command.repeat == ImageRepeat::Repeat || command.repeat == ImageRepeat::RepeatX;
    const bool repeat_y = command.repeat == ImageRepeat::Repeat || command.repeat == ImageRepeat::RepeatY;
    IntRect area = empty(clip) ? bounds : clip;

    // Clamp the tiling area to the current target/clip extents so a huge
    // element box with a tiny tile cannot generate millions of off-page tiles.
    double ext_x0 = 0.0;
    double ext_y0 = 0.0;
    double ext_x1 = 0.0;
    double ext_y1 = 0.0;
    cairo_clip_extents(cr, &ext_x0, &ext_y0, &ext_x1, &ext_y1);
    if (std::isfinite(ext_x0) && std::isfinite(ext_y0) && std::isfinite(ext_x1) && std::isfinite(ext_y1)
        && ext_x1 > ext_x0 && ext_y1 > ext_y0) {
        const int ax0 = std::max(area.x, to_pixel_floor(ext_x0));
        const int ay0 = std::max(area.y, to_pixel_floor(ext_y0));
        const int ax1 = std::min(area.x + area.width, to_pixel_ceil(ext_x1));
        const int ay1 = std::min(area.y + area.height, to_pixel_ceil(ext_y1));
        area = IntRect{ax0, ay0, std::max(0, ax1 - ax0), std::max(0, ay1 - ay0)};
    }

    int start_x = tile.x;
    int start_y = tile.y;
    int count_x = 1;
    int count_y = 1;

    // Align the first tile to the tile grid at or before the area start, then
    // count exactly enough tiles to cover the area. Using 64-bit intermediates
    // and area-relative counts bounds the work to area_size / tile_size and
    // avoids the int overflow the previous incremental formula could hit.
    const auto floor_div = [](long long a, long long b) {
        long long q = a / b;
        const long long r = a % b;
        if (r != 0 && ((r < 0) != (b < 0))) {
            --q;
        }
        return q;
    };

    if (repeat_x && !empty(area) && tile.width > 0) {
        const long long first = floor_div(static_cast<long long>(area.x) - tile.x, tile.width);
        start_x = static_cast<int>(tile.x + first * tile.width);
        const long long span = static_cast<long long>(area.x) + area.width - start_x;
        count_x = static_cast<int>(std::max<long long>(1, (span + tile.width - 1) / tile.width));
    }

    if (repeat_y && !empty(area) && tile.height > 0) {
        const long long first = floor_div(static_cast<long long>(area.y) - tile.y, tile.height);
        start_y = static_cast<int>(tile.y + first * tile.height);
        const long long span = static_cast<long long>(area.y) + area.height - start_y;
        count_y = static_cast<int>(std::max<long long>(1, (span + tile.height - 1) / tile.height));
    }

    const auto draw_tile = [&](int x, int y) {
        cairo_save(cr);
        cairo_translate(cr, x, y);
        cairo_scale(
            cr,
            static_cast<double>(tile.width) / static_cast<double>(image.width),
            static_cast<double>(tile.height) / static_cast<double>(image.height));
        cairo_set_source_surface(cr, surface.get(), 0, 0);
        cairo_rectangle(cr, 0, 0, image.width, image.height);
        cairo_fill(cr);
        cairo_restore(cr);
    };

    for (int y = 0; y < count_y; ++y) {
        for (int x = 0; x < count_x; ++x) {
            draw_tile(start_x + x * tile.width, start_y + y * tile.height);
        }
    }
    cairo_restore(cr);
}

void draw_linear_gradient(cairo_t* cr, const LinearGradientCommand& command, float scale)
{
    if (command.stops.empty()) {
        return;
    }

    const IntRect bounds = scale_rect(command.rect, scale);
    if (empty(bounds)) {
        return;
    }

    // Non-finite endpoints would push the pattern (and then the context) into an
    // error state; skip the gradient instead of throwing out of the render loop.
    const double sx = static_cast<double>(command.start.x) * scale;
    const double sy = static_cast<double>(command.start.y) * scale;
    const double ex = static_cast<double>(command.end.x) * scale;
    const double ey = static_cast<double>(command.end.y) * scale;
    if (!std::isfinite(sx) || !std::isfinite(sy) || !std::isfinite(ex) || !std::isfinite(ey)) {
        return;
    }

    cairo_pattern_t* raw_pattern = cairo_pattern_create_linear(sx, sy, ex, ey);
    check_status(cairo_pattern_status(raw_pattern), "create Cairo linear gradient");
    std::unique_ptr<cairo_pattern_t, decltype(&cairo_pattern_destroy)> pattern(raw_pattern, cairo_pattern_destroy);

    for (const auto& stop : command.stops) {
        const double offset = std::clamp(static_cast<double>(stop.offset), 0.0, 1.0);
        cairo_pattern_add_color_stop_rgba(
            pattern.get(),
            offset,
            static_cast<double>(stop.color.r) / 255.0,
            static_cast<double>(stop.color.g) / 255.0,
            static_cast<double>(stop.color.b) / 255.0,
            static_cast<double>(stop.color.a) / 255.0);
    }
    check_status(cairo_pattern_status(pattern.get()), "configure Cairo linear gradient");

    cairo_save(cr);
    clip_rounded_rect(cr, bounds, command.radii, scale);
    const IntRect clip = scale_rect(command.clip, scale);
    if (!empty(clip)) {
        cairo_rectangle(cr, clip.x, clip.y, clip.width, clip.height);
        cairo_clip(cr);
    }

    cairo_set_source(cr, pattern.get());
    cairo_rectangle(cr, bounds.x, bounds.y, bounds.width, bounds.height);
    cairo_fill(cr);
    cairo_restore(cr);
}

void draw_display_list(cairo_t* cr, const DisplayList& display_list, Color background, float scale);

void draw_command(cairo_t* cr, const SolidFillCommand& command, float scale, int&, TextRenderState&)
{
    fill_rounded_rect(cr, scale_rect(command.rect, scale), command.radii, command.color, scale);
}

void draw_command(cairo_t* cr, const BorderCommand& command, float scale, int&, TextRenderState&)
{
    draw_border(cr, command, scale);
}

void draw_command(cairo_t* cr, const TextCommand& command, float scale, int&, TextRenderState& text)
{
    draw_text(cr, command, scale, text);
}

void draw_command(cairo_t* cr, const ImageCommand& command, float scale, int&, TextRenderState&)
{
    draw_decoded_image(cr, command, scale);
}

void draw_command(cairo_t* cr, const LinearGradientCommand& command, float scale, int&, TextRenderState&)
{
    draw_linear_gradient(cr, command, scale);
}

void draw_command(cairo_t* cr, const ClipCommand& command, float scale, int& clip_depth, TextRenderState&)
{
    if (command.push) {
        cairo_save(cr);
        const IntRect rect = scale_rect(command.rect, scale);
        clip_rounded_rect(cr, rect, command.radii, scale);
        ++clip_depth;
    } else if (clip_depth > 0) {
        cairo_restore(cr);
        --clip_depth;
    }
}

void draw_display_list(cairo_t* cr, const DisplayList& display_list, Color background, float scale)
{
    set_source(cr, background);
    cairo_paint(cr);

    TextRenderState text;
    int clip_depth = 0;
    for (const auto& command : display_list.commands) {
        std::visit(
            [&](const auto& typed) {
                draw_command(cr, typed, scale, clip_depth, text);
            },
            command);
        check_status(cairo_status(cr), "draw display command");
    }

    while (clip_depth > 0) {
        cairo_restore(cr);
        --clip_depth;
    }
}

class CairoRasterBackend final : public RasterBackend {
public:
    explicit CairoRasterBackend(Color background)
        : background_(background)
    {
    }

    RenderedImage render(const DisplayList& display_list) override
    {
        const float scale = std::max(0.01f, display_list.viewport.device_scale_factor);
        const int width = std::max(1, to_pixel_ceil(static_cast<double>(display_list.viewport.width) * scale));
        const int height = std::max(1, to_pixel_ceil(static_cast<double>(display_list.viewport.height) * scale));

        cairo_surface_t* raw_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
        check_status(cairo_surface_status(raw_surface), "create Cairo image surface");
        std::unique_ptr<cairo_surface_t, decltype(&cairo_surface_destroy)> surface(raw_surface, cairo_surface_destroy);

        cairo_t* raw_cr = cairo_create(surface.get());
        check_status(cairo_status(raw_cr), "create Cairo context");
        std::unique_ptr<cairo_t, decltype(&cairo_destroy)> cr(raw_cr, cairo_destroy);

        draw_display_list(cr.get(), display_list, background_, scale);

        return surface_to_rgba(surface.get());
    }

private:
    Color background_;
};

} // namespace

std::unique_ptr<RasterBackend> create_cairo_raster_backend(Color background)
{
    return std::make_unique<CairoRasterBackend>(background);
}

std::unique_ptr<RasterBackend> create_default_raster_backend(Color background)
{
    return create_cairo_raster_backend(background);
}

void write_pdf(const DisplayList& display_list, const std::string& path, Color background)
{
    const float scale = std::max(0.01f, display_list.viewport.device_scale_factor);
    const double width = std::max(1.0, static_cast<double>(display_list.viewport.width) * scale);
    const double height = std::max(1.0, static_cast<double>(display_list.viewport.height) * scale);

    cairo_surface_t* raw_surface = cairo_pdf_surface_create(path.c_str(), width, height);
    check_status(cairo_surface_status(raw_surface), "create Cairo PDF surface");
    std::unique_ptr<cairo_surface_t, decltype(&cairo_surface_destroy)> surface(raw_surface, cairo_surface_destroy);

    cairo_t* raw_cr = cairo_create(surface.get());
    check_status(cairo_status(raw_cr), "create Cairo PDF context");
    std::unique_ptr<cairo_t, decltype(&cairo_destroy)> cr(raw_cr, cairo_destroy);

    draw_display_list(cr.get(), display_list, background, scale);
    cairo_show_page(cr.get());
    check_status(cairo_status(cr.get()), "finish Cairo PDF page");

    cairo_surface_finish(surface.get());
    check_status(cairo_surface_status(surface.get()), "write Cairo PDF file");
}

} // namespace pagecore
