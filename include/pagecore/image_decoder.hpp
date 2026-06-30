#pragma once

#include "pagecore/render.hpp"

#include <memory>
#include <string_view>

namespace pagecore {

std::shared_ptr<const DecodedImage> decode_image_rgba(std::string_view bytes);
std::shared_ptr<const DecodedImage> decode_png_rgba(std::string_view bytes);
std::shared_ptr<const DecodedImage> decode_jpeg_rgba(std::string_view bytes);
std::shared_ptr<const DecodedImage> decode_webp_rgba(std::string_view bytes);
std::shared_ptr<const DecodedImage> decode_gif_rgba(std::string_view bytes);
std::shared_ptr<const DecodedImage> decode_svg_rgba(std::string_view bytes);

} // namespace pagecore
