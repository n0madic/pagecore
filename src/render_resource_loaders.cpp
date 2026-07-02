#include "render_resource_loaders.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace pagecore {
namespace {

long long elapsed_us_since(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count();
}

} // namespace

BudgetedRenderResourceLoader::BudgetedRenderResourceLoader(
    std::shared_ptr<ResourceLoader> inner,
    RenderOptions options,
    PerfTraceCallback perf_trace)
    : inner_(std::move(inner))
    , max_loads_(options.max_external_resource_loads)
    , max_bytes_(options.max_external_resource_bytes)
    , max_time_(options.max_external_resource_time)
    , perf_trace_(std::move(perf_trace))
{
    if (!inner_) {
        throw std::runtime_error("render resource budget loader requires an inner loader");
    }
}

ResourceResponse BudgetedRenderResourceLoader::load(const ResourceRequest& request)
{
    if (auto reason = block_reason()) {
        return blocked_response(request, *reason);
    }

    const auto start = std::chrono::steady_clock::now();
    try {
        auto response = inner_->load(request);
        record_load(elapsed_us_since(start), response.body.size(), 1);
        return response;
    } catch (...) {
        record_load(elapsed_us_since(start), 0, 1);
        throw;
    }
}

std::vector<ResourceResponse> BudgetedRenderResourceLoader::load_all(
    const std::vector<ResourceRequest>& requests,
    BatchErrorMode mode)
{
    std::vector<ResourceResponse> responses(requests.size());
    std::vector<ResourceRequest> allowed_requests;
    std::vector<std::size_t> allowed_indices;
    allowed_requests.reserve(requests.size());
    allowed_indices.reserve(requests.size());

    for (std::size_t i = 0; i < requests.size(); ++i) {
        if (auto reason = block_reason(allowed_requests.size())) {
            responses[i] = blocked_response(requests[i], *reason);
            continue;
        }
        allowed_indices.push_back(i);
        allowed_requests.push_back(requests[i]);
    }

    if (allowed_requests.empty()) {
        return responses;
    }

    const auto start = std::chrono::steady_clock::now();
    std::vector<ResourceResponse> fetched;
    try {
        fetched = inner_->load_all(allowed_requests, mode);
    } catch (...) {
        record_load(elapsed_us_since(start), 0, allowed_requests.size());
        throw;
    }

    std::uint64_t bytes = 0;
    for (std::size_t i = 0; i < fetched.size() && i < allowed_indices.size(); ++i) {
        bytes += fetched[i].body.size();
        responses[allowed_indices[i]] = std::move(fetched[i]);
    }
    record_load(elapsed_us_since(start), bytes, fetched.size());
    return responses;
}

std::optional<std::string> BudgetedRenderResourceLoader::block_reason(std::size_t pending_loads) const
{
    if (max_loads_ && load_count_ + pending_loads >= *max_loads_) {
        return "budget:max_render_resource_loads";
    }
    if (max_bytes_ && loaded_bytes_ >= *max_bytes_) {
        return "budget:max_render_resource_bytes";
    }
    if (max_time_) {
        const auto max_us = std::chrono::duration_cast<std::chrono::microseconds>(*max_time_).count();
        if (elapsed_us_ >= max_us) {
            return "budget:max_render_resource_time_ms";
        }
    }
    return std::nullopt;
}

ResourceResponse BudgetedRenderResourceLoader::blocked_response(
    const ResourceRequest& request,
    const std::string& reason) const
{
    PerfEvent event{PerfPhase::ResourceLoad, "render_resource_blocked", 0, 0};
    event.property = resource_kind_name(request.kind);
    event.url = request.url;
    event.reason = reason;
    emit_perf_trace(perf_trace_, std::move(event));
    return ResourceResponse{request.url, {}, 0, {}, request.kind, false};
}

void BudgetedRenderResourceLoader::record_load(long long elapsed_us, std::uint64_t bytes, std::size_t loads)
{
    load_count_ += loads;
    loaded_bytes_ += bytes;
    elapsed_us_ += std::max<long long>(0, elapsed_us);
}

