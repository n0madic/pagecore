#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <unordered_map>

#include <uv.h>

namespace pagecore {

// HTML event-loop task sources. One task runs per turn; ordering across
// sources is global FIFO by enqueue sequence, which keeps the loop
// deterministic for tests while still separating sources for accounting.
enum class TaskSource {
    Timers,
    Networking,
    Misc,
};

// Single-threaded wrapper around an embedded uv_loop_t owned by the page's
// JsRuntime. Core invariant: libuv callbacks only enqueue tasks; JS executes
// exclusively from tasks the embedder takes via run_one_task(), never from
// inside uv_run().
class EventLoop {
public:
    using Task = std::function<void()>;
    // Invoked (from a queued Timers task) when a scheduled timer fires.
    using TimerCallback = std::function<void(std::uint64_t id)>;

    EventLoop();
    ~EventLoop();

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    uv_loop_t* uv() noexcept { return &loop_; }

    // Milliseconds since the loop was created (monotonic, wall-clock rate).
    std::chrono::milliseconds now() const;
    double now_ms() const;

    void post(TaskSource source, Task task, bool readiness_relevant = false);

    // The timer callback receives only the timer id; no JS values are retained
    // on the C++ side of the boundary.
    void set_timer_callback(TimerCallback callback);
    void schedule_timer(std::uint64_t id, std::uint64_t delay_ms, bool repeat, bool readiness_relevant);
    void cancel_timer(std::uint64_t id);

    // Animation-frame scheduling: ids accumulate until the frame timer fires
    // (~16ms cadence anchored to the previous frame); the embedder then drains
    // take_due_animation_frames() and runs one rendering phase.
    void request_animation_frame(std::uint64_t id);
    void cancel_animation_frame(std::uint64_t id);
    std::deque<std::uint64_t> take_due_animation_frames();
    bool animation_frame_due() const { return frame_due_; }

    // Runs pending libuv callbacks without blocking (UV_RUN_NOWAIT).
    void poll();

    // Executes at most one queued task (oldest across all sources).
    // Returns whether a task ran. Never re-enters libuv.
    bool run_one_task();

    bool has_ready_task() const { return !tasks_.empty(); }

    // Blocks inside uv_run(UV_RUN_ONCE) until something happens, bounded by
    // max_wait. Returns immediately when a task is already queued.
    void wait_for_activity(std::chrono::milliseconds max_wait);

    // Loaders bracket in-flight transfers so idle/readiness accounting can see
    // them without the loop knowing about loader internals.
    void begin_external_work();
    void end_external_work();
    std::size_t external_work() const { return external_work_; }

    // True when nothing relevant remains: no queued tasks, no non-interval
    // timers, no in-flight external work. Interval timers and animation frames
    // never block idleness (they would otherwise spin forever in real time).
    bool idle() const;

    // Delay until the nearest scheduled timer fires (including intervals and a
    // pending animation frame), or nullopt when none is armed.
    std::optional<std::chrono::milliseconds> next_timer_delay() const;

    // Readiness-relevant pending work: relevant queued tasks + relevant
    // non-interval timers due within `horizon` + in-flight external work.
    // The horizon mirrors the historical snapshot semantics: a relevant timer
    // scheduled far beyond the stability window must not hold readiness open.
    std::size_t relevant_pending_count(std::chrono::milliseconds horizon) const;

    // Provably quiescent: no queued tasks, no timers of any kind (including
    // intervals and a pending animation frame), no in-flight external work.
    // A quiescent loop cannot mutate the DOM again, so readiness treats the
    // DOM as trivially stable without waiting out the stability window.
    bool quiescent() const;

    // Cancels timers, drops queued tasks (safe: tasks capture only ids), closes
    // every handle and runs the loop to completion. Idempotent; called by the
    // destructor and by ~JsRuntime before the QuickJS context is freed.
    void shutdown();

private:
    struct QueuedTask {
        std::uint64_t seq = 0;
        TaskSource source = TaskSource::Misc;
        bool relevant = false;
        Task fn;
    };

    struct TimerEntry {
        uv_timer_t handle{};
        EventLoop* owner = nullptr;
        std::uint64_t id = 0;
        std::uint64_t delay_ms = 0;
        bool repeat = false;
        bool relevant = false;
        double due_ms = 0.0;
    };

    static void timer_fired(uv_timer_t* handle);
    static void frame_timer_fired(uv_timer_t* handle);
    static void close_timer_entry(uv_handle_t* handle);
    void arm_frame_timer();

    uv_loop_t loop_{};
    uv_timer_t wakeup_timer_{};
    uv_timer_t frame_timer_{};
    bool frame_timer_armed_ = false;
    bool frame_due_ = false;
    double last_frame_ms_ = 0.0;
    double frame_due_at_ms_ = 0.0;
    std::deque<std::uint64_t> pending_animation_frames_;

    std::uint64_t origin_ns_ = 0;
    std::uint64_t next_seq_ = 1;
    std::deque<QueuedTask> tasks_;
    std::unordered_map<std::uint64_t, TimerEntry*> timers_;
    TimerCallback timer_callback_;
    std::size_t external_work_ = 0;
    bool shut_down_ = false;
};

} // namespace pagecore
