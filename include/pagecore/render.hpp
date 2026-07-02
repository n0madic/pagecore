#pragma once

#include "pagecore/perf.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace pagecore {

class ResourceLoader;
struct FontEnvironment;

struct Viewport {
    int width = 1280;
    int height = 720;
    float device_scale_factor = 1.0f;
};

struct RenderOptions {
    Viewport viewport;
    bool load_external_resources = true;
    std::optional<std::size_t> max_external_resource_loads;
    std::optional<std::size_t> max_external_resource_bytes;
    std::optional<std::chrono::milliseconds> max_external_resource_time;
    std::string base_url;
    PerfTraceCallback perf_trace;
};

struct RenderedImage {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;
};

struct DecodedImage {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;
};

struct Rect {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

struct Point {
    float x = 0.0f;
    float y = 0.0f;
};

struct CornerRadii {
    float x = 0.0f;
    float y = 0.0f;
};

struct BorderRadii {
    CornerRadii top_left;
    CornerRadii top_right;
    CornerRadii bottom_right;
    CornerRadii bottom_left;
};

struct Color {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 255;
};

struct Font {
    std::string family;
    float size_px = 16.0f;
    int weight = 400;
    bool italic = false;
};

struct TextCommand {
    std::string text;
    Rect rect;
    Color color;
    Font font;
};

struct SolidFillCommand {
    Rect rect;
    Color color;
    bool is_root = false;
    BorderRadii radii;
};

enum class BorderStyle {
    None,
    Solid,
    Dotted,
    Dashed,
    Double,
    Other,
};

struct BorderSide {
    float width = 0.0f;
    Color color;
    BorderStyle style = BorderStyle::None;
};

struct BorderCommand {
    Rect rect;
    BorderSide left;
    BorderSide top;
    BorderSide right;
    BorderSide bottom;
    bool is_root = false;
    BorderRadii radii;
};

enum class ImageRepeat {
    Repeat,
    RepeatX,
    RepeatY,
    NoRepeat,
};

struct ImageCommand {
    Rect rect;
    Rect clip;
    Rect tile;
    ImageRepeat repeat = ImageRepeat::NoRepeat;
    std::string url;
    std::string base_url;
    std::shared_ptr<const DecodedImage> image;
    BorderRadii radii;
};

struct GradientStop {
    float offset = 0.0f;
    Color color;
};

struct LinearGradientCommand {
    Rect rect;
    Rect clip;
    Point start;
    Point end;
    std::vector<GradientStop> stops;
    BorderRadii radii;
};

struct ClipCommand {
    Rect rect;
    bool push = true;
    BorderRadii radii;
};

using DisplayCommand = std::variant<
    TextCommand,
    SolidFillCommand,
    BorderCommand,
    ImageCommand,
    LinearGradientCommand,
    ClipCommand>;

struct DisplayList {
    Viewport viewport;
    int content_width = 0;
    int content_height = 0;
    std::shared_ptr<const FontEnvironment> font_environment;
    std::vector<DisplayCommand> commands;

    void clear()
    {
        content_width = 0;
        content_height = 0;
        font_environment.reset();
        commands.clear();
    }
};

struct ComputedStyle {
    std::vector<std::pair<std::string, std::string>> properties;
};

struct ElementGeometry {
    Rect border_box; // getBoundingClientRect / offsetWidth,offsetHeight
    Rect padding_box; // clientWidth,clientHeight (+ clientTop/Left = padding_box - border_box)
};

class LayoutEngine {
public:
    virtual ~LayoutEngine() = default;
    virtual void set_resource_loader(std::shared_ptr<ResourceLoader> loader) = 0;
    virtual void set_font_environment(std::shared_ptr<const FontEnvironment> font_environment)
    {
        (void) font_environment;
    }
    virtual void set_viewport(Viewport viewport) = 0;
    virtual void load_html(std::string_view html, std::string_view base_url) = 0;
    virtual void layout() = 0;
    virtual const DisplayList& display_list() const = 0;

    // Runs the CSS cascade without a full layout pass. Engines that don't
    // support computed-style read-back leave this a no-op.
    virtual void compute_styles_only() { }

    // Looks up a single element by an engine-defined key (e.g. an injected
    // marker attribute) and reads back its computed style. Default
    // implementation is for engines without read-back support.
    virtual std::optional<ComputedStyle> computed_style(std::string_view node_key)
    {
        (void) node_key;
        return std::nullopt;
    }

    virtual std::optional<std::string> computed_style_property(std::string_view node_key, std::string_view property)
    {
        auto style = computed_style(node_key);
        if (!style) {
            return std::nullopt;
        }
        for (const auto& [name, value] : style->properties) {
            if (name == property) {
                return value;
            }
        }
        return std::nullopt;
    }

    // Looks up a single element by the same engine-defined key and reads
    // back its box-model geometry from the last layout() pass. Returns
    // nullopt if the element doesn't participate in layout (display:none,
    // or layout() hasn't run yet). Default implementation is for engines
    // without read-back support.
    virtual std::optional<ElementGeometry> element_geometry(std::string_view node_key)
    {
        (void) node_key;
        return std::nullopt;
    }

    // A render-local correction the engine wants applied on a second layout pass:
    // pin `position:absolute; box-sizing:border-box; width:%` element `node_key`
    // to `border_box_width_px` so its percentage-width children don't collapse.
    struct AbsolutePercentWidthOverride {
        std::string node_key;
        int border_box_width_px = 0;
    };

    // Collects the above corrections from the last layout() pass using typed
    // style/geometry getters, so the common case (no such elements) short-circuits
    // with no string formatting or DOM query. Engines without read-back return {}.
    virtual std::vector<AbsolutePercentWidthOverride> collect_absolute_percent_width_overrides()
    {
        return {};
    }
};

class LayoutEngineFactory {
public:
    virtual ~LayoutEngineFactory() = default;
    virtual std::unique_ptr<LayoutEngine> create_layout_engine() = 0;
};

class RasterBackend {
public:
    virtual ~RasterBackend() = default;
    virtual RenderedImage render(const DisplayList& display_list) = 0;
};

std::unique_ptr<RasterBackend> create_default_raster_backend(Color background = {255, 255, 255, 255});
std::unique_ptr<RasterBackend> create_cairo_raster_backend(Color background = {255, 255, 255, 255});
void write_pdf(const DisplayList& display_list, const std::string& path, Color background = {255, 255, 255, 255});
std::string display_list_to_json(const DisplayList& display_list);

#if defined(PAGECORE_ENABLE_RENDERING)
std::unique_ptr<LayoutEngine> create_litehtml_layout_engine();
std::shared_ptr<LayoutEngineFactory> create_litehtml_layout_engine_factory();
#endif

} // namespace pagecore
