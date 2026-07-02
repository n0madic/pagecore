#include "pagecore/render.hpp"

#include "pagecore/image_decoder.hpp"
#include "pagecore/resource_loader.hpp"
#include "web_fonts.hpp"

#include <cairo.h>
#include <litehtml.h>
#include <litehtml/render_item.h>
#include <pango/pangocairo.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace pagecore {
namespace {

// Enables heterogeneous lookup on the width cache: find(std::string_view) hashes
// the run directly instead of first materializing a temporary std::string (a heap
// allocation for long runs) on every text_width() call.
struct TransparentStringHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view value) const
    {
        return std::hash<std::string_view>{}(value);
    }
};

struct FontHandle {
    Font font;
    PangoFontDescription* description = nullptr;
    // Pango text shaping is deterministic for a given (font, string), so widths
    // are memoized per font. The cache lives and dies with the font handle.
    std::unordered_map<std::string, int, TransparentStringHash, std::equal_to<>> width_cache;
};

float px(litehtml::pixel_t value)
{
    return value.value();
}

Rect rect_from(const litehtml::position& position)
{
    return Rect{
        px(position.x),
        px(position.y),
        px(position.width),
        px(position.height),
    };
}

Color color_from(const litehtml::web_color& color)
{
    return Color{color.red, color.green, color.blue, color.alpha};
}

BorderStyle border_style_from(litehtml::border_style style)
{
    switch (style) {
    case litehtml::border_style_none:
    case litehtml::border_style_hidden:
        return BorderStyle::None;
    case litehtml::border_style_solid:
        return BorderStyle::Solid;
    case litehtml::border_style_dotted:
        return BorderStyle::Dotted;
    case litehtml::border_style_dashed:
        return BorderStyle::Dashed;
    case litehtml::border_style_double:
        return BorderStyle::Double;
    default:
        return BorderStyle::Other;
    }
}

BorderSide border_side_from(const litehtml::border& border)
{
    return BorderSide{
        px(border.width),
        color_from(border.color),
        border_style_from(border.style),
    };
}

BorderRadii border_radii_from(const litehtml::border_radiuses& radii)
{
    return BorderRadii{
        CornerRadii{px(radii.top_left_x), px(radii.top_left_y)},
        CornerRadii{px(radii.top_right_x), px(radii.top_right_y)},
        CornerRadii{px(radii.bottom_right_x), px(radii.bottom_right_y)},
        CornerRadii{px(radii.bottom_left_x), px(radii.bottom_left_y)},
    };
}

ImageRepeat image_repeat_from(litehtml::background_repeat repeat)
{
    switch (repeat) {
    case litehtml::background_repeat_repeat:
        return ImageRepeat::Repeat;
    case litehtml::background_repeat_repeat_x:
        return ImageRepeat::RepeatX;
    case litehtml::background_repeat_repeat_y:
        return ImageRepeat::RepeatY;
    case litehtml::background_repeat_no_repeat:
    default:
        return ImageRepeat::NoRepeat;
    }
}

std::vector<GradientStop> gradient_stops_from(const std::vector<litehtml::background_layer::color_point>& points)
{
    std::vector<GradientStop> stops;
    stops.reserve(points.size());
    for (const auto& point : points) {
        stops.push_back(GradientStop{
            std::clamp(point.offset, 0.0f, 1.0f),
            color_from(point.color),
        });
    }
    return stops;
}

// Trims a fixed-precision float down to the shortest representation CSSOM
// getters use (e.g. "50" not "50.000", "1.5" not "1.500").
std::string format_number(float value)
{
    if (!std::isfinite(value)) {
        value = 0.0f;
    }
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(3) << value;
    std::string text = stream.str();

    const auto dot = text.find('.');
    if (dot != std::string::npos) {
        auto last = text.find_last_not_of('0');
        if (last == dot) {
            --last;
        }
        text.erase(last + 1);
    }
    return text;
}

std::string format_length(const litehtml::css_length& length, std::string_view predefined_keyword)
{
    if (length.is_predefined()) {
        return std::string(predefined_keyword);
    }
    if (length.units() == litehtml::css_units_percentage) {
        return format_number(length.val()) + "%";
    }
    return format_number(length.val()) + "px";
}

std::string format_color(const litehtml::web_color& color)
{
    if (color.alpha == 255) {
        std::ostringstream stream;
        stream << "rgb(" << static_cast<int>(color.red) << ", " << static_cast<int>(color.green) << ", "
               << static_cast<int>(color.blue) << ")";
        return stream.str();
    }
    std::ostringstream stream;
    stream << "rgba(" << static_cast<int>(color.red) << ", " << static_cast<int>(color.green) << ", "
           << static_cast<int>(color.blue) << ", " << format_number(static_cast<float>(color.alpha) / 255.0f) << ")";
    return stream.str();
}

