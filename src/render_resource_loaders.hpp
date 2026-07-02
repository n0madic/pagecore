#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cookie_jar.hpp"
#include "pagecore/perf.hpp"
#include "pagecore/render.hpp"
#include "pagecore/resource_loader.hpp"

namespace pagecore {

// Enforces the per-render external-resource budgets (max loads, bytes, and wall
// time from RenderOptions) around an inner loader. Requests that would exceed a
// budget are short-circuited to an empty, unsuccessful response and reported via
// a perf trace instead of hitting the network.
class BudgetedRenderResourceLoader final : public ResourceLoader {
public:
    using ResourceLoader::load;

    BudgetedRenderResourceLoader(
        std::shared_ptr<ResourceLoader> inner,
        RenderOptions options,
        PerfTraceCallback perf_trace);

    ResourceResponse load(const ResourceRequest& request) override;
    std::vector<ResourceResponse> load_all(
        const std::vector<ResourceRequest>& requests,
        BatchErrorMode mode = BatchErrorMode::FailFast) override;

private:
    std::optional<std::string> block_reason(std::size_t pending_loads = 0) const;
    ResourceResponse blocked_response(const ResourceRequest& request, const std::string& reason) const;
    void record_load(long long elapsed_us, std::uint64_t bytes, std::size_t loads);

    std::shared_ptr<ResourceLoader> inner_;
    std::optional<std::size_t> max_loads_;
    std::optional<std::size_t> max_bytes_;
    std::optional<std::chrono::milliseconds> max_time_;
    PerfTraceCallback perf_trace_;
    std::size_t load_count_ = 0;
    std::uint64_t loaded_bytes_ = 0;
    long long elapsed_us_ = 0;
};

// Restricts an inner loader to stylesheet requests, used by geometry reads that
// need the cascade but must not fetch images yet. Non-stylesheet requests are
// short-circuited to an empty response and reported via a perf trace.
class StylesheetOnlyResourceLoader final : public ResourceLoader {
public:
    using ResourceLoader::load;

    StylesheetOnlyResourceLoader(
        std::shared_ptr<ResourceLoader> inner,
        PerfTraceCallback perf_trace);

    ResourceResponse load(const ResourceRequest& request) override;
    std::vector<ResourceResponse> load_all(
        const std::vector<ResourceRequest>& requests,
        BatchErrorMode mode = BatchErrorMode::FailFast) override;

private:
    ResourceResponse skipped_response(const ResourceRequest& request) const;

    std::shared_ptr<ResourceLoader> inner_;
    PerfTraceCallback perf_trace_;
};

// Injects request cookies before delegating to an inner loader and stores any
// Set-Cookie headers from the response, honouring the document's cookie
// credentials mode. A null cookie jar makes this a transparent pass-through.
class CookieAwareResourceLoader final : public ResourceLoader {
public:
    using ResourceLoader::load;

    CookieAwareResourceLoader(
        std::shared_ptr<ResourceLoader> inner,
        CookieJar* cookie_jar,
        std::string document_url,
        CookieCredentials credentials);

    ResourceResponse load(const ResourceRequest& request) override;
    std::vector<ResourceResponse> load_all(
        const std::vector<ResourceRequest>& requests,
        BatchErrorMode mode = BatchErrorMode::FailFast) override;

private:
    ResourceRequest with_cookies(const ResourceRequest& request) const;
    void store_cookies(std::string_view request_url, const ResourceResponse& response) const;

    std::shared_ptr<ResourceLoader> inner_;
    CookieJar* cookie_jar_ = nullptr;
    std::string document_url_;
    CookieCredentials credentials_ = CookieCredentials::SameOrigin;
};

} // namespace pagecore
