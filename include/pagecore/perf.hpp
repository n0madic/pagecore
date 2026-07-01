#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace pagecore {

enum class PerfPhase {
    SerializeHtml,
    SubresourceScan,
    Script,
    DomBridge,
    ResourceLoad,
    LitehtmlLoadHtml,
    LitehtmlLayout,
    ComputedStyle,
    Geometry,
    Raster,
    PngEncode,
};

struct PerfEvent {
    PerfPhase phase = PerfPhase::SerializeHtml;
    std::string name;
    long long elapsed_us = 0;
    std::uint64_t count = 0;

    std::optional<std::uint64_t> node_id;
    std::optional<std::uint64_t> mutation_version;
    std::optional<std::uint64_t> layout_mutation_version;
    std::optional<bool> styled_document_cache_hit;
    std::string styled_document_cache_reason;
    std::string layout_mutation_reason;
    std::string property;
    std::string url;
};

using PerfTraceCallback = std::function<void(const PerfEvent&)>;

inline std::string_view perf_phase_name(PerfPhase phase)
{
    switch (phase) {
    case PerfPhase::SerializeHtml:
        return "serialize_html";
    case PerfPhase::SubresourceScan:
        return "subresource_scan";
    case PerfPhase::Script:
        return "script";
    case PerfPhase::DomBridge:
        return "dom_bridge";
    case PerfPhase::ResourceLoad:
        return "resource_load";
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
    PerfEvent event)
{
    if (!callback) {
        return;
    }
    callback(event);
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
    emit_perf_trace(callback, PerfEvent{phase, std::string(name), elapsed_us, count});
}

} // namespace pagecore
