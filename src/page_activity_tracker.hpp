#pragma once

#include "pagecore/dom.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace pagecore {

enum class PageActivityKind {
    XhrFetch,
    DynamicScript,
    DomResource,
    MutationObserver,
};

struct PageActivitySnapshot {
    std::size_t pending_xhr_fetch = 0;
    std::size_t pending_dynamic_scripts = 0;
    std::size_t pending_dom_resources = 0;
    std::size_t pending_relevant_timers = 0;
    std::size_t pending_mutation_observers = 0;
    std::uint64_t mutation_version = 0;
    std::chrono::milliseconds clock{0};
    std::chrono::milliseconds last_mutation_clock{0};
    bool load_fired = false;
};

PageActivityKind page_activity_kind_from_string(std::string_view value);

class PageActivityTracker {
public:
    void reset(std::uint64_t mutation_version);
    void mark_load_fired();
    void set_clock(std::chrono::milliseconds clock);
    void set_relevant_timers(std::size_t count);
    void begin(PageActivityKind kind);
    void end(PageActivityKind kind);
    void mark_mutation(std::uint64_t mutation_version);
    void sync_mutation_version(std::uint64_t mutation_version);

    PageActivitySnapshot snapshot() const;
    bool network_idle() const;
    bool dom_stable(std::chrono::milliseconds stable_window) const;
    bool mutation_observers_drained() const;

private:
    std::size_t pending_xhr_fetch_ = 0;
    std::size_t pending_dynamic_scripts_ = 0;
    std::size_t pending_dom_resources_ = 0;
    std::size_t pending_relevant_timers_ = 0;
    std::size_t pending_mutation_observers_ = 0;
    std::uint64_t mutation_version_ = 0;
    std::chrono::milliseconds clock_{0};
    std::chrono::milliseconds last_mutation_clock_{0};
    bool load_fired_ = false;
};

} // namespace pagecore