void transform_ascii(std::string& text, litehtml::text_transform transform)
{
    switch (transform) {
    case litehtml::text_transform_uppercase:
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
            return static_cast<char>(std::toupper(ch));
        });
        break;
    case litehtml::text_transform_lowercase:
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        break;
    case litehtml::text_transform_capitalize: {
        bool at_word_start = true;
        for (char& ch : text) {
            const auto uch = static_cast<unsigned char>(ch);
            if (std::isspace(uch)) {
                at_word_start = true;
            } else if (at_word_start) {
                ch = static_cast<char>(std::toupper(uch));
                at_word_start = false;
            }
        }
        break;
    }
    default:
        break;
    }
}

bool has_url_scheme(std::string_view value)
{
    const auto colon = value.find(':');
    if (colon == std::string_view::npos) {
        return false;
    }

    const auto first_delimiter = value.find_first_of("/?#");
    return first_delimiter == std::string_view::npos || colon < first_delimiter;
}

std::string resolve_litehtml_base(std::string_view document_base_url, std::string_view base_url)
{
    if (base_url.empty()) {
        return std::string(document_base_url);
    }
    if (has_url_scheme(base_url)) {
        return std::string(base_url);
    }
    return resolve_url(document_base_url, base_url);
}

class LiteHtmlDisplayListContainer final : public litehtml::document_container {
public:
    LiteHtmlDisplayListContainer()
        : measure_surface_(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 2, 2))
        , measure_cr_(cairo_create(measure_surface_))
    {
        if (cairo_surface_status(measure_surface_) != CAIRO_STATUS_SUCCESS
            || cairo_status(measure_cr_) != CAIRO_STATUS_SUCCESS) {
            // The constructor hasn't completed, so ~LiteHtmlDisplayListContainer will
            // not run; free the members created in the initializer list here.
            cairo_destroy(measure_cr_);
            cairo_surface_destroy(measure_surface_);
            throw std::runtime_error("failed to create Cairo/Pango measurement context");
        }

        // Reused across text_width()/create_font() so measurement does not
        // allocate a fresh PangoLayout per call (the hot path during layout).
        reset_measure_layout();
        if (measure_layout_ == nullptr) {
            cairo_destroy(measure_cr_);
            cairo_surface_destroy(measure_surface_);
            throw std::runtime_error("failed to create Pango measurement layout");
        }
    }

    ~LiteHtmlDisplayListContainer() override
    {
        if (measure_layout_ != nullptr) {
            g_object_unref(measure_layout_);
        }
        cairo_destroy(measure_cr_);
        cairo_surface_destroy(measure_surface_);
    }

    void set_viewport(Viewport viewport)
    {
        viewport_ = viewport;
    }

    void set_resource_loader(std::shared_ptr<ResourceLoader> loader)
    {
        loader_ = std::move(loader);
    }

    void set_font_environment(std::shared_ptr<const FontEnvironment> font_environment)
    {
        // Destroy the old measurement layout (which references the old font map via
        // its PangoContext) BEFORE dropping our reference to the old FontEnvironment,
        // so the old environment cannot be destroyed while the layout still points
        // at it (use-after-free in fontconfig/pango).
        if (measure_layout_ != nullptr) {
            g_object_unref(measure_layout_);
            measure_layout_ = nullptr;
        }
        font_environment_ = std::move(font_environment);
        reset_measure_layout();
        if (measure_layout_ == nullptr) {
            throw std::runtime_error("failed to create Pango measurement layout");
        }
    }

    void set_display_list(DisplayList* display_list)
    {
        display_list_ = display_list;
    }

    void requested_images_clear()
    {
        requested_images_.clear();
        decoded_images_.clear();
    }

    std::shared_ptr<const DecodedImage> decoded_image_for(const char* src, const char* baseurl)
    {
        if (src == nullptr || *src == '\0' || !loader_) {
            return {};
        }

        const std::string resource_base_url = resolve_litehtml_base(
            base_url_,
            baseurl == nullptr ? std::string_view() : std::string_view(baseurl));
        const std::string image_url = resolve_url(resource_base_url, src);
        auto cached = decoded_images_.find(image_url);
        if (cached != decoded_images_.end()) {
            return cached->second;
        }

        requested_images_.insert(image_url);
        try {
            auto response = loader_->load(ResourceRequest{
                image_url,
                ResourceKind::Image,
                base_url_,
                resource_base_url,
            });
            auto image = decode_image_rgba(response.body);
            decoded_images_[image_url] = image;
            return image;
        } catch (const ResourceError&) {
            decoded_images_[image_url] = {};
            return {};
        } catch (const std::runtime_error&) {
            decoded_images_[image_url] = {};
            return {};
        }
    }

    litehtml::uint_ptr create_font(
        const litehtml::font_description& descr,
        const litehtml::document*,
        litehtml::font_metrics* metrics) override
    {
        auto* handle = new FontHandle;
        handle->font.family = descr.family.empty() ? get_default_font_name() : descr.family;
        handle->font.size_px = std::max(1.0f, px(descr.size));
        handle->font.weight = descr.weight;
        handle->font.italic = descr.style == litehtml::font_style_italic;
        handle->description = pango_font_description_new();
        pango_font_description_set_family(handle->description, handle->font.family.c_str());
        // Clamp before multiplying by PANGO_SCALE so a pathological font-size cannot
        // overflow the int Pango size (signed overflow is UB).
        const double device_px = std::min(
            static_cast<double>(handle->font.size_px),
            static_cast<double>(std::numeric_limits<int>::max() / PANGO_SCALE));
        pango_font_description_set_absolute_size(
            handle->description,
            static_cast<int>(std::round(device_px * PANGO_SCALE)));
        pango_font_description_set_weight(
            handle->description,
            static_cast<PangoWeight>(std::clamp(handle->font.weight, 100, 1000)));
        pango_font_description_set_style(
            handle->description,
            handle->font.italic ? PANGO_STYLE_ITALIC : PANGO_STYLE_NORMAL);

        if (metrics != nullptr) {
            // Reuse the shared measurement layout instead of allocating a fresh
            // PangoLayout+context per create_font. This is safe: create_font is
            // not interleaved with text_width mid-call, and text_width always
            // re-sets the font description and text before measuring, so the
            // "x"/"0" left behind here never leaks into a width lookup.
            PangoContext* context = pango_layout_get_context(measure_layout_);
            PangoFontMetrics* pango_metrics = pango_context_get_metrics(
                context,
                handle->description,
                pango_language_get_default());

            const int pango_ascent = PANGO_PIXELS(pango_font_metrics_get_ascent(pango_metrics));
            int pango_height = PANGO_PIXELS(pango_font_metrics_get_height(pango_metrics));
            if (pango_height <= 0) {
                pango_height = static_cast<int>(std::ceil(handle->font.size_px * 1.25f));
            }

            metrics->font_size = handle->font.size_px;
            metrics->ascent = pango_ascent;
            metrics->height = pango_height;
            metrics->descent = std::max(0, pango_height - pango_ascent);
            metrics->draw_spaces = true;
            metrics->sub_shift = std::ceil(handle->font.size_px * 0.2f);
            metrics->super_shift = std::ceil(handle->font.size_px * 0.35f);

            pango_layout_set_font_description(measure_layout_, handle->description);
            PangoRectangle ink_rect;
            PangoRectangle logical_rect;
            pango_layout_set_text(measure_layout_, "x", 1);
            pango_layout_get_pixel_extents(measure_layout_, &ink_rect, &logical_rect);
            metrics->x_height = ink_rect.height > 0 ? ink_rect.height : std::ceil(handle->font.size_px * 0.5f);

            pango_layout_set_text(measure_layout_, "0", 1);
            pango_layout_get_pixel_extents(measure_layout_, &ink_rect, &logical_rect);
            metrics->ch_width = logical_rect.width > 0 ? logical_rect.width : std::ceil(handle->font.size_px * 0.55f);

            pango_font_metrics_unref(pango_metrics);
        }

        return reinterpret_cast<litehtml::uint_ptr>(handle);
    }

    void delete_font(litehtml::uint_ptr hFont) override
    {
        auto* handle = reinterpret_cast<FontHandle*>(hFont);
        if (handle != nullptr) {
            pango_font_description_free(handle->description);
            delete handle;
        }
    }

    litehtml::pixel_t text_width(const char* text, litehtml::uint_ptr hFont) override
    {
        auto* handle = reinterpret_cast<FontHandle*>(hFont);
        if (text == nullptr || *text == '\0') {
            return 0;
        }

        if (handle != nullptr) {
            auto cached = handle->width_cache.find(std::string_view(text));
            if (cached != handle->width_cache.end()) {
                return cached->second;
            }
        }

        // Always set the description (handle's or NULL=default) so the reused
        // layout never inherits a font from a previous measurement.
        pango_layout_set_font_description(measure_layout_, handle != nullptr ? handle->description : nullptr);
        pango_layout_set_text(measure_layout_, text, -1);
        pango_cairo_update_layout(measure_cr_, measure_layout_);

        int width = 0;
        int height = 0;
        pango_layout_get_pixel_size(measure_layout_, &width, &height);

        if (handle != nullptr) {
            handle->width_cache.emplace(text, width);
        }
        return width;
    }

    void draw_text(
        litehtml::uint_ptr,
        const char* text,
        litehtml::uint_ptr hFont,
        litehtml::web_color color,
        const litehtml::position& pos) override
    {
        if (display_list_ == nullptr || text == nullptr || *text == '\0') {
            return;
        }

        const auto* handle = reinterpret_cast<const FontHandle*>(hFont);
        Font font = handle == nullptr ? Font{get_default_font_name(), px(get_default_font_size()), 400, false}
                                      : handle->font;

        Rect rect = rect_from(pos);
        if (rect.width <= 0.0f) {
            rect.width = text_width(text, hFont);
        }

        display_list_->commands.emplace_back(TextCommand{
            text,
            rect,
            color_from(color),
            std::move(font),
        });
    }

    litehtml::pixel_t pt_to_px(float pt) const override
    {
        return pt * 96.0f / 72.0f;
    }

    litehtml::pixel_t get_default_font_size() const override
    {
        return 16;
    }

    const char* get_default_font_name() const override
    {
        return "Arial";
    }

    void draw_list_marker(litehtml::uint_ptr, const litehtml::list_marker& marker) override
    {
        if (display_list_ == nullptr) {
            return;
        }

        std::string text;
        switch (marker.marker_type) {
        case litehtml::list_style_type_decimal:
            text = std::to_string(marker.index) + ".";
            break;
        case litehtml::list_style_type_circle:
            text = "o";
            break;
        case litehtml::list_style_type_square:
            text = "*";
            break;
        case litehtml::list_style_type_disc:
            text = "*";
            break;
        default:
            return;
        }

        Font font{get_default_font_name(), px(get_default_font_size()), 400, false};
        if (marker.font != 0) {
            font = reinterpret_cast<const FontHandle*>(marker.font)->font;
        }

        display_list_->commands.emplace_back(TextCommand{
            std::move(text),
            rect_from(marker.pos),
            color_from(marker.color),
            std::move(font),
        });
    }

    void load_image(const char* src, const char* baseurl, bool) override
    {
        (void) decoded_image_for(src, baseurl);
    }

    void get_image_size(const char* src, const char* baseurl, litehtml::size& sz) override
    {
        if (auto image = decoded_image_for(src, baseurl)) {
            sz = litehtml::size(image->width, image->height);
            return;
        }
        sz = litehtml::size(0, 0);
    }

    void draw_image(
        litehtml::uint_ptr,
        const litehtml::background_layer& layer,
        const std::string& url,
        const std::string& base_url) override
    {
        if (display_list_ == nullptr) {
            return;
        }

        const std::string resource_base_url = resolve_litehtml_base(base_url_, base_url);
        display_list_->commands.emplace_back(ImageCommand{
            rect_from(layer.border_box),
            rect_from(layer.clip_box),
            rect_from(layer.origin_box),
            image_repeat_from(layer.repeat),
            resolve_url(resource_base_url, url),
            resource_base_url,
            decoded_image_for(url.c_str(), resource_base_url.c_str()),
            border_radii_from(layer.border_radius),
        });
    }

    void draw_solid_fill(
        litehtml::uint_ptr,
        const litehtml::background_layer& layer,
        const litehtml::web_color& color) override
    {
        if (display_list_ == nullptr || color.alpha == 0) {
            return;
        }

        display_list_->commands.emplace_back(SolidFillCommand{
            rect_from(layer.border_box),
            color_from(color),
            layer.is_root,
            border_radii_from(layer.border_radius),
        });
    }

    void draw_linear_gradient(
        litehtml::uint_ptr,
        const litehtml::background_layer& layer,
        const litehtml::background_layer::linear_gradient& gradient) override
    {
        if (display_list_ == nullptr || gradient.color_points.empty()) {
            return;
        }

        display_list_->commands.emplace_back(LinearGradientCommand{
            rect_from(layer.border_box),
            rect_from(layer.clip_box),
            Point{gradient.start.x, gradient.start.y},
            Point{gradient.end.x, gradient.end.y},
            gradient_stops_from(gradient.color_points),
            border_radii_from(layer.border_radius),
        });
    }

    void draw_radial_gradient(
        litehtml::uint_ptr,
        const litehtml::background_layer&,
        const litehtml::background_layer::radial_gradient&) override
    {
    }

    void draw_conic_gradient(
        litehtml::uint_ptr,
        const litehtml::background_layer&,
        const litehtml::background_layer::conic_gradient&) override
    {
    }

    void draw_borders(
        litehtml::uint_ptr,
        const litehtml::borders& borders,
        const litehtml::position& draw_pos,
        bool root) override
    {
        if (display_list_ == nullptr || !borders.is_visible()) {
            return;
        }

        display_list_->commands.emplace_back(BorderCommand{
            rect_from(draw_pos),
            border_side_from(borders.left),
            border_side_from(borders.top),
            border_side_from(borders.right),
            border_side_from(borders.bottom),
            root,
            border_radii_from(borders.radius),
        });
    }

    void set_caption(const char*) override
    {
    }

    void set_base_url(const char* base_url) override
    {
        if (base_url != nullptr) {
            base_url_ = resolve_litehtml_base(document_base_url_, base_url);
        }
    }

    void set_document_base_url(std::string_view base_url)
    {
        document_base_url_.assign(base_url);
        base_url_ = document_base_url_;
    }

    void link(const std::shared_ptr<litehtml::document>&, const litehtml::element::ptr&) override
    {
    }

    void on_anchor_click(const char*, const litehtml::element::ptr&) override
    {
    }

    void on_mouse_event(const litehtml::element::ptr&, litehtml::mouse_event) override
    {
    }

    void set_cursor(const char*) override
    {
    }

    void transform_text(std::string& text, litehtml::text_transform transform) override
    {
        transform_ascii(text, transform);
    }

    void import_css(std::string& text, const std::string& url, std::string& baseurl) override
    {
        const std::string source_base_url = resolve_litehtml_base(base_url_, baseurl);
        const std::string request_url = resolve_url(source_base_url, url);

        if (!loader_) {
            text.clear();
            baseurl = request_url;
            return;
        }

        auto response = loader_->load(ResourceRequest{
            request_url,
            ResourceKind::Stylesheet,
            base_url_,
            source_base_url,
        });
        text = std::move(response.body);
        baseurl = response.url.empty() ? request_url : response.url;
    }

    void set_clip(const litehtml::position& pos, const litehtml::border_radiuses& bdr_radius) override
    {
        if (display_list_ != nullptr) {
            display_list_->commands.emplace_back(ClipCommand{rect_from(pos), true, border_radii_from(bdr_radius)});
        }
    }

    void del_clip() override
    {
        if (display_list_ != nullptr) {
            display_list_->commands.emplace_back(ClipCommand{{}, false, {}});
        }
    }

    void get_viewport(litehtml::position& viewport) const override
    {
        viewport = litehtml::position(0, 0, viewport_.width, viewport_.height);
    }

    litehtml::element::ptr create_element(
        const char*,
        const litehtml::string_map&,
        const std::shared_ptr<litehtml::document>&) override
    {
        return nullptr;
    }

    void get_media_features(litehtml::media_features& media) const override
    {
        litehtml::position viewport;
        get_viewport(viewport);
        media.type = litehtml::media_type_screen;
        media.width = viewport.width;
        media.height = viewport.height;
        media.color = 8;
        media.monochrome = 0;
        media.color_index = 0;
        media.resolution = static_cast<int>(96.0f * viewport_.device_scale_factor);
    }

    void get_language(std::string& language, std::string& culture) const override
    {
        language = "en";
        culture = "US";
    }

