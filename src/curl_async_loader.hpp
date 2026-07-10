#pragma once

// libuv-driven curl_multi_socket_action transfer engine: truly asynchronous
// HTTP(S) transfers multiplexed on the page's EventLoop. One instance per
// JsRuntime; single-threaded (everything runs on the page's thread).

#include "async_loader.hpp"
#include "curl_transfer.hpp"
#include "event_loop.hpp"

#include <curl/curl.h>
#include <uv.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace pagecore {

class CurlMultiAsyncLoader final : public AsyncResourceLoader {
public:
    // `loader` provides the policy and the CURLSH share handle; it must be the
    // same CurlResourceLoader the page uses for blocking loads so connections,
    // DNS entries, and TLS sessions are reused across both paths.
    CurlMultiAsyncLoader(
        EventLoop& loop,
        std::shared_ptr<CurlResourceLoader> loader,
        std::string user_agent);
    ~CurlMultiAsyncLoader() override;

    CurlMultiAsyncLoader(const CurlMultiAsyncLoader&) = delete;
    CurlMultiAsyncLoader& operator=(const CurlMultiAsyncLoader&) = delete;

    std::uint64_t start(const ResourceRequest& request, AsyncLoadCallback callback) override;
    void cancel(std::uint64_t id) override;

    // Aborts every in-flight transfer (no callbacks fire) and closes all libuv
    // handles. Must run before EventLoop::shutdown() so the loop can drain the
    // close callbacks. Idempotent; the destructor calls it too.
    void shutdown() override;

private:
    struct Transfer {
        std::uint64_t id = 0;
        CURL* handle = nullptr;
        ResourceRequest request;
        // Per-transfer policy snapshot: the socket-callback context points at
        // this copy for the whole transfer.
        ResourcePolicy policy;
        detail::CurlBody body;
        detail::CurlHeaders headers;
        detail::OpenSocketContext socket_context;
        detail::CurlHeaderList header_list;
        AsyncLoadCallback callback;
    };

    // One uv_poll_t per curl socket. CURL_POLL_REMOVE only stops the poll
    // (curl may re-add the same fd within the same batch); the handle itself
    // is closed when curl assigns a new socket or at shutdown.
    struct SocketPoll {
        uv_poll_t poll{};
        curl_socket_t fd = CURL_SOCKET_BAD;
        CurlMultiAsyncLoader* owner = nullptr;
    };

    static int socket_callback(CURL* easy, curl_socket_t socket, int action, void* userp, void* socketp);
    static int timer_callback(CURLM* multi, long timeout_ms, void* userp);
    static void poll_event(uv_poll_t* handle, int status, int events);
    static void timeout_event(uv_timer_t* handle);
    static void close_socket_poll(uv_handle_t* handle);
    static void close_timeout_timer(uv_handle_t* handle);

    void drive_socket_action(curl_socket_t socket, int event_flags);
    // Reaps CURLMSG_DONE transfers: builds the result in C++ and posts the
    // completion as a Networking task. Called after EVERY socket_action.
    void process_messages();
    void abort_transfer(Transfer& transfer);

    EventLoop* loop_;
    std::shared_ptr<CurlResourceLoader> loader_;
    std::string user_agent_;
    CURLM* multi_ = nullptr;
    uv_timer_t timeout_timer_{};
    std::uint64_t next_id_ = 1;
    std::unordered_map<std::uint64_t, std::unique_ptr<Transfer>> transfers_;
    // Non-network (data:/file:) loads resolve synchronously in start() but
    // deliver through a queued task; the id sits here until delivery so
    // cancel() can drop the completion like any other transfer.
    std::unordered_set<std::uint64_t> pending_immediate_;
    std::unordered_map<curl_socket_t, SocketPoll*> socket_polls_;
    // Live uv handles owned by this loader (timeout timer + socket polls).
    // shutdown() must drain their close callbacks before the loader's memory
    // goes away: the timer handle lives inside this object and libuv touches
    // closing handles until their close callback has run.
    int open_uv_handles_ = 0;
    bool shut_down_ = false;
};

} // namespace pagecore
