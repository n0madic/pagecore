#include "event_loop.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace pagecore {
namespace {

constexpr double kFrameIntervalMs = 16.0;

void noop_timer_cb(uv_timer_t*)
{
}

} // namespace

EventLoop::EventLoop()
{
    if (uv_loop_init(&loop_) != 0) {
        throw std::runtime_error("failed to initialize libuv event loop");
    }
    uv_timer_init(&loop_, &wakeup_timer_);
    uv_timer_init(&loop_, &frame_timer_);
    wakeup_timer_.data = this;
    frame_timer_.data = this;
    origin_ns_ = uv_hrtime();
}

EventLoop::~EventLoop()
{
    shutdown();
}

std::chrono::milliseconds EventLoop::now() const
{
    return std::chrono::milliseconds{static_cast<std::int64_t>(now_ms())};
}

double EventLoop::now_ms() const
{
    return static_cast<double>(uv_hrtime() - origin_ns_) / 1e6;
}

void EventLoop::post(TaskSource source, Task task, bool readiness_relevant)
{
    if (shut_down_) {
        return;
    }
    tasks_.push_back(QueuedTask{next_seq_++, source, readiness_relevant, std::move(task)});
}

void EventLoop::set_timer_callback(TimerCallback callback)
{
    timer_callback_ = std::move(callback);
}

void EventLoop::timer_fired(uv_timer_t* handle)
{
    auto* entry = static_cast<TimerEntry*>(handle->data);
    EventLoop* self = entry->owner;
    const std::uint64_t id = entry->id;
    // Interval ticks are never readiness-relevant (they would keep readiness
    // pending forever); one-shot timers carry the relevance of their task kind.
    const bool relevant = entry->relevant && !entry->repeat;
    if (entry->repeat) {
        entry->due_ms = self->now_ms() + static_cast<double>(std::max<std::uint64_t>(1, entry->delay_ms));
    } else {
        self->timers_.erase(id);
        uv_close(reinterpret_cast<uv_handle_t*>(&entry->handle), close_timer_entry);
    }
    // Only the id crosses into the queued task; the JS-side registry resolves
    // it to a callback (and tolerates ids cancelled between fire and run).
    self->post(TaskSource::Timers, [self, id] {
        if (self->timer_callback_) {
            self->timer_callback_(id);
        }
    }, relevant);
}

void EventLoop::close_timer_entry(uv_handle_t* handle)
{
    delete static_cast<TimerEntry*>(handle->data);
}

void EventLoop::schedule_timer(std::uint64_t id, std::uint64_t delay_ms, bool repeat, bool readiness_relevant)
{
    if (shut_down_) {
        return;
    }
    cancel_timer(id);

    auto* entry = new TimerEntry{};
    entry->owner = this;
    entry->id = id;
    entry->delay_ms = delay_ms;
    entry->repeat = repeat;
    entry->relevant = readiness_relevant;
    entry->due_ms = now_ms() + static_cast<double>(delay_ms);
    entry->handle.data = entry;
    uv_timer_init(&loop_, &entry->handle);
    uv_timer_start(
        &entry->handle,
        timer_fired,
        delay_ms,
        repeat ? std::max<std::uint64_t>(1, delay_ms) : 0);
    timers_.emplace(id, entry);
}

void EventLoop::cancel_timer(std::uint64_t id)
{
    const auto found = timers_.find(id);
    if (found == timers_.end()) {
        return;
    }
    TimerEntry* entry = found->second;
    timers_.erase(found);
    uv_timer_stop(&entry->handle);
    uv_close(reinterpret_cast<uv_handle_t*>(&entry->handle), close_timer_entry);
}

void EventLoop::frame_timer_fired(uv_timer_t* handle)
{
    auto* self = static_cast<EventLoop*>(handle->data);
    self->frame_timer_armed_ = false;
    self->frame_due_ = true;
    self->last_frame_ms_ = self->now_ms();
}

void EventLoop::arm_frame_timer()
{
    if (frame_timer_armed_ || shut_down_) {
        return;
    }
    const double since_last_frame = now_ms() - last_frame_ms_;
    const double delay = std::max(0.0, kFrameIntervalMs - since_last_frame);
    frame_due_at_ms_ = now_ms() + delay;
    uv_timer_start(&frame_timer_, frame_timer_fired, static_cast<std::uint64_t>(std::llround(delay)), 0);
    frame_timer_armed_ = true;
}

void EventLoop::request_animation_frame(std::uint64_t id)
{
    if (shut_down_) {
        return;
    }
    pending_animation_frames_.push_back(id);
    arm_frame_timer();
}