private:
    void reset_measure_layout()
    {
        if (measure_layout_ != nullptr) {
            g_object_unref(measure_layout_);
        }
        measure_layout_ = create_pango_layout_for_cairo(measure_cr_, font_environment_);
    }

    Viewport viewport_;
    std::shared_ptr<ResourceLoader> loader_;
    std::shared_ptr<const FontEnvironment> font_environment_;
    DisplayList* display_list_ = nullptr;
    std::string document_base_url_;
    std::string base_url_;
    std::unordered_set<std::string> requested_images_;
    std::unordered_map<std::string, std::shared_ptr<const DecodedImage>> decoded_images_;
    cairo_surface_t* measure_surface_ = nullptr;
    cairo_t* measure_cr_ = nullptr;
    PangoLayout* measure_layout_ = nullptr;
};

class LiteHtmlLayoutEngine final : public LayoutEngine {
public:
    void set_resource_loader(std::shared_ptr<ResourceLoader> loader) override
    {
        loader_ = std::move(loader);
        container_.set_resource_loader(loader_);
    }

    void set_font_environment(std::shared_ptr<const FontEnvironment> font_environment) override
    {
        font_environment_ = std::move(font_environment);
        container_.set_font_environment(font_environment_);
        display_list_.font_environment = font_environment_;
    }