StylesheetOnlyResourceLoader::StylesheetOnlyResourceLoader(
    std::shared_ptr<ResourceLoader> inner,
    PerfTraceCallback perf_trace)
    : inner_(std::move(inner))
    , perf_trace_(std::move(perf_trace))
{
    if (!inner_) {
        throw std::runtime_error("stylesheet-only resource loader requires an inner loader");
    }
}

ResourceResponse StylesheetOnlyResourceLoader::load(const ResourceRequest& request)
{
    if (request.kind != ResourceKind::Stylesheet) {
        return skipped_response(request);
    }
    return inner_->load(request);
}

std::vector<ResourceResponse> StylesheetOnlyResourceLoader::load_all(
    const std::vector<ResourceRequest>& requests,
    BatchErrorMode mode)
{
    std::vector<ResourceResponse> responses(requests.size());
    std::vector<ResourceRequest> stylesheet_requests;
    std::vector<std::size_t> stylesheet_indices;
    stylesheet_requests.reserve(requests.size());
    stylesheet_indices.reserve(requests.size());

    for (std::size_t i = 0; i < requests.size(); ++i) {
        if (requests[i].kind != ResourceKind::Stylesheet) {
            responses[i] = skipped_response(requests[i]);
            continue;
        }
        stylesheet_indices.push_back(i);
        stylesheet_requests.push_back(requests[i]);
    }

    if (stylesheet_requests.empty()) {
        return responses;
    }

    const auto fetched = inner_->load_all(stylesheet_requests, mode);
    for (std::size_t i = 0; i < fetched.size() && i < stylesheet_indices.size(); ++i) {
        responses[stylesheet_indices[i]] = fetched[i];
    }
    return responses;
}

ResourceResponse StylesheetOnlyResourceLoader::skipped_response(const ResourceRequest& request) const
{
    PerfEvent event{PerfPhase::ResourceLoad, "render_resource_skipped", 0, 0};
    event.property = resource_kind_name(request.kind);
    event.url = request.url;
    event.reason = "geometry:stylesheets_only";
    emit_perf_trace(perf_trace_, std::move(event));
    return ResourceResponse{request.url, {}, 0, {}, request.kind, false};
}

CookieAwareResourceLoader::CookieAwareResourceLoader(
    std::shared_ptr<ResourceLoader> inner,
    CookieJar* cookie_jar,
    std::string document_url,
    CookieCredentials credentials)
    : inner_(std::move(inner))
    , cookie_jar_(cookie_jar)
    , document_url_(std::move(document_url))
    , credentials_(credentials)
{
    if (!inner_) {
        throw std::runtime_error("cookie-aware resource loader requires an inner loader");
    }
}

ResourceResponse CookieAwareResourceLoader::load(const ResourceRequest& request)
{
    ResourceRequest cookie_request = with_cookies(request);
    const std::string request_url = cookie_request.url;
    ResourceResponse response = inner_->load(cookie_request);
    store_cookies(request_url, response);
    return response;
}

std::vector<ResourceResponse> CookieAwareResourceLoader::load_all(
    const std::vector<ResourceRequest>& requests,
    BatchErrorMode mode)
{
    std::vector<ResourceRequest> cookie_requests;
    cookie_requests.reserve(requests.size());
    std::vector<std::string> request_urls;
    request_urls.reserve(requests.size());
    for (const auto& request : requests) {
        cookie_requests.push_back(with_cookies(request));
        request_urls.push_back(cookie_requests.back().url);
    }

    std::vector<ResourceResponse> responses = inner_->load_all(cookie_requests, mode);
    for (std::size_t i = 0; i < responses.size() && i < request_urls.size(); ++i) {
        store_cookies(request_urls[i], responses[i]);
    }
    return responses;
}

ResourceRequest CookieAwareResourceLoader::with_cookies(const ResourceRequest& request) const
{
    if (cookie_jar_ == nullptr) {
        return request;
    }
    return cookie_jar_->with_cookie_header(request, credentials_, document_url_);
}

void CookieAwareResourceLoader::store_cookies(std::string_view request_url, const ResourceResponse& response) const
{
    if (cookie_jar_ != nullptr) {
        cookie_jar_->store_from_response(request_url, response, credentials_, document_url_);
    }
}

} // namespace pagecore
