#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace pagecore {

class ResourceLoader;

struct Viewport {
    int width = 1280;
    int height = 720;
    float device_scale_factor = 1.0f;
};

struct RenderOptions {
    Viewport viewport;
    bool load_external_resources = true;
    std::string base_url;
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
    std::vector<DisplayCommand> commands;

    void clear()
    {
        content_width = 0;
        content_height = 0;
        commands.clear();
    }
};

class LayoutEngine {
public:
    virtual ~LayoutEngine() = default;
    virtual void set_resource_loader(std::shared_ptr<ResourceLoader> loader) = 0;
    virtual void set_viewport(Viewport viewport) = 0;
    virtual void load_html(std::string_view html, std::string_view base_url) = 0;
    virtual void layout() = 0;
    virtual const DisplayList& display_list() const = 0;
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
std::string display_list_to_json(const DisplayList& display_list);

#if defined(PAGECORE_ENABLE_RENDERING)
std::unique_ptr<LayoutEngine> create_litehtml_layout_engine();
std::shared_ptr<LayoutEngineFactory> create_litehtml_layout_engine_factory();
#endif

} // namespace pagecore
