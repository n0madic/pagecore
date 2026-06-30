#include "pagecore/image_decoder.hpp"
#include "pagecore/render.hpp"

#include <memory>
#include <stdexcept>
#include <string_view>

namespace pagecore {
namespace {

[[noreturn]] void throw_rendering_unavailable()
{
    throw std::runtime_error("rendering is not available; build with PAGECORE_ENABLE_RENDERING=ON");
}

class DisabledRasterBackend final : public RasterBackend {
public:
    RenderedImage render(const DisplayList&) override
    {
        throw_rendering_unavailable();
    }
};

} // namespace

std::unique_ptr<RasterBackend> create_cairo_raster_backend(Color)
{
    return std::make_unique<DisabledRasterBackend>();
}

std::unique_ptr<RasterBackend> create_default_raster_backend(Color background)
{
    return create_cairo_raster_backend(background);
}

void write_pdf(const DisplayList&, const std::string&, Color)
{
    throw_rendering_unavailable();
}

std::shared_ptr<const DecodedImage> decode_image_rgba(std::string_view)
{
    throw_rendering_unavailable();
}

std::shared_ptr<const DecodedImage> decode_png_rgba(std::string_view)
{
    throw_rendering_unavailable();
}

std::shared_ptr<const DecodedImage> decode_jpeg_rgba(std::string_view)
{
    throw_rendering_unavailable();
}

std::shared_ptr<const DecodedImage> decode_webp_rgba(std::string_view)
{
    throw_rendering_unavailable();
}

std::shared_ptr<const DecodedImage> decode_gif_rgba(std::string_view)
{
    throw_rendering_unavailable();
}

std::shared_ptr<const DecodedImage> decode_svg_rgba(std::string_view)
{
    throw_rendering_unavailable();
}

} // namespace pagecore