    void set_viewport(Viewport viewport) override
    {
        viewport_ = viewport;
        container_.set_viewport(viewport_);
        display_list_.viewport = viewport_;
    }

    void load_html(std::string_view html, std::string_view base_url) override
    {
        html_.assign(html);
        base_url_.assign(base_url);
        container_.set_resource_loader(loader_);
        container_.set_document_base_url(base_url_);
        container_.requested_images_clear();
        document_ = litehtml::document::createFromString(litehtml::estring(html_), &container_);
        if (!document_) {
            throw std::runtime_error("litehtml failed to parse HTML");
        }
        // createFromString() already runs the cascade once (document.cpp),
        // so styles are computed before any explicit compute_styles_only().
        styles_computed_ = true;
        display_list_.clear();
        display_list_.viewport = viewport_;
        display_list_.font_environment = font_environment_;
        // Drop the old document's element::ptrs immediately rather than
        // waiting for the next find_tagged_element() call to overwrite them.
        tagged_elements_.clear();
        tagged_elements_indexed_ = false;
    }

    void layout() override
    {
        if (!document_) {
            throw std::runtime_error("layout requires loaded HTML");
        }

        display_list_.clear();
        display_list_.viewport = viewport_;
        display_list_.font_environment = font_environment_;
        container_.set_viewport(viewport_);
        container_.set_display_list(&display_list_);

        document_->render(viewport_.width);

        display_list_.content_width = static_cast<int>(document_->width());
        display_list_.content_height = static_cast<int>(document_->height());

        const int draw_height = std::max(viewport_.height, display_list_.content_height);
        litehtml::position clip(0, 0, viewport_.width, draw_height);
        document_->draw(0, 0, 0, &clip);

        container_.set_display_list(nullptr);
    }

