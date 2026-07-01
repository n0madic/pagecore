#include "page_activity_tracker.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace pagecore {

PageActivityKind page_activity_kind_from_string(std::string_view value)
{
    if (value == "xhr-fetch") return PageActivityKind::XhrFetch;
    if (value == "dynamic-script") return PageActivityKind::DynamicScript;
    if (value == "dom-resource") return PageActivityKind::DomResource;
    if (value == "mutation-observer") return PageActivityKind::MutationObserver;
    throw std::runtime_error("unknown page activity kind: " + std::string(value));
}

void PageActivityTracker::reset(std::uint64_t mutation_version)
{
    pending_xhr_fetch_ = 0;
    pending_dynamic_scripts_ = 0;
    pending_dom_resources_ = 0;
    pending_relevant_timers_ = 0;
    pending_mutation_observers_ = 0;
    mutation_version_ = mutation_version;
    clock_ = std::chrono::milliseconds{0};
    last_mutation_clock_ = clock_;
    load_fired_ = false;
}

void PageActivityTracker::mark_load_fired()
{
    load_fired_ = true;
}

void PageActivityTracker::set_clock(std::chrono::milliseconds clock)
{
    clock_ = std::max(clock_, clock);
}

void PageActivityTracker::set_relevant_timers(std::size_t count)
{
    pending_relevant_timers_ = count;
}

void PageActivityTracker::begin(PageActivityKind kind)
{
    switch (kind) {
    case PageActivityKind::XhrFetch:
        ++pending_xhr_fetch_;
        break;
    case PageActivityKind::DynamicScript:
        ++pending_dynamic_scripts_;
        break;
    case PageActivityKind::DomResource:
        ++pending_dom_resources_;
        break;
    case PageActivityKind::MutationObserver:
        ++pending_mutation_observers_;
        break;
    }
}

void PageActivityTracker::end(PageActivityKind kind)
{
    auto decrement = [](std::size_t& value) {
        if (value > 0) --value;
    };

    switch (kind) {
    case PageActivityKind::XhrFetch:
        decrement(pending_xhr_fetch_);
        break;
    case PageActivityKind::DynamicScript:
        decrement(pending_dynamic_scripts_);
        break;
    case PageActivityKind::DomResource:
        decrement(pending_dom_resources_);
        break;
    case PageActivityKind::MutationObserver:
        decrement(pending_mutation_observers_);
        break;
    }
}

void PageActivityTracker::mark_mutation(std::uint64_t mutation_version)
{
    mutation_version_ = mutation_version;
    last_mutation_clock_ = clock_;
}

void PageActivityTracker::sync_mutation_version(std::uint64_t mutation_version)
{
    if (mutation_version != mutation_version_) {
        mark_mutation(mutation_version);
    }
}

PageActivitySnapshot PageActivityTracker::snapshot() const
{
    return PageActivitySnapshot{
        pending_xhr_fetch_,
        pending_dynamic_scripts_,
        pending_dom_resources_,
        pending_relevant_timers_,
        pending_mutation_observers_,
        mutation_version_,
        clock_,
        last_mutation_clock_,
        load_fired_,
    };
}

bool PageActivityTracker::network_idle() const
{
    return pending_xhr_fetch_ == 0
        && pending_dynamic_scripts_ == 0
        && pending_dom_resources_ == 0
        && pending_relevant_timers_ == 0;
}

bool PageActivityTracker::dom_stable(std::chrono::milliseconds stable_window) const
{
    if (stable_window.count() <= 0) {
        return true;
    }
    return clock_ - last_mutation_clock_ >= stable_window;
}

bool PageActivityTracker::mutation_observers_drained() const
{
    return pending_mutation_observers_ == 0;
}

} // namespace pagecore
