#pragma once

#include <cstdint>
#include <functional>
#include <string_view>

namespace pagecore {

enum class PerfPhase {
    SerializeHtml,
    SubresourceScan,
    LitehtmlLoadHtml,
    LitehtmlLayout,
    ComputedStyle,
    Geometry,
    Raster,
    PngEncode,
};

struct PerfEvent {
    PerfPhase phase = PerfPhase::SerializeHtml;
    std::string_view name;
    long long elapsed_us = 0;
    std::uint64_t count = 0;
};

using PerfTraceCallback = std::function<void(const PerfEvent&)>;

inline std::string_view perf_phase_name(PerfPhase phase)
{
    switch (phase) {
    case PerfPhase::SerializeHtml:
        return "serialize_html";
    case PerfPhase::SubresourceScan:
        return "subresource_scan";
    case PerfPhase::LitehtmlLoadHtml:
        return "litehtml_load_html";
    case PerfPhase::LitehtmlLayout:
        return "litehtml_layout";
    case PerfPhase::ComputedStyle:
        return "computed_style";
    case PerfPhase::Geometry:
        return "geometry";
    case PerfPhase::Raster:
        return "raster";
    case PerfPhase::PngEncode:
        return "png_encode";
    }
    return "unknown";
}

inline void emit_perf_trace(
    const PerfTraceCallback& callback,
    PerfPhase phase,
    std::string_view name,
    long long elapsed_us,
    std::uint64_t count = 0)
{
    if (!callback) {
        return;
    }
    callback(PerfEvent{phase, name, elapsed_us, count});
}

} // namespace pagecore
