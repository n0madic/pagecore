#pragma once

// Internal asynchronous resource-loading interface used by JsRuntime for
// fetch/XHR/dynamic scripts. Completions are ALWAYS delivered as Networking
// tasks on the page's EventLoop - never re-entrantly from start() - so JS
// observes a consistent task ordering regardless of the backing loader.

#include "pagecore/resource_loader.hpp"
#include "event_loop.hpp"

#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <unordered_set>
#include <utility>

namespace pagecore {

struct AsyncLoadResult {
    ResourceResponse response;
    // Non-null when the transfer failed; response is then meaningless.
    std::exception_ptr error;
};

using AsyncLoadCallback = std::function<void(AsyncLoadResult)>;

class AsyncResourceLoader {
public:
    virtual ~AsyncResourceLoader() = default;

    // Begins a transfer and returns its id. May throw synchronously on policy
    // violations (the JS layer converts that into an async rejection).
    virtual std::uint64_t start(const ResourceRequest& request, AsyncLoadCallback callback) = 0;

    // Cancels an in-flight transfer; its completion callback is never invoked.
    // Unknown ids are ignored.
    virtual void cancel(std::uint64_t id) = 0;

    // Aborts everything and releases loop resources ahead of destruction.
    // Called by ~JsRuntime BEFORE EventLoop::shutdown() so uv close callbacks
    // can still be drained. Default: nothing to release.
    virtual void shutdown() {}
};

// Adapter for any blocking ResourceLoader (MemoryResourceLoader, custom
// embedder loaders): the whole transfer runs inside one queued Networking
// task, blocking that single task. Completion therefore lands at the queue
// position where the task was started, preserving the deterministic ordering
// MemoryResourceLoader-backed tests rely on.
class TaskQueueAsyncLoader final : public AsyncResourceLoader {
public:
    TaskQueueAsyncLoader(EventLoop& loop, std::shared_ptr<ResourceLoader> loader)
        : loop_(&loop)
        , loader_(std::move(loader))
    {
    }

    std::uint64_t start(const ResourceRequest& request, AsyncLoadCallback callback) override
    {
        const std::uint64_t id = next_id_++;
        pending_.insert(id);
        // Capturing `this` is safe: the EventLoop drops all queued tasks in
        // shutdown() before the owning JsRuntime destroys this loader.
        loop_->post(
            TaskSource::Networking,
            [this, id, request, callback = std::move(callback)] {
                if (pending_.erase(id) == 0) {
                    return; // cancelled before the task ran
                }
                AsyncLoadResult result;
                try {
                    result.response = loader_->load(request);
                } catch (...) {
                    result.error = std::current_exception();
                }
                callback(std::move(result));
            },
            /*readiness_relevant=*/true);
        return id;
    }

    void cancel(std::uint64_t id) override
    {
        pending_.erase(id);
    }

private:
    EventLoop* loop_;
    std::shared_ptr<ResourceLoader> loader_;
    std::uint64_t next_id_ = 1;
    std::unordered_set<std::uint64_t> pending_;
};

} // namespace pagecore