    const DisplayList& display_list() const override
    {
        return display_list_;
    }

    void compute_styles_only() override
    {
        ensure_styles_computed();
    }

    std::optional<ComputedStyle> computed_style(std::string_view node_key) override
    {
        if (!document_) {
            return std::nullopt;
        }
        ensure_styles_computed();

        auto element = find_tagged_element(node_key);
        if (!element) {
            return std::nullopt;
        }
        return ComputedStyle{computed_style_properties(element)};
    }

    std::optional<std::string> computed_style_property(std::string_view node_key, std::string_view property) override
    {
        if (!document_) {
            return std::nullopt;
        }
        ensure_styles_computed();

        auto element = find_tagged_element(node_key);
        if (!element) {
            return std::nullopt;
        }
        return computed_style_property_value(element, property);
    }

    std::optional<ElementGeometry> element_geometry(std::string_view node_key) override
    {
        if (!document_) {
            return std::nullopt;
        }

        auto element = find_tagged_element(node_key);
        if (!element) {
            return std::nullopt;
        }

        auto render_item = element->get_render_item();
        if (!render_item) {
            // No layout() pass has run yet, or the element is display:none.
            return std::nullopt;
        }

        litehtml::position content = render_item->get_placement();
        litehtml::position padding_box = content;
        padding_box += render_item->get_paddings();
        litehtml::position border_box = padding_box;
        border_box += render_item->get_borders();

        return ElementGeometry{rect_from(border_box), rect_from(padding_box)};
    }