void EventLoop::cancel_animation_frame(std::uint64_t id)
{
    const auto found = std::find(pending_animation_frames_.begin(), pending_animation_frames_.end(), id);
    if (found != pending_animation_frames_.end()) {
        pending_animation_frames_.erase(found);
    }
}

std::deque<std::uint64_t> EventLoop::take_due_animation_frames()
{
    frame_due_ = false;
    // Callbacks registered while the frame runs land in the NEXT frame, per
    // spec: the whole pending list is snapshotted and cleared up front.
    std::deque<std::uint64_t> due;
    due.swap(pending_animation_frames_);
    return due;
}

void EventLoop::poll()
{
    if (shut_down_) {
        return;
    }
    uv_run(&loop_, UV_RUN_NOWAIT);
}

bool EventLoop::run_one_task()
{
    if (tasks_.empty()) {
        return false;
    }
    QueuedTask task = std::move(tasks_.front());
    tasks_.pop_front();
    if (task.fn) {
        task.fn();
    }
    return true;
}

void EventLoop::wait_for_activity(std::chrono::milliseconds max_wait)
{
    if (shut_down_ || has_ready_task()) {
        return;
    }
    if (max_wait.count() <= 0) {
        poll();
        return;
    }
    uv_timer_start(&wakeup_timer_, noop_timer_cb, static_cast<std::uint64_t>(max_wait.count()), 0);
    uv_run(&loop_, UV_RUN_ONCE);
    uv_timer_stop(&wakeup_timer_);
}

void EventLoop::begin_external_work()
{
    ++external_work_;
}

void EventLoop::end_external_work()
{
    if (external_work_ > 0) {
        --external_work_;
    }
}

bool EventLoop::idle() const
{
    if (!tasks_.empty() || external_work_ > 0) {
        return false;
    }
    for (const auto& [id, entry] : timers_) {
        (void) id;
        if (!entry->repeat) {
            return false;
        }
    }
    return true;
}

std::optional<std::chrono::milliseconds> EventLoop::next_timer_delay() const
{
    const double current = now_ms();
    double nearest = -1.0;
    for (const auto& [id, entry] : timers_) {
        (void) id;
        const double delay = std::max(0.0, entry->due_ms - current);
        if (nearest < 0.0 || delay < nearest) {
            nearest = delay;
        }
    }
    if (frame_timer_armed_) {
        const double delay = std::max(0.0, frame_due_at_ms_ - current);
        if (nearest < 0.0 || delay < nearest) {
            nearest = delay;
        }
    }
    if (frame_due_ && !pending_animation_frames_.empty()) {
        nearest = 0.0;
    }
    if (nearest < 0.0) {
        return std::nullopt;
    }
    return std::chrono::milliseconds{static_cast<std::int64_t>(std::ceil(nearest))};
}

std::size_t EventLoop::relevant_pending_count(std::chrono::milliseconds horizon) const
{
    const double horizon_end = now_ms() + static_cast<double>(std::max<std::int64_t>(0, horizon.count()));
    std::size_t count = external_work_;
    for (const auto& task : tasks_) {
        if (task.relevant) {
            ++count;
        }
    }
    for (const auto& [id, entry] : timers_) {
        (void) id;
        if (entry->relevant && !entry->repeat && entry->due_ms <= horizon_end) {
            ++count;
        }
    }
    return count;
}

bool EventLoop::quiescent() const
{
    return tasks_.empty()
        && timers_.empty()
        && external_work_ == 0
        && pending_animation_frames_.empty()
        && !frame_due_;
}

void EventLoop::shutdown()
{
    if (shut_down_) {
        return;
    }
    shut_down_ = true;

    // Queued tasks capture only plain ids (never JSValues), so dropping them
    // wholesale is safe at any point of the runtime's teardown.
    tasks_.clear();
    pending_animation_frames_.clear();

    for (auto& [id, entry] : timers_) {
        (void) id;
        uv_timer_stop(&entry->handle);
        uv_close(reinterpret_cast<uv_handle_t*>(&entry->handle), close_timer_entry);
    }
    timers_.clear();

    uv_timer_stop(&wakeup_timer_);
    uv_close(reinterpret_cast<uv_handle_t*>(&wakeup_timer_), nullptr);
    uv_timer_stop(&frame_timer_);
    uv_close(reinterpret_cast<uv_handle_t*>(&frame_timer_), nullptr);

    // Drain the close callbacks; uv_loop_close requires every handle closed.
    while (uv_run(&loop_, UV_RUN_DEFAULT) != 0) {
    }
    uv_loop_close(&loop_);
}

} // namespace pagecore
