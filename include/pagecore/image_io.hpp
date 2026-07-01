#pragma once

#include "pagecore/render.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace pagecore {

std::vector<std::uint8_t> encode_png_rgba(const RenderedImage& image);
std::vector<std::uint8_t> encode_png_rgba(const RenderedImage& image, const PerfTraceCallback& perf_trace);
void write_png_rgba(const RenderedImage& image, const std::string& path);
void write_png_rgba(const RenderedImage& image, const std::string& path, const PerfTraceCallback& perf_trace);

} // namespace pagecore
