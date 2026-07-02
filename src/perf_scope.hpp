#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

#include "pagecore/perf.hpp"

namespace pagecore {

// RAII timer that emits a single PerfEvent when it goes out of scope, timing the
// enclosing block. The event's count/property/reason can be refined before the
// scope ends via set_count()/event().
class PerfScope final {
public:
    PerfScope(const PerfTraceCallback& callback, PerfPhase phase, std::string_view name, std::uint64_t count = 0)
        : callback_(callback)
        , event_{phase, std::string(name), 0, count}
        , start_(std::chrono::steady_clock::now())
    {
    }

    ~PerfScope()
    {
        const auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start_).count();
        try {
            event_.elapsed_us = elapsed_us;
            emit_perf_trace(callback_, event_);
        } catch (...) {
        }
    }

    void set_count(std::uint64_t count)
    {
        event_.count = count;
    }

    PerfEvent& event()
    {
        return event_;
    }

private:
    const PerfTraceCallback& callback_;
    PerfEvent event_;
    std::chrono::steady_clock::time_point start_;
};

} // namespace pagecore
