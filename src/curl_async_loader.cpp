#include "curl_async_loader.hpp"

#include <stdexcept>
#include <utility>

namespace pagecore {

namespace {

// Same browser-like connection caps as the blocking load_all() batch path.
constexpr long kMaxTotalConnections = 8;
constexpr long kMaxHostConnections = 6;

} // namespace

CurlMultiAsyncLoader::CurlMultiAsyncLoader(
    EventLoop& loop,
    std::shared_ptr<CurlResourceLoader> loader,
    std::string user_agent)
    : loop_(&loop)
    , loader_(std::move(loader))
    , user_agent_(std::move(user_agent))
{
    if (!loader_) {
        throw std::runtime_error("CurlMultiAsyncLoader requires a CurlResourceLoader");
    }
    ensure_curl_global_init();
    multi_ = curl_multi_init();
    if (multi_ == nullptr) {
        throw std::runtime_error("failed to initialize libcurl multi handle");
    }
    curl_multi_setopt(multi_, CURLMOPT_SOCKETFUNCTION, socket_callback);
    curl_multi_setopt(multi_, CURLMOPT_SOCKETDATA, this);
    curl_multi_setopt(multi_, CURLMOPT_TIMERFUNCTION, timer_callback);
    curl_multi_setopt(multi_, CURLMOPT_TIMERDATA, this);
    curl_multi_setopt(multi_, CURLMOPT_MAX_TOTAL_CONNECTIONS, kMaxTotalConnections);
    curl_multi_setopt(multi_, CURLMOPT_MAX_HOST_CONNECTIONS, kMaxHostConnections);

    uv_timer_init(loop_->uv(), &timeout_timer_);
    timeout_timer_.data = this;
    open_uv_handles_ = 1;
}

CurlMultiAsyncLoader::~CurlMultiAsyncLoader()
{
    shutdown();
}

std::uint64_t CurlMultiAsyncLoader::start(const ResourceRequest& request, AsyncLoadCallback callback)
{
    if (shut_down_) {
        throw std::runtime_error("async loader is shut down");
    }

    // Snapshot the policy per transfer (mirrors the blocking paths): the
    // connect-time socket guard references this copy for the whole transfer.
    ResourcePolicy policy = loader_->policy();
    detail::enforce_request_policy(request, policy);

    const std::uint64_t id = next_id_++;
    const std::string scheme = detail::scheme_of(request.url);

    // Non-network schemes resolve synchronously here; the completion is still
    // delivered as a queued Networking task so JS ordering stays uniform.
    if (!detail::is_network_scheme(scheme)) {
        AsyncLoadResult result;
        try {
            if (detail::is_data_scheme(scheme)) {
                result.response = detail::load_data_url(request, policy);
            } else {
                const std::string mime_type = detail::infer_mime_type(request.url, request.kind);
                ResourceResponse response{
                    request.url,
                    detail::read_file(request.url, policy),
                    200,
                    mime_type,
                    request.kind,
                    false,
                    "OK",
                    detail::content_type_header(mime_type),
                };
                detail::enforce_response_policy(request, response, policy);
                result.response = std::move(response);
            }
        } catch (...) {
            result.error = std::current_exception();
        }
        auto boxed = std::make_shared<AsyncLoadResult>(std::move(result));
        // Deliver through pending_immediate_ so cancel() honours the contract
        // (a cancelled transfer's callback never runs) for these ids too.
        // Capturing `this` is safe: the EventLoop drops queued tasks in
        // shutdown() before the owning JsRuntime destroys this loader.
        pending_immediate_.insert(id);
        loop_->post(
            TaskSource::Networking,
            [this, id, callback = std::move(callback), boxed] {
                if (pending_immediate_.erase(id) == 0) {
                    return; // cancelled before delivery
                }
                callback(std::move(*boxed));
            },
            /*readiness_relevant=*/true);
        return id;
    }

    auto transfer = std::make_unique<Transfer>();
    transfer->id = id;
    transfer->request = request;
    transfer->policy = std::move(policy);
    transfer->callback = std::move(callback);

    CURL* handle = curl_easy_init();
    if (handle == nullptr) {
        throw ResourceError(ResourceErrorCode::Transport, request.url, "failed to initialize libcurl");
    }
    transfer->handle = handle;
    transfer->header_list = detail::configure_network_handle(
        handle,
        transfer->request,
        user_agent_,
        transfer->policy,
        static_cast<CURLSH*>(loader_->share_handle()),
        transfer->body,
        transfer->headers,
        transfer->socket_context);
    curl_easy_setopt(handle, CURLOPT_PRIVATE, transfer.get());

    const CURLMcode added = curl_multi_add_handle(multi_, handle);
    if (added != CURLM_OK) {
        curl_easy_cleanup(handle);
        throw ResourceError(
            ResourceErrorCode::Transport,
            request.url,
            std::string("failed to start transfer: ") + curl_multi_strerror(added));
    }

    // In-flight transfers hold the loop out of idle/quiescent until they land.
    loop_->begin_external_work();
    transfers_.emplace(id, std::move(transfer));
    return id;
}

void CurlMultiAsyncLoader::cancel(std::uint64_t id)
{
    if (pending_immediate_.erase(id) > 0) {
        return; // the queued delivery task will now drop the completion
    }
    const auto found = transfers_.find(id);
    if (found == transfers_.end()) {
        return;
    }
    abort_transfer(*found->second);
    transfers_.erase(found);
}

void CurlMultiAsyncLoader::abort_transfer(Transfer& transfer)
{
    if (transfer.handle != nullptr) {
        curl_multi_remove_handle(multi_, transfer.handle);
        curl_easy_cleanup(transfer.handle);
        transfer.handle = nullptr;
        loop_->end_external_work();
    }
}

int CurlMultiAsyncLoader::socket_callback(CURL*, curl_socket_t socket, int action, void* userp, void* socketp)
{
    auto* self = static_cast<CurlMultiAsyncLoader*>(userp);
    // During shutdown() the poll handles are already closed (and any cached
    // socketp may be freed); registering a new watcher here would leak a live
    // handle past the drain loop and hang EventLoop::shutdown(). Mirrors the
    // guard in timer_callback.
    if (self == nullptr || self->shut_down_) {
        return 0;
    }
    auto* entry = static_cast<SocketPoll*>(socketp);

    if (action == CURL_POLL_REMOVE) {
        // Stop watching only; do NOT close here. curl can re-register the same
        // fd later in this batch, and a closed-then-reused fd would race the
        // uv close callback. Cached entries are reaped in shutdown() or when
        // the fd is re-created below.
        if (entry != nullptr) {
            uv_poll_stop(&entry->poll);
        }
        return 0;
    }

    if (entry == nullptr) {
        const auto cached = self->socket_polls_.find(socket);
        if (cached != self->socket_polls_.end()) {
            entry = cached->second;
        } else {
            entry = new SocketPoll();
            entry->fd = socket;
            entry->owner = self;
            entry->poll.data = entry;
            if (uv_poll_init_socket(self->loop_->uv(), &entry->poll, socket) != 0) {
                delete entry;
                return -1;
            }
            ++self->open_uv_handles_;
            self->socket_polls_.emplace(socket, entry);
        }
        curl_multi_assign(self->multi_, socket, entry);
    }

    int events = 0;
    if (action != CURL_POLL_IN) {
        events |= UV_WRITABLE;
    }
    if (action != CURL_POLL_OUT) {
        events |= UV_READABLE;
    }
    uv_poll_start(&entry->poll, events, poll_event);
    return 0;
}

int CurlMultiAsyncLoader::timer_callback(CURLM*, long timeout_ms, void* userp)
{
    auto* self = static_cast<CurlMultiAsyncLoader*>(userp);
    if (self->shut_down_) {
        return 0;
    }
    if (timeout_ms < 0) {
        uv_timer_stop(&self->timeout_timer_);
        return 0;
    }
    // Even a 0ms timeout goes through the uv timer: it hoists the follow-up
    // curl_multi_socket_action out of curl's own stack, which forbids
    // re-entrant multi calls from inside its callbacks.
    uv_timer_start(&self->timeout_timer_, timeout_event, static_cast<std::uint64_t>(timeout_ms), 0);
    return 0;
}

void CurlMultiAsyncLoader::timeout_event(uv_timer_t* handle)
{
    auto* self = static_cast<CurlMultiAsyncLoader*>(handle->data);
    self->drive_socket_action(CURL_SOCKET_TIMEOUT, 0);
}

void CurlMultiAsyncLoader::poll_event(uv_poll_t* handle, int status, int events)
{
    auto* entry = static_cast<SocketPoll*>(handle->data);
    int flags = 0;
    if (status < 0) {
        flags = CURL_CSELECT_ERR;
    } else {
        if (events & UV_READABLE) {
            flags |= CURL_CSELECT_IN;
        }
        if (events & UV_WRITABLE) {
            flags |= CURL_CSELECT_OUT;
        }
    }
    entry->owner->drive_socket_action(entry->fd, flags);
}

void CurlMultiAsyncLoader::drive_socket_action(curl_socket_t socket, int event_flags)
{
    int running = 0;
    curl_multi_socket_action(multi_, socket, event_flags, &running);
    process_messages();
}

void CurlMultiAsyncLoader::process_messages()
{
    CURLMsg* message = nullptr;
    int in_queue = 0;
    while ((message = curl_multi_info_read(multi_, &in_queue)) != nullptr) {
        if (message->msg != CURLMSG_DONE) {
            continue;
        }
        CURL* handle = message->easy_handle;
        const CURLcode code = message->data.result;
        Transfer* transfer = nullptr;
        curl_easy_getinfo(handle, CURLINFO_PRIVATE, &transfer);
        if (transfer == nullptr) {
            curl_multi_remove_handle(multi_, handle);
            curl_easy_cleanup(handle);
            continue;
        }

        // Build the result here in C++ (no JS runs inside libuv callbacks);
        // the JS-facing completion is deferred to a queued Networking task.
        auto result = std::make_shared<AsyncLoadResult>();
        try {
            result->response = detail::build_network_response(
                handle,
                transfer->request,
                transfer->policy,
                transfer->body,
                transfer->headers,
                transfer->socket_context,
                code);
        } catch (...) {
            result->error = std::current_exception();
        }

        curl_multi_remove_handle(multi_, handle);
        curl_easy_cleanup(handle);
        transfer->handle = nullptr;
        loop_->end_external_work();

        AsyncLoadCallback callback = std::move(transfer->callback);
        transfers_.erase(transfer->id);
        loop_->post(
            TaskSource::Networking,
            [callback = std::move(callback), result] { callback(std::move(*result)); },
            /*readiness_relevant=*/true);
    }
}

void CurlMultiAsyncLoader::close_socket_poll(uv_handle_t* handle)
{
    auto* entry = static_cast<SocketPoll*>(handle->data);
    --entry->owner->open_uv_handles_;
    delete entry;
}

void CurlMultiAsyncLoader::close_timeout_timer(uv_handle_t* handle)
{
    --static_cast<CurlMultiAsyncLoader*>(handle->data)->open_uv_handles_;
}

void CurlMultiAsyncLoader::shutdown()
{
    if (shut_down_) {
        return;
    }
    shut_down_ = true;

    // Abort in-flight transfers without invoking their callbacks: teardown
    // happens while the owning JsRuntime is being destroyed, so no JS may run.
    for (auto& [id, transfer] : transfers_) {
        (void) id;
        abort_transfer(*transfer);
    }
    transfers_.clear();
    pending_immediate_.clear();

    for (auto& [fd, entry] : socket_polls_) {
        (void) fd;
        uv_poll_stop(&entry->poll);
        uv_close(reinterpret_cast<uv_handle_t*>(&entry->poll), close_socket_poll);
    }
    socket_polls_.clear();

    uv_timer_stop(&timeout_timer_);
    uv_close(reinterpret_cast<uv_handle_t*>(&timeout_timer_), close_timeout_timer);

    // Drain OUR close callbacks before returning: the timer handle is a member
    // of this object, so it must be fully closed before this memory can go
    // away. UV_RUN_NOWAIT never blocks; closing handles complete within an
    // iteration or two.
    while (open_uv_handles_ > 0) {
        uv_run(loop_->uv(), UV_RUN_NOWAIT);
    }

    if (multi_ != nullptr) {
        curl_multi_cleanup(multi_);
        multi_ = nullptr;
    }
}

} // namespace pagecore