    std::vector<AbsolutePercentWidthOverride> collect_absolute_percent_width_overrides() override
    {
        std::vector<AbsolutePercentWidthOverride> overrides;
        if (document_) {
            collect_absolute_percent_width_overrides(document_->root(), overrides);
        }
        return overrides;
    }

private:
    // Single typed tree walk mirroring the string-based page.cpp path exactly: for
    // every position:absolute; box-sizing:border-box; width:% element that litehtml
    // laid out, pin its OWN computed border-box width (placement + paddings +
    // borders, same as element_geometry) so its percentage-width children don't
    // collapse. No string formatting, no Lexbor query; empty when there are none.
    void collect_absolute_percent_width_overrides(
        const litehtml::element::ptr& element,
        std::vector<AbsolutePercentWidthOverride>& overrides)
    {
        if (!element) {
            return;
        }

        const litehtml::css_properties& css = element->css();
        if (css.get_position() == litehtml::element_position_absolute
            && css.get_box_sizing() == litehtml::box_sizing_border_box
            && !css.get_width().is_predefined()
            && css.get_width().units() == litehtml::css_units_percentage) {
            if (auto render_item = element->get_render_item()) {
                litehtml::position border_box = render_item->get_placement();
                border_box += render_item->get_paddings();
                border_box += render_item->get_borders();
                const float width = px(border_box.width);
                const char* sid = element->get_attr("data-pc-sid");
                // width >= 1 mirrors the string path's border_box.width < 1 skip
                // (not laid out, e.g. display:none). round-to-nearest, min 1.
                if (width >= 1.0f && sid != nullptr && *sid != '\0') {
                    overrides.push_back(AbsolutePercentWidthOverride{
                        sid,
                        std::max(1, static_cast<int>(width + 0.5f)),
                    });
                }
            }
        }

        for (const auto& child : element->children()) {
            collect_absolute_percent_width_overrides(child, overrides);
        }
    }

    // O(1) data-pc-sid -> element lookup, built once per document (lazily,
    // on first lookup) and reused across every computed_style()/
    // element_geometry() call. Without this, find_tagged_element() used
    // litehtml::select_one("[data-pc-sid=\"N\"]"), which re-parses the
    // selector and does a full document tree walk on EVERY call; with
    // getComputedStyle()/getBoundingClientRect() etc. called per element on
    // real pages, that's O(n) work per call, i.e. O(n^2) for the page.
    void index_tagged_elements(const litehtml::element::ptr& element)
    {
        if (!element) {
            return;
        }
        const char* sid = element->get_attr("data-pc-sid");
        if (sid != nullptr && *sid != '\0') {
            tagged_elements_.emplace(sid, element);
        }
        for (const auto& child : element->children()) {
            index_tagged_elements(child);
        }
    }

