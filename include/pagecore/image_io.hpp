#pragma once

#include "pagecore/render.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace pagecore {

std::vector<std::uint8_t> encode_png_rgba(const RenderedImage& image);
void write_png_rgba(const RenderedImage& image, const std::string& path);

} // namespace pagecore