    litehtml::element::ptr find_tagged_element(std::string_view node_key)
    {
        if (!document_) {
            return nullptr;
        }

        if (!tagged_elements_indexed_) {
            index_tagged_elements(document_->root());
            tagged_elements_indexed_ = true;
        }

        auto found = tagged_elements_.find(std::string(node_key));
        return found != tagged_elements_.end() ? found->second : nullptr;
    }

    void ensure_styles_computed()
    {
        if (!styles_computed_ && document_) {
            document_->root()->compute_styles(true);
            styles_computed_ = true;
        }
    }

    std::vector<std::pair<std::string, std::string>> computed_style_properties(
        const litehtml::element::ptr& element) const
    {
        const litehtml::css_properties& css = element->css();
        std::vector<std::pair<std::string, std::string>> properties;
        properties.reserve(32);

        properties.emplace_back("display", std::string(litehtml::style_display_strings[css.get_display()]));
        properties.emplace_back("position", std::string(litehtml::element_position_strings[css.get_position()]));
        properties.emplace_back("float", std::string(litehtml::element_float_strings[css.get_float()]));
        properties.emplace_back("color", format_color(css.get_color()));
        properties.emplace_back("background-color", format_color(css.get_bg().m_color));
        properties.emplace_back("font-size", format_number(px(css.get_font_size())) + "px");

        const auto* font = reinterpret_cast<const FontHandle*>(css.get_font());
        if (font != nullptr) {
            properties.emplace_back("font-family", font->font.family);
            properties.emplace_back("font-weight", std::to_string(font->font.weight));
            properties.emplace_back("font-style", font->font.italic ? "italic" : "normal");
        } else {
            properties.emplace_back("font-family", container_.get_default_font_name());
            properties.emplace_back("font-weight", "400");
            properties.emplace_back("font-style", "normal");
        }

        properties.emplace_back("line-height", format_number(px(css.line_height().computed_value)) + "px");

        properties.emplace_back("width", format_length(css.get_width(), "auto"));
        properties.emplace_back("height", format_length(css.get_height(), "auto"));
        properties.emplace_back("min-width", format_length(css.get_min_width(), "auto"));
        properties.emplace_back("min-height", format_length(css.get_min_height(), "auto"));
        properties.emplace_back("max-width", format_length(css.get_max_width(), "none"));
        properties.emplace_back("max-height", format_length(css.get_max_height(), "none"));

        const litehtml::css_margins& margins = css.get_margins();
        properties.emplace_back("margin-left", format_length(margins.left, "auto"));
        properties.emplace_back("margin-right", format_length(margins.right, "auto"));
        properties.emplace_back("margin-top", format_length(margins.top, "auto"));
        properties.emplace_back("margin-bottom", format_length(margins.bottom, "auto"));

        const litehtml::css_margins& padding = css.get_padding();
        properties.emplace_back("padding-left", format_length(padding.left, "0px"));
        properties.emplace_back("padding-right", format_length(padding.right, "0px"));
        properties.emplace_back("padding-top", format_length(padding.top, "0px"));
        properties.emplace_back("padding-bottom", format_length(padding.bottom, "0px"));

        const litehtml::css_offsets& offsets = css.get_offsets();
        properties.emplace_back("left", format_length(offsets.left, "auto"));
        properties.emplace_back("right", format_length(offsets.right, "auto"));
        properties.emplace_back("top", format_length(offsets.top, "auto"));
        properties.emplace_back("bottom", format_length(offsets.bottom, "auto"));

        properties.emplace_back("text-align", std::string(litehtml::text_align_strings[css.get_text_align()]));
        properties.emplace_back("visibility", std::string(litehtml::visibility_strings[css.get_visibility()]));
        properties.emplace_back("white-space", std::string(litehtml::white_space_strings[css.get_white_space()]));
        properties.emplace_back("overflow", std::string(litehtml::overflow_strings[css.get_overflow()]));
        properties.emplace_back("box-sizing", std::string(litehtml::box_sizing_strings[css.get_box_sizing()]));
        // get_z_index() collapses "auto" and an explicit "0" to the same
        // value (litehtml doesn't expose is_predefined() for z-index).
        properties.emplace_back("z-index", std::to_string(css.get_z_index()));
        properties.emplace_back("vertical-align", std::string(litehtml::vertical_align_strings[css.get_vertical_align()]));
        properties.emplace_back("text-indent", format_length(css.get_text_indent(), "0px"));

        properties.emplace_back(
            "list-style-type", std::string(litehtml::list_style_type_strings[css.get_list_style_type()]));
        properties.emplace_back(
            "list-style-position", std::string(litehtml::list_style_position_strings[css.get_list_style_position()]));
        properties.emplace_back(
            "list-style-image",
            css.get_list_style_image().empty() ? "none" : ("url(\"" + css.get_list_style_image() + "\")"));

        // litehtml applies opacity at draw time and doesn't store it on
        // css_properties; known limitation until upstream exposes it.
        properties.emplace_back("opacity", "1");

        return properties;
    }

    std::optional<std::string> computed_style_property_value(
        const litehtml::element::ptr& element,
        std::string_view property) const
    {
        const litehtml::css_properties& css = element->css();
        if (property == "display") return std::string(litehtml::style_display_strings[css.get_display()]);
        if (property == "position") return std::string(litehtml::element_position_strings[css.get_position()]);
        if (property == "float") return std::string(litehtml::element_float_strings[css.get_float()]);
        if (property == "color") return format_color(css.get_color());
        if (property == "background-color") return format_color(css.get_bg().m_color);
        if (property == "font-size") return format_number(px(css.get_font_size())) + "px";

        if (property == "font-family" || property == "font-weight" || property == "font-style") {
            const auto* font = reinterpret_cast<const FontHandle*>(css.get_font());
            if (property == "font-family") {
                return font != nullptr ? font->font.family : container_.get_default_font_name();
            }
            if (property == "font-weight") {
                return font != nullptr ? std::to_string(font->font.weight) : std::string("400");
            }
            return font != nullptr && font->font.italic ? std::string("italic") : std::string("normal");
        }

        if (property == "line-height") return format_number(px(css.line_height().computed_value)) + "px";
        if (property == "width") return format_length(css.get_width(), "auto");
        if (property == "height") return format_length(css.get_height(), "auto");
        if (property == "min-width") return format_length(css.get_min_width(), "auto");
        if (property == "min-height") return format_length(css.get_min_height(), "auto");
        if (property == "max-width") return format_length(css.get_max_width(), "none");
        if (property == "max-height") return format_length(css.get_max_height(), "none");

        const litehtml::css_margins& margins = css.get_margins();
        if (property == "margin-left") return format_length(margins.left, "auto");
        if (property == "margin-right") return format_length(margins.right, "auto");
        if (property == "margin-top") return format_length(margins.top, "auto");
        if (property == "margin-bottom") return format_length(margins.bottom, "auto");

        const litehtml::css_margins& padding = css.get_padding();
        if (property == "padding-left") return format_length(padding.left, "0px");
        if (property == "padding-right") return format_length(padding.right, "0px");
        if (property == "padding-top") return format_length(padding.top, "0px");
        if (property == "padding-bottom") return format_length(padding.bottom, "0px");

        const litehtml::css_offsets& offsets = css.get_offsets();
        if (property == "left") return format_length(offsets.left, "auto");
        if (property == "right") return format_length(offsets.right, "auto");
        if (property == "top") return format_length(offsets.top, "auto");
        if (property == "bottom") return format_length(offsets.bottom, "auto");

        if (property == "text-align") return std::string(litehtml::text_align_strings[css.get_text_align()]);
        if (property == "visibility") return std::string(litehtml::visibility_strings[css.get_visibility()]);
        if (property == "white-space") return std::string(litehtml::white_space_strings[css.get_white_space()]);
        if (property == "overflow") return std::string(litehtml::overflow_strings[css.get_overflow()]);
        if (property == "box-sizing") return std::string(litehtml::box_sizing_strings[css.get_box_sizing()]);
        if (property == "z-index") return std::to_string(css.get_z_index());
        if (property == "vertical-align") return std::string(litehtml::vertical_align_strings[css.get_vertical_align()]);
        if (property == "text-indent") return format_length(css.get_text_indent(), "0px");
        if (property == "list-style-type") return std::string(litehtml::list_style_type_strings[css.get_list_style_type()]);
        if (property == "list-style-position") return std::string(litehtml::list_style_position_strings[css.get_list_style_position()]);
        if (property == "list-style-image") {
            return css.get_list_style_image().empty() ? std::string("none") : ("url(\"" + css.get_list_style_image() + "\")");
        }
        if (property == "opacity") return std::string("1");

        return std::nullopt;
    }

    Viewport viewport_;
    std::shared_ptr<ResourceLoader> loader_;
    std::shared_ptr<const FontEnvironment> font_environment_;
    LiteHtmlDisplayListContainer container_;
    std::string html_;
    std::string base_url_;
    litehtml::document::ptr document_;
    DisplayList display_list_;
    bool styles_computed_ = false;
    std::unordered_map<std::string, litehtml::element::ptr> tagged_elements_;
    bool tagged_elements_indexed_ = false;
};

} // namespace

std::unique_ptr<LayoutEngine> create_litehtml_layout_engine()
{
    auto engine = std::make_unique<LiteHtmlLayoutEngine>();
    engine->set_viewport(Viewport{});
    return engine;
}

class LiteHtmlLayoutEngineFactory final : public LayoutEngineFactory {
public:
    std::unique_ptr<LayoutEngine> create_layout_engine() override
    {
        return create_litehtml_layout_engine();
    }
};

std::shared_ptr<LayoutEngineFactory> create_litehtml_layout_engine_factory()
{
    return std::make_shared<LiteHtmlLayoutEngineFactory>();
}

} // namespace pagecore
